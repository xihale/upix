//! TileProcessor — Rust port of `share/core/tile_processor.{hpp,cpp}`.
//!
//! Host-side tiled pipelines (float + u8), tile layout, blending with the
//! w*w quadratic edge fade, and the Mitchell-Netravali `bicubic_scale_u8`
//! (moved to [`crate::bicubic`]). The GPU fast path / OOM ladder orchestration
//! for whole images lives here too, mirroring the C++ entry points.
//!
//! Device note (verbatim from the C++): everything here except the engine call
//! runs on the host (tile extraction, blend, normalize). The inference itself
//! runs on whatever device the Engine was initialized with, so "host-side"
//! never implies a CPU guarantee.

use crate::engine::{
    is_oom_error, DeviceType, Engine, EngineError, ProcessingProgress, TileConfig, TileRegion,
};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};

pub use crate::bicubic::bicubic_scale_u8;

#[derive(Debug, Clone)]
pub struct TileProcessorError {
    pub code: i32,
    pub message: String,
}

impl TileProcessorError {
    fn new(code: i32, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }
}

impl From<TileProcessorError> for EngineError {
    fn from(e: TileProcessorError) -> Self {
        EngineError::new(e.code, e.message)
    }
}

/// Result envelope for the allocate-and-run convenience wrapper
/// ([`process_image_u8`]): scaled pixels plus the geometry the caller needs
/// to build an image around them.
#[derive(Debug, Clone)]
pub struct ScaledImage {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub data: Vec<u8>,
}

/// One batch of same-sized tiles staged between the load/proc/blend stages.
struct BatchWork {
    begin: usize,
    end: usize,
    width: u32,
    height: u32,
    input_buffers: Vec<Vec<f32>>,
    output_buffers: Vec<Vec<f32>>,
}

/// A processed batch handed from the proc stage to the blend stage.
struct CompletedBatch {
    begin: usize,
    output_buffers: Vec<Vec<f32>>,
}

pub struct TileProcessor;

impl TileProcessor {
    /// Tile grid over the image. Overlap is clamped to `[0, tile-1]` and the
    /// step to `>= 1`, so degenerate configs lay out sane regions; trailing
    /// tiles clamp against the right/bottom edges. An empty axis (width <= 0)
    /// still yields one zero-size start so callers see a (possibly empty)
    /// grid shape rather than nothing.
    pub fn compute_tile_layout(image_width: i32, image_height: i32, tile: i32, overlap: i32) -> Vec<TileRegion> {
        let tile_size = tile.max(1);
        let overlap = overlap.clamp(0, tile_size - 1);
        let step = (tile_size - overlap).max(1);

        let mut starts_x = Vec::new();
        let mut starts_y = Vec::new();
        let mut x = 0;
        while x < image_width {
            starts_x.push(x);
            if x + tile_size >= image_width {
                break;
            }
            x += step;
        }
        let mut y = 0;
        while y < image_height {
            starts_y.push(y);
            if y + tile_size >= image_height {
                break;
            }
            y += step;
        }
        if starts_x.is_empty() {
            starts_x.push(0);
        }
        if starts_y.is_empty() {
            starts_y.push(0);
        }

        let mut regions = Vec::with_capacity(starts_x.len() * starts_y.len());
        for &y in &starts_y {
            for &x in &starts_x {
                regions.push(TileRegion {
                    x: x as u32,
                    y: y as u32,
                    width: tile_size.min(image_width - x) as u32,
                    height: tile_size.min(image_height - y) as u32,
                    overlap: overlap as u32,
                });
            }
        }
        regions
    }

    fn compute_tile_layout_cfg(image_width: i32, image_height: i32, config: &TileConfig) -> Vec<TileRegion> {
        Self::compute_tile_layout(image_width, image_height, config.tile_size, config.overlap)
    }

    /// Extract one tile as interleaved floats. `src_channels` is the source
    /// pixel stride, `dst_channels` the tile stride; copying only the leading
    /// `dst_channels` is how an RGBA source feeds an RGB model (or vice versa)
    /// without a whole-image conversion.
    pub fn extract_tile(
        image_data: &[f32],
        image_width: i32,
        src_channels: i32,
        dst_channels: i32,
        region: &TileRegion,
    ) -> Vec<f32> {
        let mut tile = vec![0.0f32; region.width as usize * region.height as usize * dst_channels as usize];

        for y in 0..region.height {
            let src_idx = ((region.y + y) as usize * image_width as usize + region.x as usize) * src_channels as usize;
            let dst_idx = y as usize * region.width as usize * dst_channels as usize;
            if src_channels == dst_channels {
                let cw = region.width as usize * dst_channels as usize;
                tile[dst_idx..dst_idx + cw]
                    .copy_from_slice(&image_data[src_idx..src_idx + cw]);
            } else {
                // De-interleave: keep the leading dst_channels of every source
                // pixel (how an RGBA source feeds an RGB model).
                for x in 0..region.width {
                    let s = src_idx + x as usize * src_channels as usize;
                    let d = dst_idx + x as usize * dst_channels as usize;
                    tile[d..d + dst_channels as usize]
                        .copy_from_slice(&image_data[s..s + dst_channels as usize]);
                }
            }
        }
        tile
    }

    /// Per-tile extraction from u8 data with u8→float conversion on-the-fly.
    /// Avoids the full-image u8→float copy, saving O(W*H*C) float allocation.
    pub fn extract_tile_u8(
        image_data: &[u8],
        image_width: i32,
        src_channels: i32,
        dst_channels: i32,
        region: &TileRegion,
    ) -> Vec<f32> {
        const INV255: f32 = 1.0 / 255.0;
        let mut tile = vec![0.0f32; region.width as usize * region.height as usize * dst_channels as usize];

        for y in 0..region.height {
            let src_idx = ((region.y + y) as usize * image_width as usize + region.x as usize) * src_channels as usize;
            let dst_idx = y as usize * region.width as usize * dst_channels as usize;
            if src_channels == dst_channels {
                let cw = region.width as usize * dst_channels as usize;
                for c in 0..cw {
                    tile[dst_idx + c] = image_data[src_idx + c] as f32 * INV255;
                }
            } else {
                // De-interleave: convert only the leading dst_channels of each
                // source pixel (how an RGBA source feeds an RGB model).
                for x in 0..region.width {
                    let s = src_idx + x as usize * src_channels as usize;
                    let d = dst_idx + x as usize * dst_channels as usize;
                    for c in 0..dst_channels as usize {
                        tile[d + c] = image_data[s + c] as f32 * INV255;
                    }
                }
            }
        }
        tile
    }

