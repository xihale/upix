//! `upix <image>` — the main upscale path (the old explicit `process`
//! subcommand is gone; the same options live on the root verb).
//!
//! Single file: try to delegate to a warm daemon first (skips the ~490ms
//! init+load), spawning one if none is running, else init an inline engine and
//! process. Directory: batch over all images (see process_batch.rs).

use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::time::Instant;

use upix_core::config::{
    normalize_output_format, DEFAULT_BATCH_SIZE, DEFAULT_MAX_TILE_SIZE, DEFAULT_MIN_TILE_SIZE,
};
use upix_core::engine_setup::{setup_engine, EngineStrategy};
use upix_core::{process_image_u8, TileSizeOptimizer};
use upix_core::tile_optimizer::ModelConstraints;
use upix_engine_ffi::NcnnEngine;
use upix_img::sr_marker;

use crate::args::ProcessArgs;
use crate::common::{
    derive_default_output, emit_json_line, encoder_options_from_wire,
    infer_format_from_extension, json_escape, report_progress, resolve_processing_config,
    spawn_daemon_and_wait, try_connect_daemon, EffectiveProcessingConfig, G_CANCEL_REQUESTED,
    G_RUNNING,
};
use crate::process_batch::run_batch;

/// Model used to tag a derived default output when the user named none: the
/// first model the manager finds in models_dir — the same "first available"
/// rule setup_engine and run_batch use.
fn first_model_name(models_dir: &str) -> String {
    let mut manager = upix_core::ModelManager::new(PathBuf::from(models_dir));
    if let Ok(scan) = manager.scan_models() {
        if !scan.is_empty() && !manager.available_models().is_empty() {
            return manager.available_models()[0].name.clone();
        }
    }
    "model".to_string()
}

fn engine_factory() -> Box<dyn upix_core::engine::Engine> {
    match NcnnEngine::new() {
        Ok(e) => Box::new(e),
        // The C++ create_ncnn_engine() aborts on failure; the FFI shim's
        // catch-all makes that path unreachable in practice.
        Err(e) => panic!("engine create failed: {}", e.message),
    }
}

