//! Engine abstraction — the single inference-backend contract for the whole
//! workspace. Rust mirror of `share/core/engine.hpp` + `share/core/tile_types.hpp`.
//!
//! Porting rule: semantics (including error codes and the documented device
//! quirks) must match the C++ original. The ncnn implementation behind
//! `upix-engine-ffi` is the only production backend; `upix-core` mocks this
//! trait for pipeline tests.

use std::path::Path;
use std::sync::atomic::AtomicBool;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceType {
    Cpu,
    GpuVulkan,
}

#[derive(Debug, Clone)]
pub struct DeviceCapability {
    pub dtype: DeviceType,
    pub name: String,
    pub compute_units: i32,
    pub memory_mb: u64,
    pub benchmark_score: f32,
}

/// Error code reserved for "allocation refused by preflight or the device ran
/// out of memory mid-run". Callers use [`is_oom_error`] to trigger the
/// shrink / fallback ladder instead of failing the whole job.
pub const ERR_OUT_OF_MEMORY: i32 = 230;

#[derive(Debug, Clone)]
pub struct EngineError {
    pub code: i32,
    pub message: String,
}

impl EngineError {
    pub fn new(code: i32, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }
}

pub fn is_oom_error(e: &EngineError) -> bool {
    e.code == ERR_OUT_OF_MEMORY || e.message.contains("out of memory")
}

/// `share/core/tile_types.hpp::TileRegion`
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TileRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub overlap: u32,
}

/// `share/core/tile_types.hpp::TileConfig`
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TileConfig {
    pub tile_size: i32,
    pub overlap: i32,
    pub batch_size: i32,
}

impl Default for TileConfig {
    fn default() -> Self {
        Self { tile_size: 256, overlap: 16, batch_size: 1 }
    }
}

/// `share/core/tile_types.hpp::ProcessingProgress`
#[derive(Debug, Clone, Copy, Default)]
pub struct ProcessingProgress {
    pub total_tiles: i32,
    pub completed_tiles: i32,
    pub current_tile_x: u32,
    pub current_tile_y: u32,
}

impl ProcessingProgress {
    pub fn percentage(&self) -> f32 {
        if self.total_tiles == 0 {
            return 0.0;
        }
        self.completed_tiles as f32 / self.total_tiles as f32 * 100.0
    }
}

/// `share/core/engine.hpp::GPUTileTaskU8` — one whole-image u8 GPU tile task.
/// `input`/`output` are full-image interleaved buffers (`input_width * input_height * channels`).
pub struct GpuTileTaskU8<'a> {
    pub input: &'a [u8],
    pub output: &'a mut [u8],
    pub input_width: i32,
    pub input_height: i32,
    pub channels: i32,
    pub regions: &'a [TileRegion],
    pub config: TileConfig,
    pub progress: Option<&'a dyn Fn(&ProcessingProgress)>,
}

/// The inference backend contract. Mirrors the C++ `upix::Engine` pure-virtual
/// interface; defaults replicate the C++ defaults exactly.
///
/// Threading: `process`/`process_u8`/`process_batch` take `&self` (backends
/// synchronize internally, like the C++ engine). Mutable lifecycle methods
/// (`initialize`, `load_model`, `unload_model`, setters) take `&mut self`.
pub trait Engine: Send + Sync {
    fn initialize(&mut self, preferred: DeviceType) -> Result<(), EngineError>;

    fn list_devices(&self) -> Vec<DeviceCapability>;

    fn load_model(&mut self, param_path: &Path, bin_path: &Path) -> Result<(), EngineError>;