    /// Blend one processed (scaled) tile into the float accumulator with the
    /// w*w quadratic edge fade. Unweighted pixels (no tile contributed) stay 0
    /// in both accumulators — the final normalize divides by the weight map.
    pub fn blend_tile(
        &self,
        tile_data: &[f32],
        output_data: &mut [f32],
        weight_data: &mut [f32],
        output_width: i32,
        output_height: i32,
        channels: i32,
        region: &TileRegion,
        scale_factor: i32,
        overlap: i32,
    ) {
        let out_x = region.x * scale_factor as u32;
        let out_y = region.y * scale_factor as u32;
        let out_w = region.width * scale_factor as u32;
        let out_h = region.height * scale_factor as u32;
        let scaled_overlap = overlap * scale_factor;
        let has_left_neighbor = out_x > 0;
        let has_top_neighbor = out_y > 0;
        let has_right_neighbor = (out_x + out_w) < output_width as u32;
        let has_bottom_neighbor = (out_y + out_h) < output_height as u32;

        if scaled_overlap <= 0 {
            for y in 0..out_h {
                for x in 0..out_w {
                    let dst_pixel = (out_y + y) as usize * output_width as usize + (out_x + x) as usize;
                    let src_idx = (y as usize * out_w as usize + x as usize) * channels as usize;
                    let dst_idx = dst_pixel * channels as usize;
                    for c in 0..channels as usize {
                        output_data[dst_idx + c] += tile_data[src_idx + c];
                    }
                    weight_data[dst_pixel] += 1.0;
                }
            }
            return;
        }

        let overlap_x = (scaled_overlap.max(0) as u32).min(out_w);
        let overlap_y = (scaled_overlap.max(0) as u32).min(out_h);
        let mut x_weight = vec![0.0f32; out_w as usize];
        let mut y_weight = vec![0.0f32; out_h as usize];
        for x in 0..out_w {
            let mut w = 1.0f32;
            if overlap_x > 0 {
                if has_left_neighbor && x < overlap_x {
                    w = (x + 1) as f32 / overlap_x as f32;
                } else if has_right_neighbor && x >= out_w - overlap_x {
                    w = (out_w - x) as f32 / overlap_x as f32;
                }
            }
            x_weight[x as usize] = w * w;
        }
        for y in 0..out_h {
            let mut w = 1.0f32;
            if overlap_y > 0 {
                if has_top_neighbor && y < overlap_y {
                    w = (y + 1) as f32 / overlap_y as f32;
                } else if has_bottom_neighbor && y >= out_h - overlap_y {
                    w = (out_h - y) as f32 / overlap_y as f32;
                }
            }
            y_weight[y as usize] = w * w;
        }

        for y in 0..out_h {
            let yw = y_weight[y as usize];
            for x in 0..out_w {
                let weight = x_weight[x as usize] * yw;

                let dst_pixel = (out_y + y) as usize * output_width as usize + (out_x + x) as usize;
                let dst_idx = dst_pixel * channels as usize;
                let src_idx = (y as usize * out_w as usize + x as usize) * channels as usize;

                for c in 0..channels as usize {
                    output_data[dst_idx + c] += tile_data[src_idx + c] * weight;
                }
                weight_data[dst_pixel] += weight;
            }
        }
    }

    /// Float tiled pipeline (`TileProcessor::process_image`).
    #[allow(clippy::too_many_arguments)]
    pub fn process_image(
        &self,
        input_data: &[f32],
        output_data: &mut [f32],
        input_width: i32,
        input_height: i32,
        channels: i32,
        engine: &dyn Engine,
        config: TileConfig,
        progress_callback: Option<&(dyn Fn(&ProcessingProgress) + Send + Sync)>,
        cancel_flag: &AtomicBool,
    ) -> Result<(), TileProcessorError> {
        if input_data.is_empty() || output_data.is_empty() {
            return Err(TileProcessorError::new(3, "Null image buffer for processing"));
        }
        if input_width <= 0 || input_height <= 0 || channels <= 0 {
            return Err(TileProcessorError::new(4, "Invalid image dimensions"));
        }

        // One normalized config for every stage. compute_tile_layout clamps the
        // overlap to tile_size-1, so blend_tile must weight that same overlap —
        // passing the raw value let a config with overlap >= tile_size lay out
        // 1-pixel steps but blend them with a whole-tile fade ramp. The tile
        // pipeline_config carries the size the OOM ladder last ran with, so the
        // blend weights always match the regions handed to the host-side stage.
        let mut pipeline_config = config;
        pipeline_config.tile_size = pipeline_config.tile_size.max(1);
        pipeline_config.overlap =
            pipeline_config.overlap.clamp(0, pipeline_config.tile_size - 1);
        pipeline_config.batch_size = pipeline_config.batch_size.max(1);

        let mut regions =
            Self::compute_tile_layout_cfg(input_width, input_height, &pipeline_config);

        // GPU fast path. Skipped for an RGBA source on an RGB model — the
        // alpha has to be split off host-side first (see below).
        if engine.supports_gpu_tile_pipeline()
            && !(channels == 4 && engine.model_input_channels() == 3)
        {
            if engine.current_device() == DeviceType::GpuVulkan && pipeline_config.batch_size == 1 {
                // Keep the device fed when the caller left the default batch size.
                pipeline_config.batch_size = 2;
            }
            // OOM ladder: shrink the tile (the per-tile working set scales with
            // tile²); when that stops helping, fall through to the host-side
            // tiled pipeline below (which blends with exactly what the ladder
            // ended on; the inference device is unchanged).
            // The engine trait takes `Option<&dyn Fn>` (auto-trait-less);
            // drop Send+Sync explicitly for the call — the GPU path runs on
            // this thread.
            let progress_plain: Option<&dyn Fn(&ProcessingProgress)> =
                progress_callback.map(|cb| cb as &dyn Fn(&ProcessingProgress));
            loop {
                let result = engine.process_tiles_gpu(
                    input_data,
                    output_data,
                    input_width,
                    input_height,
                    channels,
                    &regions,
                    pipeline_config,
                    progress_plain,
                    cancel_flag,
                );
                match result {
                    Ok(()) => return Ok(()),
                    Err(e) => {
                        if !is_oom_error(&e) {
                            return Err(TileProcessorError::new(
                                1,
                                format!("GPU tile processing failed: {}", e.message),
                            ));
                        }
                        if pipeline_config.tile_size <= 64 {
                            log::warn!(target: "TileProcessor",
                                "GPU out of memory at tile_size=64, falling back to host-side blending (inference device unchanged)");
                            break;
                        }
                        pipeline_config.tile_size = (pipeline_config.tile_size / 2).max(64);
                        pipeline_config.overlap =
                            pipeline_config.overlap.min(pipeline_config.tile_size / 4);
                        log::warn!(target: "TileProcessor",
                            "GPU out of memory, retrying with tile_size={}", pipeline_config.tile_size);
                        regions = Self::compute_tile_layout_cfg(input_width, input_height, &pipeline_config);
                    }
                }
            }
        }

        // An RGBA image fed to an RGB model: the alpha channel is not part of
        // the model's input, and the net never emits a 4th channel, so keeping
        // it in the pipeline makes process()'s output-shape check reject every
        // tile ("expected=WxHx4, actual=WxHx3"). Split it instead — the net
        // sees RGB only (a quarter less input staging) and the output's 4th
        // channel is filled from the host-side bicubic below, the same kernel
        // process_image_u8's alpha bypass uses, so both entry points agree.
        let alpha_split = channels == 4 && engine.model_input_channels() == 3;
        let model_channels = if alpha_split { 3 } else { channels };

        let scale_factor = engine.get_scale_factor();
        let output_width = input_width * scale_factor;
        let output_height = input_height * scale_factor;

        let alpha_output = if alpha_split {
            let mut alpha_input = vec![0u8; input_width as usize * input_height as usize];
            for (i, a) in alpha_input.iter_mut().enumerate() {
                *a = (input_data[i * channels as usize + 3].clamp(0.0, 1.0) * 255.0)
                    .round() as u8;
            }
            let mut out = vec![0u8; output_width as usize * output_height as usize];
            bicubic_scale_u8(
                &alpha_input,
                input_width as u32,
                input_height as u32,
                &mut out,
                output_width as u32,
                output_height as u32,
            );
            log::info!(target: "TileProcessor", "Alpha channel bypassed neural network (bicubic)");
            Some(out)
        } else {
            None
        };

        // Host-side float path: dedicated load thread + bounded ready queue
        // (deep prefetch keeps the engine fed), memcpy tile extraction, float
        // normalize.
        self.run_tiled_pipeline(
            PipelineShape::ThreeStage,
            engine,
            &regions,
            pipeline_config,
            input_width,
            input_height,
            model_channels,
            progress_callback,
            cancel_flag,
            &|image_width, ch, region| {
                Self::extract_tile(input_data, image_width, channels, ch, region)
            },
            &mut |output_buffer: &mut [f32], weight_buffer: &[f32]| {
                let pixel_count = (output_width as usize) * (output_height as usize);
                for p in 0..pixel_count {
                    let w = weight_buffer[p];
                    let src_base = p * model_channels as usize;
                    let dst_base = p * channels as usize;
                    for c in 0..model_channels as usize {
                        // Unweighted pixels (no tile contributed) stay 0, which
                        // is what the accumulator already held for them.
                        output_data[dst_base + c] = if w > 1e-6 {
                            output_buffer[src_base + c] / w
                        } else {
                            0.0
                        };
                    }
                }

                if let Some(alpha) = &alpha_output {
                    for p in 0..pixel_count {
                        output_data[p * channels as usize + 3] = alpha[p] as f32 / 255.0;
                    }
                    log::info!(target: "TileProcessor", "Merged bicubic alpha into output");
                }
            },
        )
    }

