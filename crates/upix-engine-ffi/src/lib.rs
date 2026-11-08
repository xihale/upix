//! upix-engine-ffi — the workspace's ONLY C++ FFI island: the ncnn/Vulkan
//! engine (share/core engine subset) behind a hand-written C ABI, built by
//! build.rs via the cc crate.
//!
//! Porting contract: docs/rewrite/CONTRACT.md §7.
//!
//! Layering:
//!   - `ffi`: hand-written extern "C" declarations mirroring csrc/shim.h 1:1
//!     (no bindgen; the struct layouts there are the contract).
//!   - [`NcnnEngine`]: safe RAII wrapper implementing `upix_core::engine::Engine`.
//!
//! Threading mirrors the C++ engine: process paths take `&self` (the engine
//! synchronizes internally, as its `std::async` process_batch requires),
//! lifecycle and setters take `&mut self`.

use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::path::Path;
use std::ptr;
use std::sync::atomic::AtomicBool;
use std::sync::Once;

use upix_core::engine::{
    DeviceCapability, DeviceType, Engine, EngineError, GpuTileTaskU8, ProcessingProgress,
    TileConfig, TileRegion,
};

/// Raw declarations — byte-layout twins of `csrc/shim.h`.
pub mod ffi {
    use std::os::raw::{c_char, c_int, c_void};

    /// Opaque `std::atomic<bool>` — layout-compatible with Rust's `AtomicBool`
    /// on the targets we build (lock-free, 1 byte, align 1).
    #[repr(C)]
    pub struct CAtomicBool {
        _data: u8,
    }

    pub const UPIX_DEVICE_CPU: i32 = 0;
    pub const UPIX_DEVICE_GPU_VULKAN: i32 = 1;

    #[repr(C)]
    pub struct CDevice {
        pub dtype: i32,
        pub name: [c_char; 256],
        pub compute_units: i32,
        pub memory_mb: u64,
        pub benchmark_score: f32,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct CTileRegion {
        pub x: u32,
        pub y: u32,
        pub width: u32,
        pub height: u32,
        pub overlap: u32,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct CTileConfig {
        pub tile_size: i32,
        pub overlap: i32,
        pub batch_size: i32,
    }

    /// Mirrors `upix::ProcessingProgress` exactly.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct CProgress {
        pub total_tiles: i32,
        pub completed_tiles: i32,
        pub current_tile_x: u32,
        pub current_tile_y: u32,
    }

    /// Mirrors `upix::GPUTileTaskU8` with regions vector + std::function
    /// flattened into pointer array + fn ptr/ctx.
    #[repr(C)]
    pub struct CGpuTileTask {
        pub input_data: *const u8,
        pub output_data: *mut u8,
        pub input_width: i32,
        pub input_height: i32,
        pub channels: i32,
        pub regions: *const CTileRegion,
        pub region_count: i32,
        pub config: CTileConfig,
        pub progress_callback: Option<ProgressCb>,
        pub progress_ctx: *mut c_void,
    }

    /// `{code, message}`; code == 0 = success, 199 = unexpected C++ exception
    /// caught at the boundary, otherwise the engine's EngineError code.
    #[repr(C)]
    pub struct CStatus {
        pub code: i32,
        pub message: [u8; 512],
    }

    #[repr(C)]
    pub struct CEngine {
        _opaque: [u8; 0],
    }

    pub type ProgressCb = unsafe extern "C" fn(progress: *const CProgress, ctx: *mut c_void);