pub fn run(a: &ProcessArgs) -> i32 {
    G_RUNNING.store(true, Ordering::SeqCst);
    G_CANCEL_REQUESTED.store(false, Ordering::SeqCst);
    crate::install_signal_handlers();

    // Machine event mode (UPIX_PROGRESS=json): must be on before the first
    // LOG_* so stdout stays a clean event stream. `=1` keeps the human path.
    if std::env::var_os("UPIX_PROGRESS").is_some_and(|v| v == "json") {
        crate::logging::enable_machine_output();
    }

    if a.input_path.is_empty() {
        eprintln!("upix: no input given — try `upix <image>` or `upix -i <image>`");
        return 1;
    }

    let input_path = &a.input_path;
    let output_path = &a.output_path;
    let model_name = &a.model_name;
    let models_dir = &a.models_dir;
    let effective = resolve_processing_config(a);
    let config = &effective.config;
    // Reject invalid fixed settings before delegating: the daemon must receive
    // the same valid settings that inline processing would use.
    if !effective.recommend_tile_size && !config.auto_tile_size {
        let errors = config.validate();
        if !errors.is_empty() {
            for err in errors {
                log::error!(target: "Config", "{err}");
            }
            return 1;
        }
    }

    // ── Device selection ────────────────────────────────────────────────────
    // Explicit CLI wins over the config file: --cpu (mutually exclusive with
    // --gpu) forces a CPU-only engine, --gpu prefers GPU, and a config file's
    // `use_gpu: false` is honored when neither flag was given — it used to be
    // parsed and then silently ignored on the single-image path. CPU-only also
    // skips daemon delegation below: the daemon is a warm GPU engine and
    // ServeRequest carries no device field, so delegating would run the work
    // on the GPU anyway.
    let force_cpu = a.use_cpu || (!a.use_gpu && !config.use_gpu);

    // Check if input is a directory for batch processing
    let is_dir = Path::new(input_path).is_dir();
    log::info!(target: "Debug", "Input path: {} is_dir: {}", input_path, is_dir as i32);

    // ── Already-super-resolved guard (single-file only) ────────────────────
    // Read the file once here: the marker check needs the raw bytes, and the
    // inline path below reuses them for decoding.
    let mut input_bytes: Vec<u8> = Vec::new();
    if !is_dir {
        match std::fs::read(input_path) {
            Ok(raw) => input_bytes = raw,
            Err(e) => {
                log::error!(target: "Engine", "Failed to load image: {e}");
                return 1;
            }
        }
        if !a.force {
            if let Some(marker) = sr_marker::probe(&input_bytes) {
                log::info!(
                    target: "Engine",
                    "Skipped (already super-resolved by this tool): {} [model={} scale={}x ts={}]; re-run with --force to process anyway",
                    input_path, marker.model, marker.scale, marker.ts
                );
                return 0;
            }
        }
    }

    // ── Daemon delegation (single-file only) ────────────────────────────────
    // If a local daemon is running, send it the image and let the warm engine
    // do inference — skipping the ~490 ms init+load fixed cost. If none is
    // running, spawn one in the background and wait for it (it idle-exits on
    // its own later). Falls back silently if the daemon never comes up or the
    // connection fails; on a real daemon-side error returns 1. Auto-tile and
    // nondefault batching stay inline: ServeRequest cannot carry optimizer
    // settings or batch size. Bounds on an unpinned recommended size also
    // require the inline engine to apply them.
    let daemon_supports_config = !config.auto_tile_size
        && config.batch_size == DEFAULT_BATCH_SIZE
        && (!effective.recommend_tile_size
            || (config.min_tile_size == DEFAULT_MIN_TILE_SIZE
                && config.max_tile_size == DEFAULT_MAX_TILE_SIZE));
    if !is_dir && !a.no_daemon && !force_cpu && daemon_supports_config {
        if let Some(code) = delegate_to_daemon(
            &effective,
            input_path,
            output_path,
            model_name,
            models_dir,
            a.force,
        ) {
            return code;
        }
        // else: no daemon / spawn failed → fall through to inline path
    }

    if is_dir {
        // No explicit output → derive a sibling `dir_<model>/`. Copy the args
        // so the const contract holds; the tag model mirrors what run_batch
        // will load (-m when given, else first available).
        let mut batch = a.clone();
        if batch.output_path.is_empty() {
            let model = if model_name.is_empty() {
                first_model_name(models_dir)
            } else {
                model_name.clone()
            };
            batch.output_path = derive_default_output(input_path, &model, true);
            log::info!(target: "Engine", "Output directory: {}", batch.output_path);
        }
        return run_batch(&batch, effective);
    }

    // Single file processing continues...
    log::info!(target: "Engine", "Loading image: {input_path}");

    // Bytes were already read for the marker check above; decode from them
    // (same extension-based dispatch as Image::load).
    let (mut image, orientation) = match upix_img::decode(&input_bytes) {
        Ok(img) => (img, upix_img::exif::parse_jpeg_exif_orientation(&input_bytes)),
        Err(e) => {
            log::error!(target: "Engine", "Failed to load image: {}", e.message);
            return 1;
        }
    };

    // Portrait JPEGs: bake the EXIF orientation transform into the decoded
    // pixels so the upscale runs on the upright frame (mirrors the remote
    // path and the batch path). Non-JPEG / no-EXIF inputs are untouched.
    if orientation != 1 && upix_img::apply_orientation(&mut image, orientation) {
        log::info!(target: "Engine", "applied EXIF orientation {orientation}");
    }

    log::info!(
        target: "Engine",
        "Image loaded: {}x{} ({} channels)",
        image.width, image.height, image.channels
    );

    // Engine init + model load via the shared recipe. ENGINE_DEFAULT mirrors
    // the original `engine->initialize()` (no arg): try GPU, fall back to CPU.
    // --cpu / config use_gpu=false select the CPU-only strategy; --gpu makes
    // the GPU-first strategy explicit (identical to the default, but it must
    // not be silently ignored).
    let strategy = if force_cpu {
        EngineStrategy::Cpu
    } else if a.use_gpu {
        EngineStrategy::GpuThenCpu
    } else {
        EngineStrategy::EngineDefault
    };
    let mut setup = match setup_engine(engine_factory, strategy, models_dir, model_name) {
        Ok(s) => s,
        Err(e) => {
            log::error!(target: "Engine", "{e}");
            return 1;
        }
    };
    let selected_model = setup.selected.clone().expect("setup_engine always selects");
    if model_name.is_empty() {
        log::info!(
            target: "Engine",
            "No model specified, using first available: {}",
            selected_model.name
        );
    }
    let model_name_resolved = selected_model.name.clone();
    let model_input_channels = std::cmp::max(1, selected_model.input_channels);
    log::info!(
        target: "Engine",
        "Using model: {} ({}x)",
        model_name_resolved, selected_model.scale_factor
    );
    // Fall back to the filename-parsed scale only when the engine could not
    // detect it from the param graph — a detected value describes what the
    // net actually outputs; the filename is just a convention.
    if !setup.engine.scale_factor_detected() {
        setup.engine.set_scale_factor(selected_model.scale_factor);
    }

    // No explicit output → derive `img_<model>.png` next to the input.
    let eff_output = if output_path.is_empty() {
        derive_default_output(input_path, &model_name_resolved, false)
    } else {
        output_path.clone()
    };

    log::info!(target: "Engine", "Model loaded successfully");

    let scale = setup.engine.get_scale_factor();
    log::info!(target: "Engine", "Scale factor: {scale}x");

    let mut config = effective.config.clone();
    if effective.recommend_tile_size {
        config.tile_size = setup
            .engine
            .get_recommended_tile_size()
            .clamp(config.min_tile_size, config.max_tile_size);
        log::info!(target: "Engine", "Auto-selected tile size: {}", config.tile_size);
    }

    let input_channels_before = image.channels;
    if !upix_img::normalize_image_channels(&mut image, model_input_channels, input_path) {
        return 1;
    }
    if image.channels != input_channels_before {
        log::info!(
            target: "Engine",
            "Adjusted input channels: {} -> {}",
            input_channels_before, image.channels
        );
    }

    if config.output_format.is_empty() {
        // Auto-infer output format from output file extension
        let inferred = infer_format_from_extension(&eff_output);
        if !inferred.is_empty() {
            config.output_format = normalize_output_format(&inferred);
            log::info!(target: "Config", "Auto-detected output format: {inferred}");
        }
    }

    // 如果启用自动调优，运行基准测试选择最优 tile size
    if config.auto_tile_size {
        log::info!(target: "Config", "Auto-tile enabled, benchmarking...");

        let optimizer = TileSizeOptimizer;
        let model_constraints = ModelConstraints {
            model_name: model_name_resolved.clone(),
            model_size_mb: selected_model.total_size / (1024 * 1024),
            min_tile_size: config.min_tile_size,
            max_tile_size: config.max_tile_size,
            ..Default::default()
        };

        config.tile_size = TileSizeOptimizer::find_optimal_tile_size(
            setup.engine.as_ref(),
            &model_constraints,
            &config,
            image.width,
            image.height,
        );
        config.auto_tile_size = true;
        let _ = optimizer;

        log::info!(target: "Config", "Optimal tile size determined: {}", config.tile_size);
    }

    // 验证配置
    let errors = config.validate();
    if !errors.is_empty() {
        log::error!(target: "Config", "Configuration errors:");
        for err in errors {
            log::error!(target: "Config", "  - {err}");
        }
        return 1;
    }

    // 64-bit math, then range-check BEFORE the narrowing cast (same rule as
    // the batch path): an overflowed int would size the output Image from a
    // bogus value instead of failing loudly.
    let out_w64 = image.width as i64 * scale as i64;
    let out_h64 = image.height as i64 * scale as i64;
    let bytes_per_pixel = std::cmp::max(1, image.channels) as u64;
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
            target: "Engine",
            "Upscaled output out of range: {}x{} at {}x",
            image.width, image.height, scale
        );
        return 1;
    }
    let out_width = out_w64 as i32;
    let out_height = out_h64 as i32;

    log::info!(
        target: "Engine",
        "Processing image with tile_size={} overlap={} batch_size={}...",
        config.tile_size, config.tile_overlap, config.batch_size
    );

    let cancel_flag = std::sync::atomic::AtomicBool::new(false);
    let enable_progress_log_single = std::env::var("UPIX_PROGRESS").is_ok_and(|v| v == "1");

    let process_start_time = Instant::now();
    let progress_cb = move |progress: &upix_core::engine::ProcessingProgress| {
        // Human: always print progress to stderr with ETA.
        // Machine: one JSON event per update on stdout.
        report_progress(progress, process_start_time);
        if enable_progress_log_single && progress.total_tiles > 0 {
            let pct = progress.percentage();
            if progress.completed_tiles % 10 == 0
                || progress.completed_tiles == progress.total_tiles
            {
                log::info!(
                    target: "Engine",
                    "Progress: {}/{} tiles ({:.0}%)",
                    progress.completed_tiles, progress.total_tiles, pct
                );
            }
        }
    };
    let process_result = process_image_u8(
        setup.engine.as_ref(),
        &config,
        &image.data,
        image.width,
        image.height,
        image.channels,
        Some(&progress_cb),
        &cancel_flag,
    );

    let scaled = match process_result {
        Ok(s) => s,
        Err(e) => {
            if G_CANCEL_REQUESTED.load(Ordering::SeqCst) {
                log::warn!(target: "Task", "Processing cancelled by signal");
            }
            log::error!(target: "Engine", "Processing failed: {}", e.message);
            return 1;
        }
    };

    let final_output_path = upix_core::resolve_output_path(Path::new(&eff_output), &config);
    // Match the daemon path: an explicit -o into a not-yet-existing directory
    // creates it instead of failing at open() time.
    if let Some(parent) = final_output_path.parent() {
        if !parent.as_os_str().is_empty() {
            let _ = std::fs::create_dir_all(parent);
        }
    }
    log::info!(target: "Engine", "Inference done, encoding and saving output...");
    let sr_xmp = sr_marker::build_xmp(
        &model_name_resolved,
        scale,
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0),
    );
    let output_image = upix_img::Image {
        width: scaled.width,
        height: scaled.height,
        channels: scaled.channels,
        data: scaled.data,
    };
    // Image::save dispatches on the extension and writes the encoded bytes;
    // the empty-extension default is PNG (same rule as save_to_memory).
    let save_result = upix_img::encode(
        &output_image,
        &final_output_path
            .extension()
            .map(|e| e.to_string_lossy().into_owned())
            .unwrap_or_default(),
        config.output_quality,
        &encoder_options_from_wire(&config.encoder),
        Some(&sr_xmp),
    )
    .and_then(|bytes| {
        std::fs::write(&final_output_path, bytes)
            .map_err(|e| upix_img::ImgError { code: 1, message: format!("write failed: {e}") })
    });
    if let Err(e) = save_result {
        log::error!(target: "Engine", "Failed to save output: {}", e.message);
        return 1;
    }

    if crate::logging::machine_output_enabled() {
        emit_json_line(format!(
            r#"{{"t":"saved","path":"{}"}}"#,
            json_escape(&final_output_path.to_string_lossy())
        ));
        emit_json_line(format!(
            r#"{{"t":"note","message":"{}x{}"}}"#,
            out_width, out_height
        ));
    }
    log::info!(target: "Engine", "Output saved: {}", final_output_path.display());
    log::info!(target: "Engine", "Processing complete: {out_width}x{out_height}");
    0
}

