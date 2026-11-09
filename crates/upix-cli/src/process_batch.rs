//! Directory batch path — Rust port of `cli/src/process_batch.cpp`.
//!
//! It consumes the same resolved processing configuration as single-image
//! processing so file settings and explicit CLI overrides apply identically.
//! The task helpers capture the effective config and engine-derived tile size.
//!
//! Mode selection mirrors the C++: `--jobs 1` (or default) + no GPU flag runs
//! the plain serial loop over one engine; `--jobs N` on CPU builds an
//! EnginePool; the GPU paths (single-engine pipelined queue and multi-engine
//! VRAM-admitted pool) keep the same prepare/SR/save stage structure.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::Instant;

use upix_core::config::ProcessingConfig;
use upix_core::engine::{DeviceType, Engine};
use upix_core::engine_pool::EnginePool;
use upix_core::{ModelManager, TileSizeOptimizer};
use upix_core::tile_optimizer::ModelConstraints;
use upix_engine_ffi::NcnnEngine;
use upix_img::sr_marker;

use crate::args::ProcessArgs;
use crate::common::{
    emit_json_line, encoder_options_from_wire, infer_format_from_extension, json_escape,
    report_progress, G_CANCEL_REQUESTED,
};

fn engine_factory() -> Box<dyn Engine> {
    match NcnnEngine::new() {
        Ok(e) => Box::new(e),
        Err(e) => panic!("engine create failed: {}", e.message),
    }
}

/// Header-only dimension sniff (PNG IHDR / JPEG SOF / AVIF ftyp) for the VRAM
/// admission control in the GPU directory path. Returns (0,0) when the format
/// is unknown — callers treat that as "no estimate" and rely on the runtime
/// OOM ladder instead.
fn sniff_image_dimensions(path: &Path) -> (u32, u32) {
    use std::io::{Read, Seek, SeekFrom};
    let Ok(mut f) = std::fs::File::open(path) else {
        return (0, 0);
    };
    let mut sig = [0u8; 24];
    let Ok(got) = f.read(&mut sig) else { return (0, 0) };
    let sig = &sig[..got];

    const PNG_SIG: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
    if got >= 24 && &sig[..8] == &PNG_SIG {
        let be32 = |o: usize| {
            (u32::from(sig[o]) << 24)
                | (u32::from(sig[o + 1]) << 16)
                | (u32::from(sig[o + 2]) << 8)
                | u32::from(sig[o + 3])
        };
        return (be32(16), be32(20));
    }

    if got >= 2 && sig[0] == 0xFF && sig[1] == 0xD8 {
        let _ = f.seek(SeekFrom::Start(2));
        loop {
            let mut m = [0u8; 4];
            if f.read_exact(&mut m).is_err() || m[0] != 0xFF {
                break;
            }
            let is_sof = (0xC0..=0xCF).contains(&m[1])
                && m[1] != 0xC4
                && m[1] != 0xC8
                && m[1] != 0xCC;
            let seg_len = (u64::from(m[2]) << 8) | u64::from(m[3]);
            if is_sof {
                let mut sof = [0u8; 5]; // precision + height + width
                if f.read_exact(&mut sof).is_err() {
                    break;
                }
                let h = (u32::from(sof[1]) << 8) | u32::from(sof[2]);
                let w = (u32::from(sof[3]) << 8) | u32::from(sof[4]);
                return (w, h);
            }
            if f.seek(SeekFrom::Current(seg_len as i64 - 2)).is_err() {
                break;
            }
        }
    }

    if got >= 12 && &sig[4..8] == b"ftyp" {
        let mut header = vec![0u8; 256 * 1024];
        if let Ok(n) = f.read(&mut header) {
            return upix_img::sniff_dimensions(&header[..n]);
        }
    }
    (0, 0)
}

/// Tri-state outcome: a marker skip is neither a success nor a failure.
enum PrepareStatus {
    Ok,
    Skipped,
    Failed,
}

struct PreparedTask {
    in_path: String,
    out_path: String,
    width: i32,
    height: i32,
    channels: i32,
    /// decoded frame bytes, for queue byte caps
    pixel_bytes: usize,
    input_image: upix_img::Image,
}

struct PrepareOutcome {
    status: PrepareStatus,
    task: Option<PreparedTask>,
}

struct BatchState {
    config: ProcessingConfig,
    model_name_used: String,
    model_total_size: u64,
    model_input_channels: i32,
    force: bool,
    enable_progress_log: bool,
    batch_start_time: Instant,
    skip_count: AtomicUsize,
    /// Images discovered for this run; the human progress line numbers files
    /// against it ("[3/5] b.jpg") so a multi-image run is attributable.
    total_count: usize,
    /// Files that reached the engine, in the order they started.
    started_count: AtomicUsize,
}