    unsafe extern "C" {
        pub fn upix_engine_set_spirv_pack(data: *const u8, size: usize);
        pub fn upix_engine_create() -> *mut CEngine;
        pub fn upix_engine_destroy(engine: *mut CEngine);
        pub fn upix_engine_initialize(engine: *mut CEngine, preferred: c_int) -> CStatus;
        pub fn upix_engine_list_devices(
            engine: *mut CEngine,
            out: *mut CDevice,
            cap: i32,
            out_len: *mut i32,
        ) -> CStatus;
        pub fn upix_engine_load_model(
            engine: *mut CEngine,
            param_path: *const c_char,
            bin_path: *const c_char,
        ) -> CStatus;
        pub fn upix_engine_unload_model(engine: *mut CEngine);
        pub fn upix_engine_process(
            engine: *mut CEngine,
            input: *const f32,
            output: *mut f32,
            width: i32,
            height: i32,
            channels: i32,
            cancel_flag: *const CAtomicBool,
        ) -> CStatus;
        pub fn upix_engine_process_u8(
            engine: *mut CEngine,
            input: *const u8,
            output: *mut u8,
            width: i32,
            height: i32,
            channels: i32,
            cancel_flag: *const CAtomicBool,
        ) -> CStatus;
        pub fn upix_engine_process_batch(
            engine: *mut CEngine,
            inputs: *const *const f32,
            outputs: *const *mut f32,
            count: i32,
            width: i32,
            height: i32,
            channels: i32,
            cancel_flag: *const CAtomicBool,
        ) -> CStatus;
        pub fn upix_engine_process_tiles_gpu(
            engine: *mut CEngine,
            input_data: *const f32,
            output_data: *mut f32,
            input_width: i32,
            input_height: i32,
            channels: i32,
            regions: *const CTileRegion,
            region_count: i32,
            config: CTileConfig,
            progress_cb: Option<ProgressCb>,
            progress_ctx: *mut c_void,
            cancel_flag: *const CAtomicBool,
        ) -> CStatus;
        pub fn upix_engine_process_tiles_gpu_u8(
            engine: *mut CEngine,
            input_data: *const u8,
            output_data: *mut u8,
            input_width: i32,
            input_height: i32,
            channels: i32,
            regions: *const CTileRegion,
            region_count: i32,
            config: CTileConfig,
            progress_cb: Option<ProgressCb>,
            progress_ctx: *mut c_void,
            cancel_flag: *const CAtomicBool,
        ) -> CStatus;
        pub fn upix_engine_process_tiles_gpu_u8_queue(
            engine: *mut CEngine,
            tasks: *const CGpuTileTask,
            task_count: i32,
            cancel_flag: *const CAtomicBool,
        ) -> CStatus;
        pub fn upix_engine_debug_blob_scan(
            engine: *mut CEngine,
            input: *const u8,
            width: i32,
            height: i32,
            channels: i32,
            buf: *mut c_char,
            cap: usize,
        ) -> CStatus;
        pub fn upix_engine_benchmark(engine: *mut CEngine, test_tile_size: i32) -> f32;
        pub fn upix_engine_current_device(engine: *mut CEngine) -> i32;
        pub fn upix_engine_is_initialized(engine: *mut CEngine) -> i32;
        pub fn upix_engine_model_name(engine: *mut CEngine, buf: *mut c_char, cap: usize);
        pub fn upix_engine_get_scale_factor(engine: *mut CEngine) -> i32;
        pub fn upix_engine_scale_factor_detected(engine: *mut CEngine) -> i32;
        pub fn upix_engine_model_input_channels(engine: *mut CEngine) -> i32;
        pub fn upix_engine_set_scale_factor(engine: *mut CEngine, scale: i32);
        pub fn upix_engine_set_inference_threads(engine: *mut CEngine, threads: i32);
        pub fn upix_engine_get_recommended_tile_size(engine: *mut CEngine) -> i32;
        pub fn upix_engine_get_device_memory_budget_mb(engine: *mut CEngine) -> u64;
        pub fn upix_engine_get_memory_info(
            engine: *mut CEngine,
            total_mb: *mut u64,
            available_mb: *mut u64,
        );
        pub fn upix_engine_hardware_id(engine: *mut CEngine, buf: *mut c_char, cap: usize);
        pub fn upix_engine_supports_gpu_tile_pipeline(engine: *mut CEngine) -> i32;
        pub fn upix_engine_supports_gpu_tile_pipeline_u8(engine: *mut CEngine) -> i32;
        pub fn upix_engine_supports_gpu_tile_pipeline_u8_queue(engine: *mut CEngine) -> i32;
    }
}

use ffi::{
    CAtomicBool, CDevice, CGpuTileTask, CEngine, CProgress, CStatus, CTileConfig, CTileRegion,
    UPIX_DEVICE_CPU, UPIX_DEVICE_GPU_VULKAN,
};

/// Injects the precompiled SPIR-V pack into the C++ side. Idempotent and
/// thread-safe; must happen before the first GPU initialize (done eagerly by
/// [`NcnnEngine::new`]). The pack is baked into this crate
/// (`csrc/spirv_pack.bin`), replacing the retired `.S`/`.incbin` embedding.
pub fn init_spirv_pack() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        const PACK: &[u8] = include_bytes!("../../../share/core/spirv_pack.bin");
        unsafe { ffi::upix_engine_set_spirv_pack(PACK.as_ptr(), PACK.len()) };
    });
}