    /// u8 tiled pipeline (`TileProcessor::process_image_u8`).
    #[allow(clippy::too_many_arguments)]
    pub fn process_image_u8_tiled(
        &self,
        input_data: &[u8],
        output_data: &mut [u8],
        input_width: i32,
        input_height: i32,
        channels: i32,
        engine: &dyn Engine,
        config: TileConfig,
        progress_callback: Option<&(dyn Fn(&ProcessingProgress) + Send + Sync)>,
        cancel_flag: &AtomicBool,
        enable_alpha_bicubic: bool,
    ) -> Result<(), TileProcessorError> {
        if input_data.is_empty() || output_data.is_empty() {
            return Err(TileProcessorError::new(3, "Null image buffer for u8 processing"));
        }
        if input_width <= 0 || input_height <= 0 || channels <= 0 {
            return Err(TileProcessorError::new(4, "Invalid image dimensions"));
        }

        // Same normalized config as the float path: the blend weights must use
        // the overlap the layout actually applied, and the host-side stage must
        // see the size the OOM ladder ended on.
        let mut pipeline_config = config;
        pipeline_config.tile_size = pipeline_config.tile_size.max(1);
        pipeline_config.overlap =
            pipeline_config.overlap.clamp(0, pipeline_config.tile_size - 1);
        pipeline_config.batch_size = pipeline_config.batch_size.max(1);

        let mut regions =
            Self::compute_tile_layout_cfg(input_width, input_height, &pipeline_config);

        if engine.supports_gpu_tile_pipeline_u8()
            && !(channels == 4 && enable_alpha_bicubic && engine.model_input_channels() == 3)
        {
            if engine.current_device() == DeviceType::GpuVulkan && pipeline_config.batch_size == 1 {
                pipeline_config.batch_size = 2;
            }

            // OOM ladder (same as the float path): shrink the tile, then blend
            // on the host with only tile-sized GPU work (the inference device
            // is unchanged — this is not a CPU fallback).
            let progress_plain: Option<&dyn Fn(&ProcessingProgress)> =
                progress_callback.map(|cb| cb as &dyn Fn(&ProcessingProgress));
            loop {
                let result = engine.process_tiles_gpu_u8(
                    input_data,
                    output_data,
                    input_width,
                    input_height,
                    channels,
                    &regions,
                    pipeline_config,
                    progress_plain,
                    cancel_flag,
                );
                match result {
                    Ok(()) => return Ok(()),
                    Err(e) => {
                        if !is_oom_error(&e) {
                            return Err(TileProcessorError::new(
                                1,
                                format!("GPU u8 tile processing failed: {}", e.message),
                            ));
                        }
                        if pipeline_config.tile_size <= 64 {
                            log::warn!(target: "TileProcessor",
                                "GPU out of memory at tile_size=64, falling back to host-side blending (inference device unchanged)");
                            break;
                        }
                        pipeline_config.tile_size = (pipeline_config.tile_size / 2).max(64);
                        pipeline_config.overlap =
                            pipeline_config.overlap.min(pipeline_config.tile_size / 4);
                        log::warn!(target: "TileProcessor",
                            "GPU out of memory, retrying with tile_size={}", pipeline_config.tile_size);
                        regions = Self::compute_tile_layout_cfg(input_width, input_height, &pipeline_config);
                    }
                }
            }
        }

        // Per-tile host path: extract from u8, convert to float per-tile,
        // process, blend. This avoids the full-image u8→float copy, saving
        // O(W*H*C) float allocation.
        let scale_factor = engine.get_scale_factor();
        let output_width = input_width * scale_factor;
        let output_height = input_height * scale_factor;

        // Alpha channel bypass for RGBA images: the 3-channel model must not be
        // handed a 4th channel it never emits back (process()'s output-shape
        // check would reject every tile), so the tiles handed to the engine are
        // RGB and alpha is scaled here on the host and merged into the output's
        // 4th channel at the end. `enable_alpha_bicubic=false` keeps alpha on
        // the network input path, which only a 4-channel model can consume.
        let alpha_bicubic =
            channels == 4 && enable_alpha_bicubic && engine.model_input_channels() == 3;
        let model_channels = if alpha_bicubic { 3 } else { channels };

        let alpha_output = if alpha_bicubic {
            let mut alpha_input = vec![0u8; input_width as usize * input_height as usize];
            for (i, a) in alpha_input.iter_mut().enumerate() {
                *a = input_data[i * channels as usize + 3];
            }
            let mut out = vec![0u8; output_width as usize * output_height as usize];
            bicubic_scale_u8(
                &alpha_input,
                input_width as u32,
                input_height as u32,
                &mut out,
                output_width as u32,
                output_height as u32,
            );
            log::info!(target: "TileProcessor", "Alpha channel bypassed neural network (bicubic)");
            Some(out)
        } else {
            None
        };

        // Host-side u8 path: one-ahead batch prefetch (per-tile u8→float
        // extraction keeps the memory footprint low), u8 normalize/quantize on
        // the way out.
        self.run_tiled_pipeline(
            PipelineShape::TwoStage,
            engine,
            &regions,
            pipeline_config,
            input_width,
            input_height,
            model_channels,
            progress_callback,
            cancel_flag,
            &|image_width, ch, region| {
                // Per-tile u8→float extraction (avoids full-image float copy);
                // drops the alpha channel when the net only wants RGB.
                Self::extract_tile_u8(input_data, image_width, channels, ch, region)
            },
            &mut |output_float: &mut [f32], weight_buffer: &[f32]| {
                // Normalize and convert float output to u8, back onto the
                // caller's pixel stride (which is wider than the model's when
                // alpha was split).
                let pixel_count = (output_width as usize) * (output_height as usize);
                for p in 0..pixel_count {
                    let w = weight_buffer[p];
                    if w <= 1e-6 {
                        continue;
                    }
                    let src_base = p * model_channels as usize;
                    let dst_base = p * channels as usize;
                    for c in 0..model_channels as usize {
                        let v = ((output_float[src_base + c] / w) * 255.0).clamp(0.0, 255.0);
                        output_data[dst_base + c] = v as u8;
                    }
                }

                // Replace alpha with bicubic-scaled alpha (bypasses the neural
                // network for alpha).
                if let Some(alpha) = &alpha_output {
                    for (i, dst) in output_data
                        .iter_mut()
                        .skip(3)
                        .step_by(channels as usize)
                        .take(pixel_count)
                        .enumerate()
                    {
                        *dst = alpha[i];
                    }
                    log::info!(target: "TileProcessor", "Merged bicubic alpha into output");
                }
            },
        )
    }