impl BatchState {
    #[allow(clippy::too_many_arguments)]
    fn prepare_task(&self, in_path: &str, out_path: &str) -> PrepareOutcome {
        if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
            return PrepareOutcome { status: PrepareStatus::Failed, task: None };
        }
        // Read once: the SR-marker check needs the raw bytes and the
        // decode below reuses them (same extension-based dispatch as
        // Image::load).
        let raw = match std::fs::read(in_path) {
            Ok(r) => r,
            Err(_) => {
                log::error!(target: "Task", "Failed to load image: {in_path}");
                return PrepareOutcome { status: PrepareStatus::Failed, task: None };
            }
        };
        if !self.force {
            if let Some(marker) = sr_marker::probe(&raw) {
                self.skip_count.fetch_add(1, Ordering::SeqCst);
                log::info!(
                    target: "Task",
                    "Skipped (already super-resolved by this tool): {} [model={} scale={}x ts={}]; --force re-processes",
                    in_path, marker.model, marker.scale, marker.ts
                );
                return PrepareOutcome { status: PrepareStatus::Skipped, task: None };
            }
        }
        let mut image = match upix_img::decode(&raw) {
            Ok(img) => img,
            Err(_) => {
                log::error!(target: "Task", "Failed to load image: {in_path}");
                return PrepareOutcome { status: PrepareStatus::Failed, task: None };
            }
        };

        // Portrait JPEGs: bake the EXIF orientation in right after
        // decode (same rule as the single-image and remote paths).
        let exif_orientation = upix_img::exif::parse_jpeg_exif_orientation(&raw);
        if exif_orientation != 1 && upix_img::apply_orientation(&mut image, exif_orientation) {
            log::info!(target: "Task", "applied EXIF orientation {exif_orientation} for {in_path}");
        }

        if !upix_img::normalize_image_channels(&mut image, self.model_input_channels, in_path) {
            return PrepareOutcome { status: PrepareStatus::Failed, task: None };
        }

        let task = PreparedTask {
            in_path: in_path.to_string(),
            out_path: out_path.to_string(),
            width: image.width,
            height: image.height,
            channels: image.channels,
            pixel_bytes: image.size(),
            input_image: image,
        };
        PrepareOutcome { status: PrepareStatus::Ok, task: Some(task) }
    }

    fn infer_prepared_task(
        &self,
        prepared: PreparedTask,
        eng: &dyn Engine,
    ) -> Option<ProcessedTask> {
        if crate::logging::machine_output_enabled() {
            // The GUI shows which file of the batch is on the engine; a
            // log scrape can't do this (the old line was LOG_DEBUG).
            let name = Path::new(&prepared.in_path)
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default();
            emit_json_line(format!(
                r#"{{"t":"current","file":"{}"}}"#,
                json_escape(&name)
            ));
        } else {
            // Human mode: name the file and its place in the batch. Without
            // this a 5-image run printed five identical "100% (1/1)" lines
            // and there was no way to tell which image was in flight.
            let n = self.started_count.fetch_add(1, Ordering::SeqCst) + 1;
            let name = Path::new(&prepared.in_path)
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_else(|| prepared.in_path.clone());
            log::info!(target: "Engine", "[{n}/{}] {name}", self.total_count);
            if self.enable_progress_log {
                log::info!(target: "Engine", "Processing image: {}", prepared.in_path);
            }
        }

        let mut local_config = self.config.clone();

        if local_config.auto_tile_size {
            log::info!(target: "Task", "Auto-tile for {}", prepared.in_path);
            let mc = ModelConstraints {
                model_name: self.model_name_used.clone(),
                model_size_mb: self.model_total_size / (1024 * 1024),
                min_tile_size: local_config.min_tile_size,
                max_tile_size: local_config.max_tile_size,
                ..Default::default()
            };

            local_config.tile_size = TileSizeOptimizer::find_optimal_tile_size(
                eng,
                &mc,
                &local_config,
                prepared.width,
                prepared.height,
            );
            local_config.auto_tile_size = true;
            log::info!(target: "Task", "Optimal tile size: {}", local_config.tile_size);
        }

        // 64-bit math, then range-check BEFORE the narrowing cast: an
        // overflowed int would make Image() allocate a bogus (often
        // negative → huge) buffer, and the byte product must still fit a
        // usize. The bound is the int payload the rest of the pipeline
        // (TileConfig/Image/ServeResponse) carries — not a memory policy,
        // so previously-working large inputs keep working.
        let scale_factor = eng.get_scale_factor();
        let out_w64 = prepared.width as i64 * scale_factor as i64;
        let out_h64 = prepared.height as i64 * scale_factor as i64;
        let bytes_per_pixel = std::cmp::max(1, prepared.channels) as u64;
        let size_overflow = out_w64 > 0
            && out_h64 > 0
            && (out_w64 as u64) > u64::MAX / (out_h64 as u64 * bytes_per_pixel);
        if out_w64 <= 0
            || out_h64 <= 0
            || out_w64 > i32::MAX as i64
            || out_h64 > i32::MAX as i64
            || size_overflow
        {
            log::error!(
                target: "Task",
                "Upscaled output out of range: {}x{} at {}x",
                prepared.width, prepared.height, scale_factor
            );
            return None;
        }
        let out_width = out_w64 as i32;
        let out_height = out_h64 as i32;

        let cancel = AtomicBool::new(false);
        let result = upix_core::process_image_u8(
            eng,
            &local_config,
            &prepared.input_image.data,
            prepared.width,
            prepared.height,
            prepared.channels,
            Some(&|prog: &upix_core::engine::ProcessingProgress| {
                // Human: stderr line. Machine: JSON event on stdout.
                report_progress(prog, self.batch_start_time);
                if self.enable_progress_log
                    && prog.total_tiles > 0
                    && (prog.completed_tiles % 10 == 0
                        || prog.completed_tiles == prog.total_tiles)
                {
                    let pct = prog.percentage();
                    log::info!(
                        target: "Engine",
                        "Progress: {}/{} tiles ({:.0}%)",
                        prog.completed_tiles, prog.total_tiles, pct
                    );
                }
            }),
            &cancel,
        );

        let scaled = match result {
            Ok(s) => s,
            Err(e) => {
                if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                    log::warn!(target: "Task", "Processing cancelled by signal");
                }
                log::error!(target: "Task", "Processing failed: {}", e.message);
                return None;
            }
        };

        Some(ProcessedTask {
            out_path: prepared.out_path,
            out_width,
            out_height,
            channels: prepared.channels,
            scale: eng.get_scale_factor(),
            pixel_bytes: scaled.data.len(),
            output_image: upix_img::Image {
                width: scaled.width,
                height: scaled.height,
                channels: scaled.channels,
                data: scaled.data,
            },
        })
    }

    fn save_processed_task(&self, processed: ProcessedTask) -> bool {
        let final_out_path = upix_core::resolve_output_path(Path::new(&processed.out_path), &self.config);
        if let Some(parent) = final_out_path.parent() {
            if !parent.as_os_str().is_empty() {
                if let Err(e) = std::fs::create_dir_all(parent) {
                    log::error!(
                        target: "Task",
                        "Cannot create output directory {}: {e}",
                        parent.display()
                    );
                    return false;
                }
            }
        }

        let sr_xmp = sr_marker::build_xmp(
            &self.model_name_used,
            processed.scale,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs() as i64)
                .unwrap_or(0),
        );
        let save_res = upix_img::encode(
            &processed.output_image,
            &final_out_path
                .extension()
                .map(|e| e.to_string_lossy().into_owned())
                .unwrap_or_default(),
            self.config.output_quality,
            &encoder_options_from_wire(&self.config.encoder),
            Some(&sr_xmp),
        )
        .and_then(|bytes| {
            std::fs::write(&final_out_path, bytes)
                .map_err(|e| upix_img::ImgError { code: 1, message: format!("write failed: {e}") })
        });
        if let Err(e) = save_res {
            log::error!(
                target: "Task",
                "Failed to save output: {} -> {}",
                e.message,
                final_out_path.display()
            );
            return false;
        }

        if self.enable_progress_log {
            log::info!(target: "Task", "Completed: {}", final_out_path.display());
        } else if !crate::logging::machine_output_enabled() {
            // Human mode: close the "[3/5] b.jpg" line with where the result
            // went, so the batch output is a readable transcript.
            log::info!(target: "Engine", "Saved: {}", final_out_path.display());
        }
        if crate::logging::machine_output_enabled() {
            emit_json_line(format!(
                r#"{{"t":"saved","path":"{}"}}"#,
                json_escape(&final_out_path.to_string_lossy())
            ));
        }
        true
    }

    fn process_one(&self, in_path: &str, out_path: &str, eng: &dyn Engine) -> TaskResult {
        let prepared = self.prepare_task(in_path, out_path);
        let task = match prepared.status {
            PrepareStatus::Ok => prepared.task.expect("Ok carries a task"),
            PrepareStatus::Skipped => return TaskResult::Skipped,
            PrepareStatus::Failed => return TaskResult::Failure,
        };
        match self.infer_prepared_task(task, eng) {
            Some(processed) => {
                if self.save_processed_task(processed) {
                    TaskResult::Success
                } else {
                    TaskResult::Failure
                }
            }
            None => TaskResult::Failure,
        }
    }
}