/// Daemon delegation for the single-file path. Returns Some(exit_code) when
/// the daemon handled (or failed) the request; None = fall back to inline.
fn delegate_to_daemon(
    effective: &EffectiveProcessingConfig,
    input_path: &str,
    output_path: &str,
    model_name: &str,
    models_dir: &str,
    force: bool,
) -> Option<i32> {
    let config = &effective.config;

    // Empty → daemon derives: keeps the input's extension when it maps
    // to a known encoder, else .png (same rule as derive_default_output).
    let out_fmt = if !config.output_format.is_empty() {
        config.output_format.clone()
    } else if !output_path.is_empty() {
        infer_format_from_extension(output_path)
    } else {
        String::new()
    };

    // The daemon runs on this host but may have a different cwd, so
    // hand it absolute paths only.
    let in_abs = std::fs::canonicalize(input_path).ok()?;
    let out_abs: Option<PathBuf> = if output_path.is_empty() {
        None
    } else {
        Some(lexically_normal(Path::new(output_path)))
    };

    // 1. Probe: no daemon accepting → try to start one (30 s cap);
    //    give up silently on failure and let the inline path run.
    let sock_path = upix_host::default_serve_socket_path();
    if sock_path.is_empty() {
        return None;
    }
    if !try_connect_daemon(&sock_path)
        && !spawn_daemon_and_wait(
            &sock_path,
            models_dir,
            model_name,
            upix_host::DEFAULT_IDLE_TIMEOUT_S, // args::Daemon{}.idle_timeout_s default
        )
    {
        return None;
    }

    let delegate_start_time = Instant::now();
    log::info!(target: "Engine", "delegating to warm engine daemon...");

    // 2. Request = struct + [input path][output path]. No image bytes
    //    ever cross the socket — the daemon reads and writes the files.
    let args = upix_host::ServeRequestArgs {
        input_path: in_abs,
        output_path: out_abs,
        model_name: (!model_name.is_empty()).then(|| model_name.to_string()),
        output_format: (!out_fmt.is_empty()).then_some(out_fmt),
        // Only the no-config, unpinned case delegates engine recommendation.
        tile_size: if effective.recommend_tile_size {
            0
        } else {
            config.tile_size
        },
        tile_overlap: config.tile_overlap,
        output_quality: config.output_quality,
        encoder: encoder_options_from_wire(&config.encoder),
        force,
    };

    #[cfg(unix)] let uds = upix_host::transport_std::StdUdsFactory;
    // Windows: the daemon endpoint is loopback TCP (transport_std::loopback).
    #[cfg(windows)] let uds = upix_host::transport_std::LoopbackFactory;
    let on_progress = |sp: &upix_net::protocol::ServeProgress| {
        if sp.stage == 0 {
            let prog = upix_core::engine::ProcessingProgress {
                total_tiles: sp.total_tiles as i32,
                completed_tiles: sp.completed_tiles as i32,
                ..Default::default()
            };
            report_progress(&prog, delegate_start_time);
        } else if !crate::logging::machine_output_enabled() {
            {
                use std::io::Write;
                let mut err = std::io::stderr();
                let _ = writeln!(err);
            }
            log::info!(target: "Engine", "Inference done, encoding output...");
        }
    };

    let outcome =
        match upix_host::request(&uds, &NetFramer, Path::new(&sock_path), &args, Some(&on_progress))
        {
            Ok(o) => o,
            Err(e) => {
                // A refused connect before the exchange means we lost a race
                // with daemon shutdown → fall back inline; anything after the
                // request was sent is a real failure (C++ returned 1).
                if e.contains("connect") {
                    return None;
                }
                log::error!(target: "Engine", "{e}");
                return Some(1);
            }
        };

    if outcome.skipped {
        log::info!(target: "Engine", "Skipped (already super-resolved by this tool): {input_path}");
        return Some(0);
    }

    let saved_path = outcome.out_path.to_string_lossy().into_owned();
    if crate::logging::machine_output_enabled() {
        emit_json_line(format!(
            r#"{{"t":"saved","path":"{}"}}"#,
            json_escape(&saved_path)
        ));
        emit_json_line(format!(
            r#"{{"t":"note","message":"{}x{}"}}"#,
            outcome.width, outcome.height
        ));
    }
    log::info!(target: "Engine", "Output saved (via warm daemon): {saved_path}");
    log::info!(
        target: "Engine",
        "Processing complete: {}x{}",
        outcome.width, outcome.height
    );
    Some(0)
}