    /// Allocate-and-run convenience wrapper matching the CONTRACT §3 API pin:
    /// sizes the output from `engine.get_scale_factor()` and returns the
    /// scaled pixels.
    #[allow(clippy::too_many_arguments)]
    pub fn process_image_u8(
        engine: &dyn Engine,
        cfg: &crate::config::ProcessingConfig,
        input: &[u8],
        width: i32,
        height: i32,
        channels: i32,
        progress: Option<&(dyn Fn(&ProcessingProgress) + Send + Sync)>,
        cancel: &AtomicBool,
    ) -> Result<ScaledImage, EngineError> {
        let scale = engine.get_scale_factor();
        let out_len = (width as usize * scale as usize)
            * (height as usize * scale as usize)
            * channels as usize;
        let mut output = vec![0u8; out_len];
        let tile_cfg = TileConfig {
            tile_size: cfg.tile_size,
            overlap: cfg.tile_overlap,
            batch_size: cfg.batch_size,
        };
        TileProcessor
            .process_image_u8_tiled(
                input,
                &mut output,
                width,
                height,
                channels,
                engine,
                tile_cfg,
                progress,
                cancel,
                true,
            )
            .map_err(EngineError::from)?;
        Ok(ScaledImage { width: width * scale, height: height * scale, channels, data: output })
    }