struct ProcessedTask {
    out_path: String,
    // Parity with the C++ ProcessedTask surface; the Rust summary prints
    // counts/sizes only, so the dims stay unused today.
    #[allow(dead_code)]
    out_width: i32,
    #[allow(dead_code)]
    out_height: i32,
    #[allow(dead_code)]
    channels: i32,
    /// engine scale factor, for the SR marker
    scale: i32,
    /// encoded-ready frame bytes, for queue byte caps
    #[allow(dead_code)]
    pixel_bytes: usize,
    output_image: upix_img::Image,
}

enum TaskResult {
    Success,
    Failure,
    Skipped,
}

pub fn run_batch(a: &ProcessArgs, effective: crate::common::EffectiveProcessingConfig) -> i32 {
    let input_path = &a.input_path;
    let output_path = &a.output_path;
    let model_name = &a.model_name;
    let models_dir = &a.models_dir;
    let mut config = effective.config.clone();
    let recommend_tile_size = effective.recommend_tile_size;
    let jobs = a.jobs;
    let jobs_explicit = a.jobs_explicit;
    let use_gpu = a.use_gpu;

    // Validate output path
    if output_path.is_empty() {
        log::error!(target: "CLI", "Output path required when input is a directory");
        return 1;
    }
    if Path::new(output_path).exists() && !Path::new(output_path).is_dir() {
        log::error!(target: "CLI", "Output path must be a directory when input is a directory");
        return 1;
    }
    // `upix imgs -o imgs` writes every result over its own input (the
    // relative name is preserved), destroying the originals with no
    // backup. Compare the canonical forms so a symlink or a "./imgs"
    // spelling cannot slip past.
    {
        let in_canon = std::fs::canonicalize(input_path);
        let out_canon = std::fs::canonicalize(output_path);
        if let (Ok(i), Ok(o)) = (in_canon, out_canon) {
            if i == o {
                log::error!(
                    target: "CLI",
                    "Output directory must differ from the input directory — results would overwrite the inputs"
                );
                return 1;
            }
        }
    }
    // Gather image files recursively. Must mirror the decoders Image::load
    // actually ships: .bmp/.tiff used to be enumerated here but always failed
    // at load with "Unsupported image format", so they are not tasks at all.
    // Filesystem errors are collected rather than fatal: an unreadable
    // subdirectory must skip that entry, not abort the whole batch.
    const EXTENSIONS: [&str; 5] = [".png", ".jpg", ".jpeg", ".webp", ".avif"];
    let mut tasks: Vec<(String, String)> = Vec::new();
    {
        let mut stack = vec![PathBuf::from(input_path)];
        while let Some(dir) = stack.pop() {
            let entries = match std::fs::read_dir(&dir) {
                Ok(e) => e,
                Err(e) => {
                    log::warn!(target: "CLI", "Skipping unreadable directory entry: {e}");
                    continue;
                }
            };
            for entry in entries.flatten() {
                let p = entry.path();
                let Ok(ft) = entry.file_type() else { continue };
                if ft.is_dir() {
                    stack.push(p);
                    continue;
                }
                if !ft.is_file() {
                    continue;
                }
                let ext = p
                    .extension()
                    .map(|e| e.to_string_lossy().to_ascii_lowercase())
                    .map(|e| format!(".{e}"))
                    .unwrap_or_default();
                if !EXTENSIONS.contains(&ext.as_str()) {
                    continue;
                }
                // Lexical, not canonicalized-relative: that one canonicalizes
                // both sides, so a symlinked input pointing outside the tree
                // would produce a ".."-laden relative path and the result
                // would be written outside the output directory.
                let rel = match p.strip_prefix(input_path) {
                    Ok(r) => r,
                    Err(_) => {
                        log::warn!(
                            target: "CLI",
                            "Skipping an entry outside the input tree: {}",
                            p.display()
                        );
                        continue;
                    }
                };
                if rel.as_os_str().is_empty() || rel.starts_with("..") {
                    log::warn!(
                        target: "CLI",
                        "Skipping an entry outside the input tree: {}",
                        p.display()
                    );
                    continue;
                }
                let out_file = Path::new(output_path).join(rel);
                tasks.push((p.to_string_lossy().into_owned(), out_file.to_string_lossy().into_owned()));
            }
        }
    }
    if tasks.is_empty() {
        // No inputs is a failure, not a success: scripts branch on the exit
        // code. The output directory is created after this check, so an empty
        // run no longer leaves an empty "<dir>_<model>/" behind.
        log::error!(
            target: "CLI",
            "No image files (.png/.jpg/.jpeg/.webp/.avif) found in {input_path}"
        );
        return 1;
    }
    log::info!(target: "CLI", "Found {} images to process", tasks.len());
    if let Err(e) = std::fs::create_dir_all(output_path) {
        log::error!(target: "CLI", "Cannot create output directory {output_path}: {e}");
        return 1;
    }

    // Largest-first (LPT) scheduling: the biggest image dominates the
    // critical path, so it must start first when workers run in
    // parallel; for a single worker the order does not change the total.
    tasks.sort_by(|a, b| {
        let sa = std::fs::metadata(&a.0).map(|m| m.len()).unwrap_or(0);
        let sb = std::fs::metadata(&b.0).map(|m| m.len()).unwrap_or(0);
        (sa, &a.0).cmp(&(sb, &b.0)).reverse()
    });

    // Model selection
    let mut manager = ModelManager::new(PathBuf::from(models_dir));
    let models = match manager.scan_models() {
        Ok(m) if !m.is_empty() => m,
        Ok(_) => {
            log::error!(target: "Engine", "No models installed in {models_dir}");
            log::error!(
                target: "Engine",
                "Install one with `upix model download <name>` (see `upix model list-remote`)"
            );
            return 1;
        }
        Err(e) => {
            log::error!(target: "Engine", "No models installed in {models_dir} ({})", e.message);
            log::error!(
                target: "Engine",
                "Install one with `upix model download <name>` (see `upix model list-remote`)"
            );
            return 1;
        }
    };

    let selected_model = if model_name.is_empty() {
        log::info!(
            target: "Engine",
            "No model specified, using first available: {}",
            models[0].name
        );
        &models[0]
    } else {
        match ModelManager::find_model(&models, model_name) {
            Some(m) => m,
            None => {
                log::error!(target: "Engine", "Model not found: {model_name}");
                log::error!(target: "Engine", "Available models:");
                for m in &models {
                    log::error!(target: "Engine", "  - {} ({}x)", m.name, m.scale_factor);
                }
                log::error!(
                    target: "Engine",
                    "Install more models with `upix model download <name>` (see `upix model list-remote`)"
                );
                return 1;
            }
        }
    };

    let param_path = selected_model.param_path.clone();
    let bin_path = selected_model.bin_path.clone();
    let mut state = std::sync::Arc::new(BatchState {
        config: config.clone(),
        model_name_used: selected_model.name.clone(),
        model_total_size: selected_model.total_size,
        model_input_channels: std::cmp::max(1, selected_model.input_channels),
        force: a.force,
        enable_progress_log: std::env::var("UPIX_PROGRESS").is_ok_and(|v| v == "1"),
        batch_start_time: Instant::now(),
        skip_count: AtomicUsize::new(0),
        total_count: tasks.len(),
        started_count: AtomicUsize::new(0),
    });
    log::info!(
        target: "Engine",
        "Using model: {} ({}x)",
        selected_model.name, selected_model.scale_factor
    );

    // Preserve a configured output format; infer from the first target only
    // when neither the config file nor CLI selected one.
    if config.output_format.is_empty() && !tasks.is_empty() {
        let inferred = infer_format_from_extension(&tasks[0].1);
        if !inferred.is_empty() {
            config.output_format = upix_core::config::normalize_output_format(&inferred);
            std::sync::Arc::get_mut(&mut state).unwrap().config.output_format = config.output_format.clone();
            log::info!(target: "Config", "Auto-detected output format: {inferred}");
        }
    }

    // One resolved output format is applied to the whole batch, so two
    // inputs can map to the SAME output file — `a.png` + `a.jpg` in one
    // directory, or two symlinks to one target. Both would be processed,
    // both counted as successes, and one result silently overwritten by
    // the other (nondeterministically, since saves run in parallel).
    // Refuse the batch up front instead of losing a file.
    {
        let mut outputs: HashMap<String, String> = HashMap::new();
        for (in_file, out_file) in &tasks {
            let final_path = upix_core::resolve_output_path(Path::new(out_file), &config)
                .to_string_lossy()
                .into_owned();
            if let Some(first_in) = outputs.insert(final_path.clone(), in_file.clone()) {
                log::error!(
                    target: "CLI",
                    "Output collision: {first_in} and {in_file} both map to {final_path} — use separate input trees or a per-file output format"
                );
                return 1;
            }
        }
    }

    // Validate configuration
    let errors = config.validate();
    if !errors.is_empty() {
        log::error!(target: "Config", "Configuration errors:");
        for err in errors {
            log::error!(target: "Config", "  - {err}");
        }
        return 1;
    }

    // GPU directory mode defaults to VRAM-sized multi-engine concurrency
    // (decided inside that branch, where engine #0 reports the budget)
    // with the same prepare/SR/save pipeline stages as -j 1, just N
    // engines in the SR stage; an explicit -j 1 still pins the
    // single-engine variant of that pipeline, and CPU jobs<=1 keeps the
    // plain serial loop.
    let single_engine_mode = jobs <= 1 && !(use_gpu && !jobs_explicit && tasks.len() > 1);
    if single_engine_mode {
        let mut engine = engine_factory();
        let device_type = if use_gpu { DeviceType::GpuVulkan } else { DeviceType::Cpu };
        if let Err(e) = engine.initialize(device_type) {
            log::error!(target: "Engine", "Failed to initialize engine: {}", e.message);
            return 1;
        }
        let initialized_on_gpu = engine.current_device() == DeviceType::GpuVulkan;
        log::info!(
            target: "Engine",
            "Engine initialized on {}",
            if initialized_on_gpu { "GPU" } else { "CPU" }
        );

        // Load model once
        if let Err(e) = engine.load_model(&param_path, &bin_path) {
            log::error!(target: "Engine", "Failed to load model: {}", e.message);
            return 1;
        }
        // Fall back to the filename-parsed scale only when the engine
        // could not detect it from the param graph.
        if !engine.scale_factor_detected() {
            engine.set_scale_factor(selected_model.scale_factor);
        }
        log::info!(target: "Engine", "Model loaded successfully");

        let scale = engine.get_scale_factor();
        log::info!(target: "Engine", "Scale factor: {scale}x");

        if recommend_tile_size {
            config.tile_size = engine
                .get_recommended_tile_size()
                .clamp(config.min_tile_size, config.max_tile_size);
            std::sync::Arc::get_mut(&mut state).unwrap().config.tile_size = config.tile_size;
            log::info!(target: "Engine", "Auto-selected tile size: {}", config.tile_size);
        }

        let mut success_count: usize = 0;
        for (in_file, out_file) in &tasks {
            if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                break;
            }
            if matches!(
                state.process_one(in_file, out_file, engine.as_ref()),
                TaskResult::Success
            ) {
                success_count += 1;
            }
        }

        print_summary(success_count, state.skip_count.load(Ordering::SeqCst), tasks.len());
        let skipped = state.skip_count.load(Ordering::SeqCst);
        return if success_count + skipped == tasks.len() { 0 } else { 1 };
    }

    if use_gpu {
        // GPU multi-engine path: the C++ admission control sizes workers from
        // the primary engine's VRAM budget, then runs the same three-stage
        // pipeline with N engine threads. Engine creation goes through the
        // same factory; the pipelined queue semantics are preserved.
        let primary = engine_factory();
        let mut primary = primary;
        if let Err(e) = primary.initialize(DeviceType::GpuVulkan) {
            log::error!(target: "Engine", "Failed to initialize GPU engine #0: {}", e.message);
            return 1;
        }
        if let Err(e) = primary.load_model(&param_path, &bin_path) {
            log::error!(
                target: "Engine",
                "Failed to load model into GPU engine #0: {}",
                e.message
            );
            return 1;
        }
        if !primary.scale_factor_detected() {
            primary.set_scale_factor(selected_model.scale_factor);
        }

        let mut requested_workers = std::cmp::max(1, jobs) as usize;
        // Admission control (applies to explicit -j too — an overcommitted
        // worker count fails mid-run when concurrent full-output buffers
        // exhaust VRAM, which is far worse than running with fewer workers).
        let budget_mb = primary.get_device_memory_budget_mb();
        let scale = primary.get_scale_factor();
        if budget_mb > 0 {
            let mut needs_mb: Vec<usize> = Vec::with_capacity(tasks.len());
            for t in &tasks {
                let (w, h) = sniff_image_dimensions(Path::new(&t.0));
                if w == 0 || h == 0 {
                    continue;
                }
                // float accumulator + weight map + u8 staging:
                // ~20 B per output pixel (C=3), 5% headroom.
                let out_px = f64::from(w) * f64::from(h) * f64::from(scale) * f64::from(scale);
                needs_mb.push((out_px * 21.0 / (1024.0 * 1024.0)) as usize);
            }
            needs_mb.sort_unstable_by(|a, b| b.cmp(a));
            let mut peak_mb: usize = 0;
            let mut fit: usize = 1;
            for (w, need) in needs_mb.iter().enumerate().take(4) {
                peak_mb += need;
                if peak_mb > (budget_mb as usize) * 9 / 10 {
                    break;
                }
                fit = w + 1;
            }
            if !jobs_explicit {
                log::info!(
                    target: "Engine",
                    "Auto-selected jobs={} from VRAM budget {}MB (projected peak {}MB with 4 workers; use -j to override)",
                    fit, budget_mb, peak_mb
                );
                requested_workers = fit;
            } else if requested_workers > fit {
                log::warn!(
                    target: "Engine",
                    "jobs={} would overcommit VRAM (projected peak {}MB vs budget {}MB), capping to {} workers",
                    requested_workers, peak_mb, budget_mb, fit
                );
                requested_workers = fit;
            }
        }
        let worker_count = std::cmp::min(requested_workers, tasks.len());

        let mut gpu_engines: Vec<Box<dyn Engine>> = Vec::with_capacity(worker_count);
        gpu_engines.push(primary);
        if worker_count > 1 {
            // Degrade to the engines that did come up instead of failing the
            // whole run: VRAM exhaustion on init of the Nth engine leaves
            // 1..N-1 perfectly usable.
            let results: Vec<std::thread::Result<Result<Box<dyn Engine>, String>>> =
                std::thread::scope(|s| {
                    let handles: Vec<_> = (1..worker_count)
                        .map(|i| {
                            let param = &param_path;
                            let bin = &bin_path;
                            s.spawn(move || {
                                let mut e = engine_factory();
                                if let Err(err) = e.initialize(DeviceType::GpuVulkan) {
                                    return Err(format!(
                                        "Failed to initialize GPU engine #{i}: {}",
                                        err.message
                                    ));
                                }
                                if let Err(err) = e.load_model(param, bin) {
                                    return Err(format!(
                                        "Failed to load model into GPU engine #{i}: {}",
                                        err.message
                                    ));
                                }
                                Ok(e)
                            })
                        })
                        .collect();
                    handles.into_iter().map(|h| h.join()).collect()
                });
            for r in results {
                match r {
                    Ok(Ok(e)) => gpu_engines.push(e),
                    Ok(Err(msg)) => {
                        log::warn!(
                            target: "Engine",
                            "{msg}, continuing with {} workers",
                            gpu_engines.len()
                        );
                        break;
                    }
                    Err(_) => {
                        log::warn!(
                            target: "Engine",
                            "engine worker panicked, continuing with {} workers",
                            gpu_engines.len()
                        );
                        break;
                    }
                }
            }
        }
        let worker_count = gpu_engines.len();
        log::info!(target: "Engine", "GPU engine workers ready: {worker_count}");

        // Same auto tile-size selection as the jobs<=1 path: without this,
        // jobs>1 silently used the CLI default (256) while jobs=1 used the
        // engine recommendation, so the tile grid — and therefore the
        // output — depended on --jobs.
        if recommend_tile_size {
            config.tile_size = gpu_engines[0]
                .get_recommended_tile_size()
                .clamp(config.min_tile_size, config.max_tile_size);
            std::sync::Arc::get_mut(&mut state).unwrap().config.tile_size = config.tile_size;
            log::info!(target: "Engine", "Auto-selected tile size: {}", config.tile_size);
        }

        let success_count = run_pipelined(
            std::sync::Arc::clone(&state),
            tasks.clone().into_iter().collect::<std::sync::Arc<[(String, String)]>>(),
            gpu_engines,
        );
        print_summary(
            success_count,
            state.skip_count.load(Ordering::SeqCst),
            tasks.len(),
        );
        let skipped = state.skip_count.load(Ordering::SeqCst);
        return if success_count + skipped == tasks.len() { 0 } else { 1 };
    }

    // CPU EnginePool: fixed worker pool pulling tasks via an atomic
    // index (same shape as the GPU jobs>1 branch). One thread per file
    // would race acquire() with more tasks than engines and stack N
    // engines' default ncnn thread armies.
    // `-j` is only range-checked (1..1024) and every pool engine
    // loads its own copy of the model, so an absurd -j pins
    // gigabytes of RAM for no gain. Clamp to the machine's
    // parallelism (and the task count) and say when it was reduced.
    let requested_workers = std::cmp::max(1, jobs) as usize;
    let hw_threads = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    let worker_count = std::cmp::min(requested_workers, std::cmp::min(tasks.len(), hw_threads));
    if worker_count < requested_workers {
        log::info!(
            target: "Engine",
            "Reduced CPU workers from {requested_workers} to {worker_count} (hardware concurrency / task count)"
        );
    }
    let pool = EnginePool::new(
        worker_count,
        engine_factory,
        &param_path,
        &bin_path,
        DeviceType::Cpu,
    );
    let pool = match pool {
        Ok(p) => p,
        Err(e) => {
            log::error!(target: "Engine", "Failed to create engine pool: {}", e.message);
            return 1;
        }
    };
    log::info!(target: "Engine", "Created engine pool with {worker_count} CPU engines");

    // Same auto tile-size selection as the jobs<=1 path: without this,
    // jobs>1 silently used the CLI default (256) while jobs=1 used the
    // engine recommendation (128 on CPU), so the tile grid — and therefore
    // the output — depended on --jobs.
    if recommend_tile_size {
        let probe = pool.acquire();
        config.tile_size = probe
            .get_recommended_tile_size()
            .clamp(config.min_tile_size, config.max_tile_size);
        drop(probe);
        std::sync::Arc::get_mut(&mut state).unwrap().config.tile_size = config.tile_size;
        log::info!(target: "Engine", "Auto-selected tile size: {}", config.tile_size);
    }

    let next_task = AtomicUsize::new(0);
    let success_count = AtomicUsize::new(0);
    std::thread::scope(|s| {
        for _ in 0..worker_count {
            s.spawn(|| {
                while !G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                    let idx = next_task.fetch_add(1, Ordering::SeqCst);
                    if idx >= tasks.len() {
                        break;
                    }
                    let guard = pool.acquire();
                    let eng: &dyn Engine = &*guard;
                    let ok = match state.process_one(&tasks[idx].0, &tasks[idx].1, eng) {
                        TaskResult::Success => true,
                        TaskResult::Skipped | TaskResult::Failure => false,
                    };
                    // (guard drops here, releasing the engine slot)
                    if ok {
                        success_count.fetch_add(1, Ordering::SeqCst);
                    }
                }
            });
        }
    });

    print_summary(
        success_count.load(Ordering::SeqCst),
        state.skip_count.load(Ordering::SeqCst),
        tasks.len(),
    );
    let skipped = state.skip_count.load(Ordering::SeqCst);
    if success_count.load(Ordering::SeqCst) + skipped == tasks.len() {
        0
    } else {
        1
    }
}