/// `std::filesystem::path::lexically_normal` approximation: collapse `.` and
/// `..` lexically without touching the filesystem.
fn lexically_normal(p: &Path) -> PathBuf {
    let mut out: PathBuf = PathBuf::new();
    for comp in p.components() {
        match comp {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                out.pop();
            }
            other => out.push(other.as_os_str()),
        }
    }
    if out.as_os_str().is_empty() {
        out.push(".");
    }
    out
}

/// Plaintext frame codec over upix-net (the serve socket is local; the C++
/// daemon used plain frames too).
pub struct NetFramer;

impl upix_host::Framer for NetFramer {
    fn serialize_frame(
        &self,
        ty: upix_net::protocol::MessageType,
        payload: &[u8],
    ) -> Vec<u8> {
        upix_net::frame::serialize_frame(
            upix_net::protocol::MessageHeader {
                magic: upix_net::protocol::MAGIC,
                msg_type: ty,
                payload_length: payload.len() as u32,
                checksum: 0, // filled by serialize
            },
            payload,
        )
    }
    fn parse_header(&self, buf: &[u8]) -> Option<upix_net::protocol::MessageHeader> {
        upix_net::frame::parse_header(buf)
    }
    fn verify_frame(&self, header: &[u8; 14], payload: &[u8]) -> bool {
        upix_net::frame::verify_frame(header, payload)
    }
}
