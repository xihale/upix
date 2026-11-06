//! Test-only deterministic engine mock (nearest-neighbor 2x upsample).
//! Shared by the pipeline / pool / setup / optimizer unit tests.

#![cfg(test)]

use crate::engine::{
    DeviceType, Engine, EngineError, GpuTileTaskU8, ProcessingProgress, TileConfig, TileRegion,
};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Mutex;

pub struct MockEngine {
    pub device: DeviceType,
    pub input_channels: i32,
    pub gpu_tiles: bool,
    pub gpu_tiles_u8: bool,
    /// Fail the GPU tile path with this error before falling through to the
    /// generic path (Some = fail on first call).
    pub gpu_fail: Option<EngineError>,
    /// Return this instead of running when config.tile_size exceeds this
    /// bound (OOM-ladder harness).
    pub oom_above_tile: Option<(i32, EngineError)>,
    pub load_calls: AtomicUsize,
    /// config.tile_size observed on every process_tiles_* call.
    pub gpu_tile_sizes: Mutex<Vec<i32>>,
    /// number of completed process() calls (fast-path/generic counters).
    pub process_calls: AtomicUsize,
    initialized: AtomicBool,
    /// Device the engine actually initialized on (None = not initialized).
    active_device: Mutex<Option<DeviceType>>,
}

impl Default for MockEngine {
    fn default() -> Self {
        Self {
            device: DeviceType::Cpu,
            input_channels: 0,
            gpu_tiles: false,
            gpu_tiles_u8: false,
            gpu_fail: None,
            oom_above_tile: None,
            load_calls: AtomicUsize::new(0),
            gpu_tile_sizes: Mutex::new(Vec::new()),
            process_calls: AtomicUsize::new(0),
            initialized: AtomicBool::new(false),
            active_device: Mutex::new(None),
        }
    }
}

impl MockEngine {
    pub fn new() -> Self {
        Self { input_channels: 3, ..Default::default() }
    }

    pub fn with_gpu() -> Self {
        Self {
            device: DeviceType::GpuVulkan,
            input_channels: 3,
            gpu_tiles: true,
            gpu_tiles_u8: true,
            ..Default::default()
        }
    }

    fn record_gpu_attempt(&self, config: TileConfig) -> Option<EngineError> {
        self.gpu_tile_sizes.lock().unwrap().push(config.tile_size);
        if let Some((bound, err)) = &self.oom_above_tile {
            if config.tile_size > *bound {
                return Some(err.clone());
            }
        }
        if let Some(fail) = &self.gpu_fail {
            return Some(fail.clone());
        }
        None
    }

    /// Deterministic nearest-neighbor 2x upsample of interleaved data.
    pub(crate) fn nearest_2x<T: Copy>(&self, input: &[T], output: &mut [T], width: i32, height: i32, channels: i32) {
        let w = width as usize;
        let ow = (width as usize) * 2;
        for oy in 0..(height as usize * 2) {
            let sy = oy / 2;
            for ox in 0..ow {
                let sx = ox / 2;
                for c in 0..channels as usize {
                    output[(oy * ow + ox) * channels as usize + c] =
                        input[(sy * w + sx) * channels as usize + c];
                }
            }
        }
    }
}

impl Engine for MockEngine {
    fn initialize(&mut self, preferred: DeviceType) -> Result<(), EngineError> {
        // GPU init fails when the caller asks for Vulkan but the mock was
        // built CPU-only (fallback tests), mirroring "no Vulkan device".
        if preferred == DeviceType::GpuVulkan && self.device != DeviceType::GpuVulkan {
            return Err(EngineError::new(101, "no vulkan device"));
        }
        *self.active_device.lock().unwrap() = Some(preferred);
        self.initialized.store(true, Ordering::Relaxed);
        Ok(())
    }

    fn list_devices(&self) -> Vec<crate::engine::DeviceCapability> {
        vec![]
    }

    fn load_model(&mut self, _param_path: &Path, _bin_path: &Path) -> Result<(), EngineError> {
        self.load_calls.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn process(
        &self,
        input: &[f32],
        output: &mut [f32],
        width: i32,
        height: i32,
        channels: i32,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        if channels != self.input_channels {
            return Err(EngineError::new(
                103,
                format!(
                    "expected=WxHx{}, actual=WxHx{}",
                    self.input_channels, channels
                ),
            ));
        }
        assert_eq!(output.len(), input.len() * 4);
        self.nearest_2x(input, output, width, height, channels);
        self.process_calls.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn benchmark(&self, _test_tile_size: i32) -> f32 {
        1.0
    }

    fn unload_model(&mut self) {}

    fn current_device(&self) -> DeviceType {
        self.active_device.lock().unwrap().unwrap_or(self.device)
    }

    fn is_initialized(&self) -> bool {
        self.initialized.load(Ordering::Relaxed)
    }

    fn model_name(&self) -> String {
        "mock".to_string()
    }

    fn get_scale_factor(&self) -> i32 {
        2
    }

    fn model_input_channels(&self) -> i32 {
        self.input_channels
    }

    fn get_memory_info(&self) -> (u64, u64) {
        (0, 0)
    }

    fn supports_gpu_tile_pipeline(&self) -> bool {
        self.gpu_tiles
    }

    fn supports_gpu_tile_pipeline_u8(&self) -> bool {
        self.gpu_tiles_u8
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
        _progress: Option<&dyn Fn(&ProcessingProgress)>,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        if let Some(e) = self.record_gpu_attempt(config) {
            return Err(e);
        }
        // GPU fast path on the mock: nearest-upsample each region in place
        // (tests drive it with overlap=0, so regions tile the image exactly).
        for region in regions {
            for y in 0..region.height {
                for x in 0..region.width {
                    let sx = (region.x + x) as usize;
                    let sy = (region.y + y) as usize;
                    let ox = (region.x as usize) * 2 + x as usize * 2;
                    let oy = (region.y as usize) * 2 + y as usize * 2;
                    for dy in 0..2 {
                        for dx in 0..2 {
                            for c in 0..channels as usize {
                                output[((oy + dy) * (input_width as usize) * 2 + ox + dx)
                                    * channels as usize
                                    + c] =
                                    input[(sy * input_width as usize + sx) * channels as usize + c];
                            }
                        }
                    }
                }
            }
        }
        let _ = input_height;
        self.process_calls.fetch_add(1, Ordering::Relaxed);
        Ok(())
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
        _progress: Option<&dyn Fn(&ProcessingProgress)>,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        if let Some(e) = self.record_gpu_attempt(config) {
            return Err(e);
        }
        for region in regions {
            for y in 0..region.height {
                for x in 0..region.width {
                    let sx = (region.x + x) as usize;
                    let sy = (region.y + y) as usize;
                    let ox = (region.x as usize) * 2 + x as usize * 2;
                    let oy = (region.y as usize) * 2 + y as usize * 2;
                    for dy in 0..2 {
                        for dx in 0..2 {
                            for c in 0..channels as usize {
                                output[((oy + dy) * (input_width as usize) * 2 + ox + dx)
                                    * channels as usize
                                    + c] =
                                    input[(sy * input_width as usize + sx) * channels as usize + c];
                            }
                        }
                    }
                }
            }
        }
        let _ = input_height;
        self.process_calls.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn process_tiles_gpu_u8_queue(
        &self,
        tasks: &mut [GpuTileTaskU8],
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
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