fn print_summary(success: usize, skipped: usize, total: usize) {
    let suffix = if skipped > 0 {
        format!(", {skipped} skipped (already super-resolved)")
    } else {
        String::new()
    };
    log::info!(target: "CLI", "Processing summary: {success}/{total} succeeded{suffix}");
}

/// The pipelined prepare/SR/save stage structure shared by both GPU modes:
/// prepare workers read+decode into the prepared queue, engine threads only
/// run SR, save workers encode+write.
fn run_pipelined(
    state: std::sync::Arc<BatchState>,
    tasks: std::sync::Arc<[(String, String)]>,
    engines: Vec<Box<dyn Engine>>,
) -> usize {
    use std::sync::{Arc, Condvar, Mutex};

    let hw = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    let k_save_workers = if hw > 0 {
        (hw / 8).clamp(2, 4)
    } else {
        2
    };
    let k_prepare_workers = if hw > 0 {
        (hw / 4).clamp(2, 6)
    } else {
        2
    };
    let prepared_queue_limit = std::cmp::max(4, k_prepare_workers * 2);
    let processed_queue_limit = std::cmp::max(2, k_save_workers * 2);
    // Byte caps on top of the count limits: a directory of huge
    // images makes each queued frame worth hundreds of MB, so a
    // count-only bound can pin several GB of decoded inputs /
    // full outputs ahead of the engines and the encoders. A queue
    // always accepts its first item, so one oversized image that
    // alone exceeds the cap still flows.
    const K_PREPARED_QUEUE_BYTE_CAP: usize = 768 << 20;
    const K_PROCESSED_QUEUE_BYTE_CAP: usize = 1024 << 20;
    const K_CANCEL_POLL: std::time::Duration = std::time::Duration::from_millis(50);

    struct Queues {
        prepared: Mutex<PreparedQueues>,
        prepared_cv: Condvar,
        processed: Mutex<ProcessedQueues>,
        processed_cv: Condvar,
    }
    struct PreparedQueues {
        items: std::collections::VecDeque<PreparedTask>,
        bytes: usize,
        done: bool,
    }
    struct ProcessedQueues {
        items: std::collections::VecDeque<ProcessedTask>,
        bytes: usize,
        done: bool,
    }

    let queues = Arc::new(Queues {
        prepared: Mutex::new(PreparedQueues {
            items: std::collections::VecDeque::new(),
            bytes: 0,
            done: false,
        }),
        prepared_cv: Condvar::new(),
        processed: Mutex::new(ProcessedQueues {
            items: std::collections::VecDeque::new(),
            bytes: 0,
            done: false,
        }),
        processed_cv: Condvar::new(),
    });

    let next_prepare = Arc::new(AtomicUsize::new(0));
    let save_success_count = Arc::new(AtomicUsize::new(0));
    let engines = Arc::new(Mutex::new(std::collections::VecDeque::from(engines)));
    let engines_cv = Condvar::new();
    let engines_cv = Arc::new(engines_cv);
    let active_preparers = Arc::new(AtomicUsize::new(k_prepare_workers));

    let mut handles = Vec::new();

    // prepare workers
    for _ in 0..k_prepare_workers {
        let q = Arc::clone(&queues);
        let next = Arc::clone(&next_prepare);
        let state_ref = Arc::clone(&state);
        let tasks_ref = Arc::clone(&tasks);
        let active = Arc::clone(&active_preparers);
        handles.push(std::thread::spawn(move || {
            loop {
                if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                    break;
                }
                let idx = next.fetch_add(1, Ordering::SeqCst);
                if idx >= tasks_ref.len() {
                    break;
                }
                let task = (tasks_ref[idx].0.clone(), tasks_ref[idx].1.clone());
                let prepared = state_ref.prepare_task(&task.0, &task.1);
                let task = match prepared.status {
                    PrepareStatus::Ok => prepared.task.expect("Ok carries a task"),
                    // Skip (already logged + counted) or load
                    // failure (error logged) — never enqueued.
                    PrepareStatus::Skipped | PrepareStatus::Failed => continue,
                };
                let task_bytes = task.pixel_bytes;
                {
                    let mut pq = q.prepared.lock().unwrap();
                    while (pq.items.len() >= prepared_queue_limit
                        || (pq.bytes > 0 && pq.bytes + task_bytes > K_PREPARED_QUEUE_BYTE_CAP))
                        && !G_CANCEL_REQUESTED.load(Ordering::SeqCst)
                    {
                        pq = q.prepared_cv.wait_timeout(pq, K_CANCEL_POLL).unwrap().0;
                    }
                    if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                        break;
                    }
                    pq.bytes += task_bytes;
                    pq.items.push_back(task);
                }
                q.prepared_cv.notify_one();
            }
            if active.fetch_sub(1, Ordering::SeqCst) == 1 {
                let mut pq = q.prepared.lock().unwrap();
                pq.done = true;
                q.prepared_cv.notify_all();
            }
        }));
    }

    // engine threads
    let active_engines = Arc::new(AtomicUsize::new(engines.lock().unwrap().len()));
    let mut engine_handles = Vec::new();
    for _ in 0..engines.lock().unwrap().len() {
        let q = Arc::clone(&queues);
        let pool = Arc::clone(&engines);
        let pool_cv = Arc::clone(&engines_cv);
        let state_ref = Arc::clone(&state);
        let active = Arc::clone(&active_engines);
        engine_handles.push(std::thread::spawn(move || {
            loop {
                // acquire an engine (serialized checkout like EnginePool)
                let mut engs = pool.lock().unwrap();
                while engs.is_empty() {
                    engs = pool_cv.wait(engs).unwrap();
                }
                let eng = engs.pop_front().unwrap();
                drop(engs);

                let prepared;
                {
                    let mut pq = q.prepared.lock().unwrap();
                    while pq.items.is_empty() && !pq.done && !G_CANCEL_REQUESTED.load(Ordering::SeqCst)
                    {
                        pq = q.prepared_cv.wait_timeout(pq, K_CANCEL_POLL).unwrap().0;
                    }
                    if (pq.items.is_empty() && pq.done) || G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                        // return the engine, then exit
                        pool.lock().unwrap().push_front(eng);
                        pool_cv.notify_one();
                        break;
                    }
                    pq.bytes -= pq.items.front().unwrap().pixel_bytes;
                    prepared = pq.items.pop_front().unwrap();
                }
                q.prepared_cv.notify_all();

                let processed = state_ref.infer_prepared_task(prepared, eng.as_ref());

                // return the engine before enqueueing output
                pool.lock().unwrap().push_front(eng);
                pool_cv.notify_one();

                let Some(processed) = processed else { continue };
                let task_bytes = processed.pixel_bytes;
                {
                    let mut pp = q.processed.lock().unwrap();
                    while (pp.items.len() >= processed_queue_limit
                        || (pp.bytes > 0 && pp.bytes + task_bytes > K_PROCESSED_QUEUE_BYTE_CAP))
                        && !G_CANCEL_REQUESTED.load(Ordering::SeqCst)
                    {
                        pp = q.processed_cv.wait_timeout(pp, K_CANCEL_POLL).unwrap().0;
                    }
                    if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                        break;
                    }
                    pp.bytes += task_bytes;
                    pp.items.push_back(processed);
                }
                q.processed_cv.notify_one();
            }
            if active.fetch_sub(1, Ordering::SeqCst) == 1 {
                let mut pp = q.processed.lock().unwrap();
                pp.done = true;
                q.processed_cv.notify_all();
                q.prepared_cv.notify_all();
            }
        }));
    }

    // save workers
    let mut save_handles = Vec::new();
    for _ in 0..k_save_workers {
        let q = Arc::clone(&queues);
        let state_ref = Arc::clone(&state);
        let counter = Arc::clone(&save_success_count);
        save_handles.push(std::thread::spawn(move || {
            loop {
                let processed;
                {
                    let mut pp = q.processed.lock().unwrap();
                    while pp.items.is_empty() && !pp.done && !G_CANCEL_REQUESTED.load(Ordering::SeqCst)
                    {
                        pp = q.processed_cv.wait_timeout(pp, K_CANCEL_POLL).unwrap().0;
                    }
                    if (pp.items.is_empty() && pp.done) || G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                        break;
                    }
                    pp.bytes -= pp.items.front().unwrap().pixel_bytes;
                    processed = pp.items.pop_front().unwrap();
                }
                q.processed_cv.notify_one();

                if state_ref.save_processed_task(processed) {
                    counter.fetch_add(1, Ordering::SeqCst);
                }
            }
        }));
    }

    for h in handles {
        let _ = h.join();
    }
    for h in engine_handles {
        let _ = h.join();
    }
    for h in save_handles {
        let _ = h.join();
    }
    save_success_count.load(Ordering::SeqCst)
}