    /// Driver for the host-side tiled pipeline. `extract_tile_fn` extracts one
    /// tile as floats (image_width, model_channels, region). `finalize_fn`
    /// consumes the accumulated float output after blending. Only
    /// extraction/blending/finalizing are host work — the engine call inside
    /// runs on the engine's own device (GPU for a Vulkan engine).
    ///
    /// `shape` selects the scheduling shape, preserved exactly from the C++:
    /// `ThreeStage` — dedicated load thread feeding a bounded ready queue
    /// (capacity 2), main-thread engine processing, blend thread (float path);
    /// `TwoStage` — one-ahead batch prefetch on a helper thread, blend thread
    /// (u8 path).
    #[allow(clippy::too_many_arguments)]
    fn run_tiled_pipeline(
        &self,
        shape: PipelineShape,
        engine: &dyn Engine,
        regions: &[TileRegion],
        config: TileConfig,
        input_width: i32,
        input_height: i32,
        model_channels: i32,
        progress_callback: Option<&(dyn Fn(&ProcessingProgress) + Send + Sync)>,
        cancel_flag: &AtomicBool,
        extract_tile_fn: &(dyn Fn(i32, i32, &TileRegion) -> Vec<f32> + Send + Sync),
        finalize_fn: &mut dyn FnMut(&mut [f32], &[f32]),
    ) -> Result<(), TileProcessorError> {
        let scale_factor = engine.get_scale_factor();
        let output_width = (input_width * scale_factor) as usize;
        let output_height = (input_height * scale_factor) as usize;

        // Host RAM preflight for the full-output float accumulator + weight
        // map: refuse with a clean error instead of a failed allocation
        // mid-run.
        {
            let (total_mb, avail_mb) = engine.get_memory_info();
            let _ = total_mb;
            if avail_mb > 0 {
                let host_need = output_width * output_height
                    * ((model_channels as usize) + 1)
                    * std::mem::size_of::<f32>();
                if host_need > (avail_mb as usize) * 1024 * 1024 {
                    return Err(TileProcessorError::new(
                        crate::engine::ERR_OUT_OF_MEMORY,
                        format!(
                            "out of host memory: need ~{}MB, available {}MB",
                            host_need / (1024 * 1024),
                            avail_mb
                        ),
                    ));
                }
            }
        }

        let accum = Mutex::new((
            vec![0.0f32; output_width * output_height * model_channels as usize],
            vec![0.0f32; output_width * output_height],
        ));

        let progress = Mutex::new(ProcessingProgress {
            total_tiles: regions.len() as i32,
            completed_tiles: 0,
            ..ProcessingProgress::default()
        });

        let mut effective_batch_size = config.batch_size.max(1);
        if effective_batch_size == 1 && engine.current_device() == DeviceType::GpuVulkan {
            // A small implicit batch helps keep GPU busy when callers keep the
            // default batch_size.
            effective_batch_size = 2;
        }

        let cancel_flag: &AtomicBool = cancel_flag;

        // Build one engine batch starting at `start_idx`: consecutive regions
        // of identical size, up to the effective batch size.
        let build_batch = |start_idx: usize| -> Option<BatchWork> {
            if start_idx >= regions.len() || cancel_flag.load(Ordering::Relaxed) {
                return None;
            }

            let width = regions[start_idx].width;
            let height = regions[start_idx].height;
            let mut end = start_idx + 1;

            while end < regions.len()
                && end - start_idx < effective_batch_size as usize
                && regions[end].width == width
                && regions[end].height == height
            {
                end += 1;
            }

            let batch_count = end - start_idx;
            let mut input_buffers = Vec::with_capacity(batch_count);
            let mut output_buffers = Vec::with_capacity(batch_count);

            for region in &regions[start_idx..end] {
                input_buffers.push(extract_tile_fn(input_width, model_channels, region));

                let tile_out_w = region.width as usize * scale_factor as usize;
                let tile_out_h = region.height as usize * scale_factor as usize;
                output_buffers.push(vec![0.0f32; tile_out_w * tile_out_h * model_channels as usize]);
            }

            Some(BatchWork { begin: start_idx, end, width, height, input_buffers, output_buffers })
        };

        // Proc → blend handoff queue. A stage stop wakes the wait regardless
        // of the queue predicate.
        let completed_queue = Mutex::new(VecDeque::<CompletedBatch>::new());
        let completed_cv = Condvar::new();
        let process_done = Arc::new(AtomicBool::new(false));
        // Load→Proc bounded queue state, declared outside the thread scope so
        // the stage closures borrow it from the enclosing frame (the C++
        // equivalents are captured by reference the same way).
        let ready_queue = Mutex::new(VecDeque::<BatchWork>::new());
        let ready_cv = Condvar::new();
        let load_done = Arc::new(AtomicBool::new(false));
        // Wake the load thread when the main thread exits early (engine
        // error): the C++ relied on destructor-side stop flags for this; the
        // explicit flag keeps the same "no stranded stage" contract.
        let load_stop = Arc::new(AtomicBool::new(false));
        let mut process_error: Option<String> = None;

        std::thread::scope(|scope| {
            // --- Blend stage (blend/save thread) ---
            // A failing stage degrades to a reported error rather than a
            // crash; safe Rust makes the stage body infallible, so the C++
            // catch-all here maps to "cannot happen".
            let blend_handle = scope.spawn(|| {
                loop {
                    let completed_batch = {
                        let mut queue = completed_queue.lock().unwrap();
                        queue = completed_cv
                            .wait_while(queue, |q: &mut VecDeque<CompletedBatch>| {
                                q.is_empty() && !process_done.load(Ordering::Relaxed)
                            })
                            .unwrap();
                        if queue.is_empty() {
                            // Queue drained and producer done, or stop
                            // requested (cancel / exceptional shutdown):
                            // either way, exit.
                            break;
                        }
                        queue.pop_front().unwrap()
                    };

                    let mut accum = accum.lock().unwrap();
                    let accum_tuple = &mut *accum;
                    let mut prog = progress.lock().unwrap();
                    for (idx, output) in completed_batch.output_buffers.iter().enumerate() {
                        let region = regions[completed_batch.begin + idx];
                        self.blend_tile(
                            output,
                            &mut accum_tuple.0,
                            &mut accum_tuple.1,
                            output_width as i32,
                            output_height as i32,
                            model_channels,
                            &region,
                            scale_factor,
                            config.overlap,
                        );
                        prog.completed_tiles += 1;
                        prog.current_tile_x = region.x;
                        prog.current_tile_y = region.y;
                        if let Some(cb) = progress_callback {
                            cb(&*prog);
                        }
                    }
                }
            });

            match shape {
                PipelineShape::ThreeStage => {
                    // --- Three-stage pipeline: Load | Proc | Save ---
                    // Load thread extracts tiles, Proc (main thread) processes
                    // via engine, blend thread blends. Bounded queues connect
                    // stages to keep all hardware busy.

                    // Bounded queue for Load → Proc (capacity 2: one
                    // processing, one ready).
                    const READY_QUEUE_CAPACITY: usize = 2;
                    let mut load_error: Option<String> = None;

                    let load_handle = scope.spawn(|| {
                        let mut next_idx = 0usize;
                        while !cancel_flag.load(Ordering::Relaxed)
                            && !load_stop.load(Ordering::Relaxed)
                        {
                            let batch = match build_batch(next_idx) {
                                Some(b) => b,
                                None => break,
                            };
                            next_idx = batch.end;

                            {
                                let mut queue = ready_queue.lock().unwrap();
                                queue = ready_cv
                                    .wait_while(queue, |q: &mut VecDeque<BatchWork>| {
                                        q.len() >= READY_QUEUE_CAPACITY
                                            && !cancel_flag.load(Ordering::Relaxed)
                                            && !load_stop.load(Ordering::Relaxed)
                                    })
                                    .unwrap();
                                if cancel_flag.load(Ordering::Relaxed)
                                    || load_stop.load(Ordering::Relaxed)
                                {
                                    break;
                                }
                                queue.push_back(batch);
                            }
                            ready_cv.notify_one();
                        }
                        load_done.store(true, Ordering::Relaxed);
                        ready_cv.notify_one();
                    });

                    // --- Stage 2: Proc (main thread) - process batches
                    // through the engine ---
                    while !cancel_flag.load(Ordering::Relaxed) {
                        let current_batch = {
                            let mut queue = ready_queue.lock().unwrap();
                            queue = ready_cv
                                .wait_while(queue, |q: &mut VecDeque<BatchWork>| {
                                    q.is_empty() && !load_done.load(Ordering::Relaxed)
                                })
                                .unwrap();
                            if queue.is_empty() && load_done.load(Ordering::Relaxed) {
                                break;
                            }
                            queue.pop_front()
                        };
                        ready_cv.notify_one(); // Signal load thread to fill the queue

                        let Some(mut current_batch) = current_batch else {
                            break;
                        };

                        let inputs: Vec<&[f32]> =
                            current_batch.input_buffers.iter().map(|v| v.as_slice()).collect();
                        let mut outputs: Vec<&mut [f32]> = current_batch
                            .output_buffers
                            .iter_mut()
                            .map(|v| v.as_mut_slice())
                            .collect();

                        let result = engine.process_batch(
                            &inputs,
                            &mut outputs,
                            current_batch.width as i32,
                            current_batch.height as i32,
                            model_channels,
                            cancel_flag,
                        );
                        // `outputs` borrow the same storage as
                        // `current_batch.output_buffers`; the engine results
                        // are already in place.
                        if let Err(e) = result {
                            process_error =
                                Some(format!("Batch processing failed: {}", e.message));
                            break;
                        }

                        completed_queue
                            .lock()
                            .unwrap()
                            .push_back(CompletedBatch {
                                begin: current_batch.begin,
                                output_buffers: current_batch.output_buffers,
                            });
                        completed_cv.notify_one();
                    }

                    // Wake the load thread if the main loop exited early on an
                    // engine error (it may be parked on a full queue).
                    load_stop.store(true, Ordering::Relaxed);
                    ready_cv.notify_all();

                    process_done.store(true, Ordering::Relaxed);
                    completed_cv.notify_all();

                    if let Err(payload) = load_handle.join() {
                        // The stage body is infallible; a panic here is a bug —
                        // report it like the C++ catch-all did.
                        load_error = Some(stage_panic_message(payload, "tile-load thread"));
                    }

                    if load_error.is_some() && process_error.is_none() {
                        process_error =
                            Some(format!("Tile loading failed: {}", load_error.unwrap()));
                    }
                }
                PipelineShape::TwoStage => {
                    // --- Two-stage pipeline: one-ahead batch prefetch | blend
                    // --- (the C++ used std::async for the prefetch).
                    let mut current_batch = build_batch(0);
                    while current_batch.is_some() && !cancel_flag.load(Ordering::Relaxed) {
                        let mut current = current_batch.unwrap();

                        let next_start = current.end;
                        let has_next = next_start < regions.len();
                        let next_handle = has_next.then(|| {
                            scope.spawn(move || build_batch(next_start))
                        });

                        let inputs: Vec<&[f32]> =
                            current.input_buffers.iter().map(|v| v.as_slice()).collect();
                        let mut outputs: Vec<&mut [f32]> =
                            current.output_buffers.iter_mut().map(|v| v.as_mut_slice()).collect();

                        let result = engine.process_batch(
                            &inputs,
                            &mut outputs,
                            current.width as i32,
                            current.height as i32,
                            model_channels,
                            cancel_flag,
                        );
                        // Same in-place storage as above.
                        if let Err(e) = result {
                            process_error =
                                Some(format!("Batch processing failed: {}", e.message));
                            break;
                        }

                        completed_queue
                            .lock()
                            .unwrap()
                            .push_back(CompletedBatch {
                                begin: current.begin,
                                output_buffers: current.output_buffers,
                            });
                        completed_cv.notify_one();

                        current_batch = match next_handle {
                            Some(h) => h.join().ok().flatten(),
                            None => None,
                        };
                    }

                    process_done.store(true, Ordering::Relaxed);
                    completed_cv.notify_all();
                }
            }

            if let Err(payload) = blend_handle.join() {
                if process_error.is_none() {
                    process_error = Some(stage_panic_message(payload, "blend thread"));
                }
            }
        });

        if let Some(err) = process_error {
            return Err(TileProcessorError::new(1, err));
        }

        if cancel_flag.load(Ordering::Relaxed) {
            return Err(TileProcessorError::new(2, "Processing cancelled"));
        }

        {
            let mut guard = accum.lock().unwrap();
            // Destructure the tuple through a mutable reborrow; the two
            // field borrows are disjoint.
            let (output_buffer, weight_buffer) = &mut *guard;
            finalize_fn(output_buffer, weight_buffer);
        }
        Ok(())
    }
}

enum PipelineShape {
    /// Load thread | Proc (main) | Blend thread — float path.
    ThreeStage,
    /// One-ahead prefetch | Proc (main) | Blend thread — u8 path.
    TwoStage,
}