/// FFI boundary catch-all code: a C++ exception escaped engine-internal scope
/// and was converted by the shim. Never originates from the engine itself.
pub const FFI_EXCEPTION_CODE: i32 = 199;

fn status_message(status: &CStatus) -> String {
    let len = status.message.iter().position(|&b| b == 0).unwrap_or(512);
    String::from_utf8_lossy(&status.message[..len]).into_owned()
}

fn status_into_result(status: CStatus) -> Result<(), EngineError> {
    if status.code == 0 {
        return Ok(());
    }
    Err(EngineError::new(status.code, status_message(&status)))
}

fn path_cstring(path: &Path) -> Result<CString, EngineError> {
    CString::new(path.as_os_str().as_encoded_bytes())
        .map_err(|_| EngineError::new(2, "model path contains NUL byte"))
}

fn cancel_ptr(cancel: &AtomicBool) -> *const CAtomicBool {
    ptr::from_ref(cancel).cast::<CAtomicBool>()
}

/// Reads a NUL-terminated buffer the shim wrote into an owned String.
unsafe fn read_c_string(buf: &[c_char]) -> String {
    let bytes: Vec<u8> = buf.iter().take_while(|&&b| b != 0).map(|&b| b as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

/// Trampoline: C progress fn-ptr/ctx → Rust closure. `ctx` points at an
/// `Option<&dyn Fn>` slot owned by the FFI caller's frame (see
/// [`progress_ctx`]); it is only dereferenced for the duration of the
/// synchronous C call.
unsafe extern "C" fn progress_trampoline(progress: *const CProgress, ctx: *mut c_void) {
    unsafe {
        if progress.is_null() || ctx.is_null() {
            return;
        }
        // ctx points at an `Option<&dyn Fn>` slot owned by the FFI caller's
        // frame, which outlives the synchronous C++ call (see progress_ctx).
        let slot = &*(ctx as *const Option<&dyn Fn(&ProcessingProgress)>);
        let Some(f) = slot.as_ref() else {
            return;
        };
        let p = *progress;
        f(&ProcessingProgress {
            total_tiles: p.total_tiles,
            completed_tiles: p.completed_tiles,
            current_tile_x: p.current_tile_x,
            current_tile_y: p.current_tile_y,
        });
    }
}

/// FFI fn-ptr + ctx for a progress closure stored in the caller's frame. The
/// returned ctx borrows `slot` and is valid exactly as long as `slot` lives —
/// the C++ tile pipelines invoke the callback on the calling thread only, and
/// only during the synchronous call the ctx is passed into (matching the
/// unbounded `&dyn Fn` in the Engine trait).
fn progress_ctx(
    slot: &Option<&dyn Fn(&ProcessingProgress)>,
) -> (Option<ffi::ProgressCb>, *mut c_void) {
    let tramp: unsafe extern "C" fn(*const CProgress, *mut c_void) = progress_trampoline;
    (
        slot.is_some().then_some(tramp),
        ptr::from_ref(slot).cast::<c_void>().cast_mut(),
    )
}

fn to_c_config(config: TileConfig) -> CTileConfig {
    CTileConfig {
        tile_size: config.tile_size,
        overlap: config.overlap,
        batch_size: config.batch_size,
    }
}

fn to_c_regions(regions: &[TileRegion]) -> Vec<CTileRegion> {
    regions
        .iter()
        .map(|r| CTileRegion {
            x: r.x,
            y: r.y,
            width: r.width,
            height: r.height,
            overlap: r.overlap,
        })
        .collect()
}

/// Safe RAII wrapper over the C++ `upix::NCNNEngineImpl`.
pub struct NcnnEngine {
    inner: ptr::NonNull<CEngine>,
}

// The C++ engine is internally synchronized (mutex-guarded inference state;
// its process_batch drives tiles through concurrent `process` calls), so a
// shared handle is sound on any thread — same as the C++ class.
unsafe impl Send for NcnnEngine {}
unsafe impl Sync for NcnnEngine {}

impl NcnnEngine {
    /// Creates the engine and eagerly injects the SPIR-V pack (first caller wins).
    pub fn new() -> Result<Self, EngineError> {
        init_spirv_pack();
        let raw = unsafe { ffi::upix_engine_create() };
        ptr::NonNull::new(raw)
            .map(|inner| Self { inner })
            .ok_or_else(|| EngineError::new(1, "engine create failed (C++ exception)"))
    }

    fn raw(&self) -> *mut CEngine {
        self.inner.as_ptr()
    }

    /// Device enumeration with capability snapshot.
    pub fn list_devices(&self) -> Vec<DeviceCapability> {
        let mut count: i32 = 0;
        unsafe {
            let st = ffi::upix_engine_list_devices(self.raw(), ptr::null_mut(), 0, &mut count);
            if st.code != 0 || count <= 0 {
                return Vec::new();
            }
            let mut devices = Vec::<CDevice>::with_capacity(count as usize);
            let mut written: i32 = 0;
            let st = ffi::upix_engine_list_devices(
                self.raw(),
                devices.as_mut_ptr(),
                count,
                &mut written,
            );
            if st.code != 0 {
                return Vec::new();
            }
            devices.set_len(written.max(0) as usize);
            devices
                .into_iter()
                .map(|d| DeviceCapability {
                    dtype: if d.dtype == UPIX_DEVICE_GPU_VULKAN {
                        DeviceType::GpuVulkan
                    } else {
                        DeviceType::Cpu
                    },
                    name: read_c_string(&d.name),
                    compute_units: d.compute_units,
                    memory_mb: d.memory_mb,
                    benchmark_score: d.benchmark_score,
                })
                .collect()
        }
    }

    /// Diagnostic: per-blob value ranges of one whole-image inference.
    pub fn debug_blob_scan(&self, input: &[u8], width: i32, height: i32, channels: i32) -> String {
        // 1 MiB is generous for the per-blob report; the shim truncates.
        let mut buf = vec![0 as c_char; 1 << 20];
        unsafe {
            let st = ffi::upix_engine_debug_blob_scan(
                self.raw(),
                input.as_ptr(),
                width,
                height,
                channels,
                buf.as_mut_ptr(),
                buf.len(),
            );
            if st.code == FFI_EXCEPTION_CODE {
                return format!("blobscan failed: {}", status_message(&st));
            }
            // Engine-reported statuses carry their own message in st, but the
            // report buffer is the primary payload on success (code 0).
            read_c_string(&buf)
        }
    }

    /// Stable hardware identity (auto-tile benchmark cache key).
    pub fn hardware_id(&self) -> String {
        let mut buf = [0 as c_char; 256];
        unsafe {
            ffi::upix_engine_hardware_id(self.raw(), buf.as_mut_ptr(), buf.len());
            read_c_string(&buf)
        }
    }

    /// Name of the currently loaded model ("" when none).
    pub fn model_name(&self) -> String {
        let mut buf = [0 as c_char; 256];
        unsafe {
            ffi::upix_engine_model_name(self.raw(), buf.as_mut_ptr(), buf.len());
            read_c_string(&buf)
        }
    }
}

impl Drop for NcnnEngine {
    fn drop(&mut self) {
        unsafe { ffi::upix_engine_destroy(self.raw()) };
    }
}

impl Engine for NcnnEngine {
    fn initialize(&mut self, preferred: DeviceType) -> Result<(), EngineError> {
        let pref = match preferred {
            DeviceType::GpuVulkan => UPIX_DEVICE_GPU_VULKAN,
            DeviceType::Cpu => UPIX_DEVICE_CPU,
        };
        let st = unsafe { ffi::upix_engine_initialize(self.raw(), pref) };
        status_into_result(st)
    }

    fn list_devices(&self) -> Vec<DeviceCapability> {
        NcnnEngine::list_devices(self)
    }

    fn debug_blob_scan(&self, input: &[u8], width: i32, height: i32, channels: i32) -> String {
        NcnnEngine::debug_blob_scan(self, input, width, height, channels)
    }

    fn load_model(&mut self, param_path: &Path, bin_path: &Path) -> Result<(), EngineError> {
        let param = path_cstring(param_path)?;
        let bin = path_cstring(bin_path)?;
        let st = unsafe { ffi::upix_engine_load_model(self.raw(), param.as_ptr(), bin.as_ptr()) };
        status_into_result(st)
    }

    fn process(
        &self,
        input: &[f32],
        output: &mut [f32],
        width: i32,
        height: i32,
        channels: i32,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        let st = unsafe {
            ffi::upix_engine_process(
                self.raw(),
                input.as_ptr(),
                output.as_mut_ptr(),
                width,
                height,
                channels,
                cancel_ptr(cancel),
            )
        };
        status_into_result(st)
    }

    fn process_u8(
        &self,
        input: &[u8],
        output: &mut [u8],
        width: i32,
        height: i32,
        channels: i32,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        let st = unsafe {
            ffi::upix_engine_process_u8(
                self.raw(),
                input.as_ptr(),
                output.as_mut_ptr(),
                width,
                height,
                channels,
                cancel_ptr(cancel),
            )
        };
        status_into_result(st)
    }

    fn process_batch(
        &self,
        inputs: &[&[f32]],
        outputs: &mut [&mut [f32]],
        width: i32,
        height: i32,
        channels: i32,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        if inputs.len() != outputs.len() {
            return Err(EngineError::new(100, "Batch input/output size mismatch"));
        }
        let in_ptrs: Vec<*const f32> = inputs.iter().map(|s| s.as_ptr()).collect();
        let out_ptrs: Vec<*mut f32> = outputs.iter_mut().map(|s| s.as_mut_ptr()).collect();
        let st = unsafe {
            ffi::upix_engine_process_batch(
                self.raw(),
                in_ptrs.as_ptr(),
                out_ptrs.as_ptr(),
                in_ptrs.len() as i32,
                width,
                height,
                channels,
                cancel_ptr(cancel),
            )
        };
        status_into_result(st)
    }

    fn benchmark(&self, test_tile_size: i32) -> f32 {
        unsafe { ffi::upix_engine_benchmark(self.raw(), test_tile_size) }
    }

    fn unload_model(&mut self) {
        unsafe { ffi::upix_engine_unload_model(self.raw()) };
    }

    fn current_device(&self) -> DeviceType {
        if unsafe { ffi::upix_engine_current_device(self.raw()) } == UPIX_DEVICE_GPU_VULKAN {
            DeviceType::GpuVulkan
        } else {
            DeviceType::Cpu
        }
    }

    fn is_initialized(&self) -> bool {
        unsafe { ffi::upix_engine_is_initialized(self.raw()) != 0 }
    }

    fn model_name(&self) -> String {
        NcnnEngine::model_name(self)
    }

    fn get_scale_factor(&self) -> i32 {
        unsafe { ffi::upix_engine_get_scale_factor(self.raw()) }
    }

    fn scale_factor_detected(&self) -> bool {
        unsafe { ffi::upix_engine_scale_factor_detected(self.raw()) != 0 }
    }

    fn model_input_channels(&self) -> i32 {
        unsafe { ffi::upix_engine_model_input_channels(self.raw()) }
    }

    fn set_scale_factor(&mut self, scale: i32) {
        unsafe { ffi::upix_engine_set_scale_factor(self.raw(), scale) };
    }

    fn set_inference_threads(&mut self, threads: i32) {
        unsafe { ffi::upix_engine_set_inference_threads(self.raw(), threads) };
    }

    fn get_recommended_tile_size(&self) -> i32 {
        unsafe { ffi::upix_engine_get_recommended_tile_size(self.raw()) }
    }

    fn get_device_memory_budget_mb(&self) -> u64 {
        unsafe { ffi::upix_engine_get_device_memory_budget_mb(self.raw()) }
    }

    fn get_memory_info(&self) -> (u64, u64) {
        let (mut total, mut avail) = (0u64, 0u64);
        unsafe { ffi::upix_engine_get_memory_info(self.raw(), &mut total, &mut avail) };
        (total, avail)
    }

    fn hardware_id(&self) -> String {
        NcnnEngine::hardware_id(self)
    }

    fn supports_gpu_tile_pipeline(&self) -> bool {
        unsafe { ffi::upix_engine_supports_gpu_tile_pipeline(self.raw()) != 0 }
    }

    fn supports_gpu_tile_pipeline_u8(&self) -> bool {
        unsafe { ffi::upix_engine_supports_gpu_tile_pipeline_u8(self.raw()) != 0 }
    }

    fn supports_gpu_tile_pipeline_u8_queue(&self) -> bool {
        unsafe { ffi::upix_engine_supports_gpu_tile_pipeline_u8_queue(self.raw()) != 0 }
    }

    fn process_tiles_gpu(
        &self,
        input: &[f32],
        output: &mut [f32],
        input_width: i32,
        input_height: i32,
        channels: i32,
        regions: &[TileRegion],
        config: TileConfig,
        progress: Option<&dyn Fn(&ProcessingProgress)>,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        let regions_c = to_c_regions(regions);
        let progress_slot = progress;
        let (cb, ctx) = progress_ctx(&progress_slot);
        let st = unsafe {
            ffi::upix_engine_process_tiles_gpu(
                self.raw(),
                input.as_ptr(),
                output.as_mut_ptr(),
                input_width,
                input_height,
                channels,
                regions_c.as_ptr(),
                regions_c.len() as i32,
                to_c_config(config),
                cb,
                ctx,
                cancel_ptr(cancel),
            )
        };
        status_into_result(st)
    }

    fn process_tiles_gpu_u8(
        &self,
        input: &[u8],
        output: &mut [u8],
        input_width: i32,
        input_height: i32,
        channels: i32,
        regions: &[TileRegion],
        config: TileConfig,
        progress: Option<&dyn Fn(&ProcessingProgress)>,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        let regions_c = to_c_regions(regions);
        let progress_slot = progress;
        let (cb, ctx) = progress_ctx(&progress_slot);
        let st = unsafe {
            ffi::upix_engine_process_tiles_gpu_u8(
                self.raw(),
                input.as_ptr(),
                output.as_mut_ptr(),
                input_width,
                input_height,
                channels,
                regions_c.as_ptr(),
                regions_c.len() as i32,
                to_c_config(config),
                cb,
                ctx,
                cancel_ptr(cancel),
            )
        };
        status_into_result(st)
    }

    fn process_tiles_gpu_u8_queue(
        &self,
        tasks: &mut [GpuTileTaskU8<'_>],
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        // The C++ task struct carries a regions VECTOR pointer, so each task's
        // region slice is converted into an owned Vec that outlives the call.
        let region_backing: Vec<Vec<CTileRegion>> =
            tasks.iter().map(|t| to_c_regions(t.regions)).collect();
        let mut tasks_c: Vec<CGpuTileTask> = Vec::with_capacity(tasks.len());
        for (i, task) in tasks.iter_mut().enumerate() {
            // ctx points at the caller-owned `progress` field (Option<&dyn Fn>)
            // inside `tasks` — it outlives the synchronous C++ call below.
            let cb = task.progress.is_some().then_some(progress_trampoline as unsafe extern "C" fn(*const CProgress, *mut c_void));
            let ctx = ptr::from_ref(&task.progress).cast::<c_void>().cast_mut();
            tasks_c.push(CGpuTileTask {
                input_data: task.input.as_ptr(),
                output_data: task.output.as_mut_ptr(),
                input_width: task.input_width,
                input_height: task.input_height,
                channels: task.channels,
                regions: region_backing[i].as_ptr(),
                region_count: region_backing[i].len() as i32,
                config: to_c_config(task.config),
                progress_callback: cb,
                progress_ctx: ctx,
            });
        }
        let st = unsafe {
            ffi::upix_engine_process_tiles_gpu_u8_queue(
                self.raw(),
                tasks_c.as_ptr(),
                tasks_c.len() as i32,
                cancel_ptr(cancel),
            )
        };
        status_into_result(st)
    }
}