    /// One whole-image float inference (interleaved HWC, `width*height*channels` floats each way).
    fn process(
        &self,
        input: &[f32],
        output: &mut [f32],
        width: i32,
        height: i32,
        channels: i32,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError>;

    fn process_u8(
        &self,
        _input: &[u8],
        _output: &mut [u8],
        _width: i32,
        _height: i32,
        _channels: i32,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        Err(EngineError::new(102, "u8 process path not supported"))
    }

    /// Diagnostic: per-blob value ranges on one whole-image inference.
    fn debug_blob_scan(&self, _input: &[u8], _width: i32, _height: i32, _channels: i32) -> String {
        "debug_blob_scan not supported".to_string()
    }

    /// Process a batch of same-shaped tiles (interleaved HWC float).
    /// Default mirrors the C++ default: GPU backends serialize (Vulkan unsafe
    /// for concurrent submits), CPU backends run tiles concurrently on
    /// scoped threads.
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
        if self.current_device() == DeviceType::GpuVulkan {
            for i in 0..inputs.len() {
                self.process(inputs[i], outputs[i], width, height, channels, cancel)?;
            }
            return Ok(());
        }
        if inputs.len() == 1 {
            return self.process(inputs[0], outputs[0], width, height, channels, cancel);
        }
        // CPU: concurrent tiles via scoped threads (C++ used std::async).
        // Split `outputs` safely: each thread owns disjoint output slices.
        let mut results: Vec<Result<(), EngineError>> = Vec::with_capacity(inputs.len());
        std::thread::scope(|s| {
            let handles: Vec<_> = inputs
                .iter()
                .zip(outputs.iter_mut())
                .map(|(i, o)| s.spawn(move || self.process(i, o, width, height, channels, cancel)))
                .collect();
            for h in handles {
                results.push(h.join().expect("batch worker panicked"));
            }
        });
        for r in results {
            r?;
        }
        Ok(())
    }

    fn benchmark(&self, test_tile_size: i32) -> f32;

    fn unload_model(&mut self);

    fn current_device(&self) -> DeviceType;
    fn is_initialized(&self) -> bool;
    fn model_name(&self) -> String;

    fn get_scale_factor(&self) -> i32 {
        2
    }

    /// True when the engine inferred the scale from the param graph itself.
    /// Then callers must NOT overwrite it with a filename-derived guess.
    fn scale_factor_detected(&self) -> bool {
        false
    }

    /// Channel count the loaded model reads on its input blob (3 = RGB).
    /// 0 = unknown, which keeps the caller's channel count.
    fn model_input_channels(&self) -> i32 {
        0
    }

    /// Provide the filename-parsed scale (engines with successful param-graph
    /// detection ignore it).
    fn set_scale_factor(&mut self, _scale: i32) {}

    /// Cap inference threads for CPU engines. Must be called before `load_model`.
    fn set_inference_threads(&mut self, _threads: i32) {}

    /// Recommended tile size based on GPU VRAM (256 default, 0 = not available).
    fn get_recommended_tile_size(&self) -> i32 {
        256
    }

    /// Device memory available for allocations, in MB (0 = unknown).
    /// Honors UPIX_GPU_FORCE_BUDGET_MB (testing).
    fn get_device_memory_budget_mb(&self) -> u64 {
        0
    }

    /// (total_mb, available_mb)
    fn get_memory_info(&self) -> (u64, u64) {
        (0, 0)
    }

    /// Stable hardware identity for the auto-tile benchmark cache. Empty =
    /// unknown; callers then keep the cache process-local.
    fn hardware_id(&self) -> String {
        String::new()
    }

    fn supports_gpu_tile_pipeline(&self) -> bool {
        false
    }
    fn supports_gpu_tile_pipeline_u8(&self) -> bool {
        false
    }
    fn supports_gpu_tile_pipeline_u8_queue(&self) -> bool {
        false
    }

    fn process_tiles_gpu(
        &self,
        _input: &[f32],
        _output: &mut [f32],
        _input_width: i32,
        _input_height: i32,
        _channels: i32,
        _regions: &[TileRegion],
        _config: TileConfig,
        _progress: Option<&dyn Fn(&ProcessingProgress)>,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        Err(EngineError::new(200, "GPU tile pipeline not supported"))
    }

    fn process_tiles_gpu_u8(
        &self,
        _input: &[u8],
        _output: &mut [u8],
        _input_width: i32,
        _input_height: i32,
        _channels: i32,
        _regions: &[TileRegion],
        _config: TileConfig,
        _progress: Option<&dyn Fn(&ProcessingProgress)>,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        Err(EngineError::new(201, "GPU u8 tile pipeline not supported"))
    }

    fn process_tiles_gpu_u8_queue(
        &self,
        tasks: &mut [GpuTileTaskU8],
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        // Default mirrors the C++ default: run tasks sequentially through
        // process_tiles_gpu_u8.
        for task in tasks.iter_mut() {
            self.process_tiles_gpu_u8(
                task.input,
                &mut *task.output,
                task.input_width,
                task.input_height,
                task.channels,
                task.regions,
                task.config,
                task.progress,
                cancel,
            )?;
        }
        Ok(())
    }
}