/// Free-function form of [`TileProcessor::process_image_u8`] (CONTRACT §3
/// API pin).
#[allow(clippy::too_many_arguments)]
pub fn process_image_u8(
    engine: &dyn Engine,
    cfg: &crate::config::ProcessingConfig,
    input: &[u8],
    width: i32,
    height: i32,
    channels: i32,
    progress: Option<&(dyn Fn(&ProcessingProgress) + Send + Sync)>,
    cancel: &AtomicBool,
) -> Result<ScaledImage, EngineError> {
    TileProcessor::process_image_u8(engine, cfg, input, width, height, channels, progress, cancel)
}

fn stage_panic_message(payload: Box<dyn std::any::Any + Send>, stage: &str) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        format!("unknown failure in {stage}: {s}")
    } else if let Some(s) = payload.downcast_ref::<String>() {
        format!("unknown failure in {stage}: {s}")
    } else {
        format!("unknown failure in {stage}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockEngine;
    use std::sync::Arc;

    fn cancel_flag() -> AtomicBool {
        AtomicBool::new(false)
    }

    // ---- compute_tile_layout: boundaries and clamps ----

    #[test]
    fn layout_exact_tiling_no_overlap() {
        let regions = TileProcessor::compute_tile_layout(10, 4, 4, 0);
        // starts x: 0, 4, 8 (8+4 >= 10 stops) → 3 columns x 1 row
        assert_eq!(regions.len(), 3);
        assert_eq!(
            regions.iter().map(|r| (r.x, r.width)).collect::<Vec<_>>(),
            vec![(0, 4), (4, 4), (8, 2)],
            "trailing tile must clamp to the right edge"
        );
        assert!(regions.iter().all(|r| r.y == 0 && r.height == 4 && r.overlap == 0));
    }

    #[test]
    fn layout_overlap_steps_and_edge_clamp() {
        // step = 8 - 2 = 6; starts 0, 6 (6+8 >= 10 stops)
        let regions = TileProcessor::compute_tile_layout(10, 10, 8, 2);
        assert_eq!(regions.len(), 4); // 2 x 2 grid
        let xs: Vec<u32> = regions.iter().map(|r| r.x).collect();
        assert_eq!(xs, vec![0, 6, 0, 6]);
        // Right column clamps from 8 to 4 wide.
        assert_eq!(regions[1].width, 4);
        assert_eq!(regions[1].height, 8);
        assert_eq!(regions[3].height, 4);
    }

    #[test]
    fn layout_overlap_clamped_to_tile_minus_one() {
        // overlap 10 with tile 4 → clamped to 3, step 1; image width 5:
        // starts 0 (0+4<5), 1 (1+4>=5 stops) → 2 columns.
        let regions = TileProcessor::compute_tile_layout(5, 1, 4, 10);
        assert_eq!(regions.len(), 2);
        assert_eq!(regions[0].overlap, 3);
        assert_eq!((regions[0].x, regions[1].x), (0, 1));
    }

    #[test]
    fn layout_degenerate_inputs() {
        // Zero-size image still yields the (empty) region grid shape.
        let regions = TileProcessor::compute_tile_layout(0, 0, 4, 0);
        assert_eq!(regions.len(), 1);
        assert_eq!(regions[0].width, 0);
        // Tile < 1 clamps to 1.
        let regions = TileProcessor::compute_tile_layout(3, 1, 0, -5);
        assert_eq!(regions.len(), 3);
        assert_eq!(regions[0].overlap, 0);
    }

    // ---- blend_tile: w*w fade and seam continuity ----

    #[test]
    fn blend_two_constant_tiles_seam_is_continuous() {
        // Input 6x2, tile 4 overlap 2 → tiles (0,0,4,2) and (4,0,2,2);
        // scaled x2 → output 12x4, fully covered by the two regions.
        let scale = 2;
        let out_w = 12i32;
        let out_h = 4i32;
        let channels = 1;
        let mut output = vec![0.0f32; (out_w * out_h) as usize];
        let mut weights = vec![0.0f32; (out_w * out_h) as usize];

        let proc = TileProcessor;
        // Left tile (4x2 @ 0,0 → 8x4): fades on the right edge only.
        let left = vec![1.0f32; 8 * 4];
        proc.blend_tile(
            &left, &mut output, &mut weights, out_w, out_h, channels,
            &TileRegion { x: 0, y: 0, width: 4, height: 2, overlap: 2 }, scale, 2,
        );
        // Right tile (2x2 @ 4,0 → 4x4): fades on the left edge only (right
        // and bottom are flush with the output edge).
        let right = vec![1.0f32; 4 * 4];
        proc.blend_tile(
            &right, &mut output, &mut weights, out_w, out_h, channels,
            &TileRegion { x: 4, y: 0, width: 2, height: 2, overlap: 2 }, scale, 2,
        );

        // Every pixel is covered by at least one tile with positive weight,
        // and constant tiles must blend back to the same constant everywhere.
        let normalized: Vec<f32> = output
            .iter()
            .zip(&weights)
            .map(|(&v, &w)| if w > 1e-6 { v / w } else { -1.0 })
            .collect();
        for (i, v) in normalized.iter().enumerate() {
            assert!((v - 1.0).abs() < 1e-5, "pixel {i}: {v} (constant tiles must blend to the same constant)");
        }
    }

    #[test]
    fn blend_weights_are_quadratic_and_edge_aware() {
        // Single tile, no neighbors at all: all weights must be exactly 1.
        let proc = TileProcessor;
        let tile = vec![2.0f32; 4 * 4];
        let mut output = vec![0.0f32; 4 * 4];
        let mut weights = vec![0.0f32; 4 * 4];
        proc.blend_tile(&tile, &mut output, &mut weights, 4, 4, 1,
            &TileRegion { x: 0, y: 0, width: 4, height: 4, overlap: 2 }, 1, 2);
        assert!(weights.iter().all(|&w| w == 1.0));
        assert!(output.iter().all(|&v| v == 2.0));

        // Same tile shifted right by one pixel: the tile's leftmost column
        // (global x=1) fades to (1/2)^2; x=0 is uncovered (weight 0); the
        // flush right edge must not fade.
        let tile = vec![2.0f32; 4 * 4];
        let mut output = vec![0.0f32; 5 * 4];
        let mut weights = vec![0.0f32; 5 * 4];
        proc.blend_tile(&tile, &mut output, &mut weights, 5, 4, 1,
            &TileRegion { x: 1, y: 0, width: 4, height: 4, overlap: 2 }, 1, 2);
        assert_eq!(weights[0], 0.0, "uncovered pixel keeps weight 0");
        assert!((weights[1] - 0.25).abs() < 1e-6, "left fade column = (1/ov)^2*1");
        assert!((weights[2] - 1.0).abs() < 1e-6);
        // Right edge flush with output → no right fade.
        assert!((weights[4] - 1.0).abs() < 1e-6);
    }

    #[test]
    fn blend_zero_overlap_accumulates_additively() {
        let proc = TileProcessor;
        let tile = vec![1.0f32; 2 * 2];
        let mut output = vec![0.0f32; 2 * 2];
        let mut weights = vec![0.0f32; 2 * 2];
        proc.blend_tile(&tile, &mut output, &mut weights, 2, 2, 1,
            &TileRegion { x: 0, y: 0, width: 2, height: 2, overlap: 0 }, 1, 0);
        assert!(weights.iter().all(|&w| w == 1.0));
    }

    // ---- extract_tile_u8 / extract_tile: RGBA de-interleave ----

    #[test]
    fn extract_tile_u8_rgba_splits_roundtrip() {
        // 2x2 RGBA source; expect leading-3 de-interleave per pixel, u8→float.
        let img: Vec<u8> = vec![
            10, 20, 30, 255, 40, 50, 60, 0,
            70, 80, 90, 128, 100, 110, 120, 200,
        ];
        let region = TileRegion { x: 0, y: 0, width: 2, height: 2, overlap: 0 };
        let tile = TileProcessor::extract_tile_u8(&img, 2, 4, 3, &region);
        let inv255 = 1.0f32 / 255.0;
        let expect: Vec<f32> = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120]
            .iter().map(|&v| v as f32 * inv255).collect();
        assert_eq!(tile.len(), 12);
        for (a, b) in tile.iter().zip(&expect) {
            assert_eq!(a, b);
        }
        // Same-stride path is a straight convert (no de-interleave).
        let tile = TileProcessor::extract_tile_u8(&img, 2, 4, 4, &region);
        assert_eq!(tile.len(), 16);
        assert_eq!(tile[3], 1.0); // px0 alpha 255
        assert_eq!(tile[11], 128.0f32 * (1.0 / 255.0)); // px3 alpha 128
    }

    #[test]
    fn extract_tile_float_partial_stride() {
        // 4x4 RGBA float source (64 values).
        let img: Vec<f32> = (0..64).map(|i| i as f32).collect();
        let region = TileRegion { x: 1, y: 1, width: 2, height: 2, overlap: 0 };
        let tile = TileProcessor::extract_tile(&img, 4, 4, 3, &region);
        // pixel base = (y*4 + x)*4: (1,1)→[20,21,22], (2,1)→[24,25,26],
        // (1,2)→[36,37,38], (2,2)→[40,41,42].
        assert_eq!(tile, vec![20.0, 21.0, 22.0, 24.0, 25.0, 26.0, 36.0, 37.0, 38.0, 40.0, 41.0, 42.0]);
    }

    // ---- pipeline: MockEngine (deterministic 2x nearest) ----

    #[test]
    fn u8_generic_pipeline_matches_whole_image_nearest_upsample() {
        // 5x3 RGB, tile 4 overlap 2 → multi-row/col grid with seams.
        let w = 5i32;
        let h = 3i32;
        let input: Vec<u8> = (0..(w * h * 3) as u32).map(|i| (i % 251) as u8).collect();
        let engine = MockEngine::new();
        let proc = TileProcessor;
        let mut output = vec![0u8; (w as usize * 2) * (h as usize * 2) * 3];

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 3, &engine,
            TileConfig { tile_size: 4, overlap: 2, batch_size: 1 },
            None, &cancel_flag(), true,
        )
        .expect("pipeline must succeed");

        let mut expected = vec![0u8; output.len()];
        engine.nearest_2x(&input, &mut expected, w, h, 3);
        assert_eq!(output, expected, "tiled pipeline must reproduce the whole-image transform");
    }

    #[test]
    fn float_generic_pipeline_normalizes_by_weight_map() {
        let w = 4i32;
        let h = 4i32;
        let input: Vec<f32> = (0..(w * h * 3)).map(|i| (i % 200) as f32 / 255.0).collect();
        let engine = MockEngine::new();
        let proc = TileProcessor;
        let mut output = vec![0f32; (w as usize * 2) * (h as usize * 2) * 3];

        proc.process_image(
            &input, &mut output, w, h, 3, &engine,
            TileConfig { tile_size: 4, overlap: 2, batch_size: 2 },
            None, &cancel_flag(),
        )
        .expect("pipeline must succeed");

        let mut expected = vec![0f32; output.len()];
        engine.nearest_2x(&input, &mut expected, w, h, 3);
        for (i, (a, b)) in output.iter().zip(&expected).enumerate() {
            assert!((a - b).abs() < 1e-4, "pixel {i}: {a} vs {b}");
        }
    }

    #[test]
    fn rgba_alpha_split_roundtrip_through_rgb_model() {
        // RGBA source + 3-channel model: RGB goes through the net, alpha is
        // bicubic-scaled host-side and merged into channel 3.
        let w = 4i32;
        let h = 2i32;
        let mut input = vec![0u8; (w * h * 4) as usize];
        for (i, px) in input.chunks_exact_mut(4).enumerate() {
            px[0] = (i * 7 % 256) as u8;
            px[1] = (i * 13 % 256) as u8;
            px[2] = (i * 29 % 256) as u8;
            px[3] = (i * 53 % 256) as u8;
        }
        let engine = MockEngine::new(); // input_channels = 3
        let proc = TileProcessor;
        let mut output = vec![0u8; (w as usize * 2) * (h as usize * 2) * 4];

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 4, &engine,
            TileConfig { tile_size: 4, overlap: 0, batch_size: 1 },
            None, &cancel_flag(), true,
        )
        .expect("alpha split pipeline must succeed");

        // RGB channels: nearest 2x of the input RGB planes.
        let mut rgb_expected = vec![0u8; (w as usize * 2) * (h as usize * 2) * 3];
        let rgb_input: Vec<u8> = input
            .chunks_exact(4)
            .flat_map(|px| [px[0], px[1], px[2]])
            .collect();
        engine.nearest_2x(&rgb_input, &mut rgb_expected, w, h, 3);
        for (i, px) in output.chunks_exact(4).enumerate() {
            assert_eq!(&px[..3], &rgb_expected[i * 3..i * 3 + 3], "pixel {i} rgb");
        }

        // Alpha channel: bicubic 2x of the input alpha plane.
        let alpha_in: Vec<u8> = input.chunks_exact(4).map(|px| px[3]).collect();
        let mut alpha_expected = vec![0u8; (w as usize * 2) * (h as usize * 2)];
        bicubic_scale_u8(&alpha_in, w as u32, h as u32, &mut alpha_expected, (w * 2) as u32, (h * 2) as u32);
        for (i, px) in output.chunks_exact(4).enumerate() {
            assert_eq!(px[3], alpha_expected[i], "pixel {i} alpha");
        }
    }

    #[test]
    fn alpha_disabled_keeps_alpha_on_net_input_path() {
        // 4-channel model: alpha must flow through the engine untouched.
        let w = 2i32;
        let h = 2i32;
        let mut input = vec![0u8; (w * h * 4) as usize];
        for (i, v) in input.iter_mut().enumerate() {
            *v = (i % 256) as u8;
        }
        let mut engine = MockEngine::new();
        engine.input_channels = 4;
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 4)];

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 4, &engine,
            TileConfig { tile_size: 2, overlap: 0, batch_size: 1 },
            None, &cancel_flag(), false, // alpha bicubic disabled
        )
        .expect("4-channel model pipeline must succeed");

        let mut expected = vec![0u8; output.len()];
        engine.nearest_2x(&input, &mut expected, w, h, 4);
        assert_eq!(output, expected);
    }

    fn output_len(w: i32, h: i32, channels: i32) -> usize {
        (w as usize * 2) * (h as usize * 2) * channels as usize
    }

    #[test]
    fn gpu_fast_path_handles_whole_image_without_generic_pipeline() {
        let w = 6i32;
        let h = 4i32;
        let input: Vec<u8> = (0..(w * h * 3) as u32).map(|i| (i % 251) as u8).collect();
        let engine = MockEngine::with_gpu();
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 3, &engine,
            TileConfig { tile_size: 4, overlap: 0, batch_size: 1 },
            None, &cancel_flag(), true,
        )
        .expect("GPU fast path must succeed");

        // Fast path, not the generic pipeline (which would call process()).
        assert_eq!(engine.process_calls.load(Ordering::Relaxed), 1);
        let mut expected = vec![0u8; output.len()];
        engine.nearest_2x(&input, &mut expected, w, h, 3);
        assert_eq!(output, expected);
    }

    #[test]
    fn gpu_non_oom_failure_is_a_hard_error_no_reload() {
        let w = 4i32;
        let h = 2i32;
        let input: Vec<u8> = (0..(w * h * 3) as u32).map(|i| (i % 251) as u8).collect();
        let mut engine = MockEngine::with_gpu();
        engine.gpu_fail = Some(EngineError::new(5, "vulkan surface lost"));
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];

        let err = proc
            .process_image_u8_tiled(
                &input, &mut output, w, h, 3, &engine,
                TileConfig { tile_size: 4, overlap: 0, batch_size: 1 },
                None, &cancel_flag(), true,
            )
            .expect_err("non-OOM GPU failure must surface, not fall through");
        assert_eq!(err.code, 1);
        assert!(err.message.contains("vulkan surface lost"));
        // Same engine object, no model reload, one failed GPU attempt.
        assert_eq!(engine.load_calls.load(Ordering::Relaxed), 0);
        assert_eq!(engine.process_calls.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn oom_ladder_shrinks_tile_then_succeeds_on_gpu_path() {
        let w = 8i32;
        let h = 8i32;
        let input: Vec<u8> = (0..(w * h * 3) as u32).map(|i| (i % 251) as u8).collect();
        let mut engine = MockEngine::with_gpu();
        // OOM above tile 128: ladder 256 → 128 must stop there.
        engine.oom_above_tile =
            Some((128, EngineError::new(crate::engine::ERR_OUT_OF_MEMORY, "out of memory")));
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 3, &engine,
            TileConfig { tile_size: 256, overlap: 16, batch_size: 1 },
            None, &cancel_flag(), true,
        )
        .expect("OOM ladder must converge on the GPU path");

        assert_eq!(
            *engine.gpu_tile_sizes.lock().unwrap(),
            vec![256, 128],
            "ladder must halve the tile once and succeed at 128"
        );
        assert_eq!(engine.process_calls.load(Ordering::Relaxed), 1);
        // Overlap clamps to tile/4 along the way (16 → 4 at tile 128; the
        // successful 128 call saw overlap = min(16, 256/4)=16 at first retry
        // config then 4 — assert the recorded region overlap instead).
        let mut expected = vec![0u8; output.len()];
        engine.nearest_2x(&input, &mut expected, w, h, 3);
        assert_eq!(output, expected);
    }

    #[test]
    fn oom_ladder_bottoms_out_at_64_then_host_blending() {
        let w = 4i32;
        let h = 2i32;
        let input: Vec<u8> = (0..(w * h * 3) as u32).map(|i| (i % 251) as u8).collect();
        let mut engine = MockEngine::with_gpu();
        // OOM at every size: ladder 256 → 128 → 64, then break to the host
        // path with tile 64 (the inference device is unchanged).
        engine.oom_above_tile =
            Some((0, EngineError::new(crate::engine::ERR_OUT_OF_MEMORY, "out of memory")));
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 3, &engine,
            TileConfig { tile_size: 256, overlap: 16, batch_size: 1 },
            None, &cancel_flag(), true,
        )
        .expect("host blending after ladder bottom must succeed");

        assert_eq!(*engine.gpu_tile_sizes.lock().unwrap(), vec![256, 128, 64]);
        // The generic host path then ran on the same engine (no reload).
        // OOM attempts fail before reaching process(), so only the host
        // batch counts: 4x2 @ tile 64 → one region → one process call.
        assert_eq!(engine.load_calls.load(Ordering::Relaxed), 0);
        assert_eq!(engine.process_calls.load(Ordering::Relaxed), 1);
        let mut expected = vec![0u8; output.len()];
        engine.nearest_2x(&input, &mut expected, w, h, 3);
        assert_eq!(output, expected);
    }

    #[test]
    fn non_oom_gpu_failure_is_a_hard_error() {
        let w = 4i32;
        let h = 2i32;
        let input: Vec<u8> = (0..(w * h * 3) as u32).map(|i| (i % 251) as u8).collect();
        let mut engine = MockEngine::with_gpu();
        engine.gpu_fail = Some(EngineError::new(5, "driver crash"));
        engine.oom_above_tile = None;
        // gpu_fail fires first (record_gpu_attempt checks bounds, then fail).
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];

        let err = proc
            .process_image_u8_tiled(
                &input, &mut output, w, h, 3, &engine,
                TileConfig { tile_size: 4, overlap: 0, batch_size: 1 },
                None, &cancel_flag(), true,
            )
            .expect_err("non-OOM GPU failure must surface");
        assert_eq!(err.code, 1);
        assert!(err.message.contains("driver crash"));
    }

    #[test]
    fn cancel_produces_code_2() {
        let w = 4i32;
        let h = 2i32;
        let input: Vec<u8> = vec![1; (w * h * 3) as usize];
        let engine = MockEngine::new();
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];
        let cancel = cancel_flag();
        cancel.store(true, Ordering::Relaxed);

        let err = proc
            .process_image_u8_tiled(
                &input, &mut output, w, h, 3, &engine,
                TileConfig { tile_size: 4, overlap: 0, batch_size: 1 },
                None, &cancel, true,
            )
            .expect_err("cancelled run must fail");
        assert_eq!(err.code, 2);
        assert_eq!(err.message, "Processing cancelled");
    }

    #[test]
    fn progress_callback_sees_every_tile() {
        let w = 8i32;
        let h = 8i32;
        let input: Vec<u8> = vec![9; (w * h * 3) as usize];
        let engine = MockEngine::new();
        let proc = TileProcessor;
        let mut output = vec![0u8; output_len(w, h, 3)];

        let seen = Arc::new(Mutex::new(Vec::new()));
        let sink = seen.clone();
        let cb = move |p: &ProcessingProgress| {
            sink.lock().unwrap().push((p.completed_tiles, p.total_tiles));
        };

        proc.process_image_u8_tiled(
            &input, &mut output, w, h, 3, &engine,
            TileConfig { tile_size: 4, overlap: 0, batch_size: 1 },
            Some(&cb), &cancel_flag(), true,
        )
        .expect("pipeline must succeed");

        let seen = seen.lock().unwrap();
        assert_eq!(seen.last().copied(), Some((4, 4)), "final report must be 4/4");
        // Monotone progress, one report per tile.
        assert!(seen.windows(2).all(|w| w[0].0 < w[1].0));
    }

    #[test]
    fn invalid_dims_and_buffers_are_rejected() {
        let engine = MockEngine::new();
        let proc = TileProcessor;
        let cancel = cancel_flag();

        let err = proc
            .process_image(&[0.0], &mut [0.0; 8], 0, 2, 1, &engine,
                TileConfig::default(), None, &cancel)
            .expect_err("zero width must be rejected");
        assert_eq!(err.code, 4);

        let err = proc
            .process_image(&[], &mut [0.0; 8], 2, 2, 1, &engine,
                TileConfig::default(), None, &cancel)
            .expect_err("empty buffer must be rejected");
        assert_eq!(err.code, 3);
    }
}

