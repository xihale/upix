//! Engine bridge: worker-thread image decode/upscale/encode with warm daemon delegation
//! (`upix_host::serve::request`) or in-process NCNN (`upix_engine_ffi` + `upix_core`).
//! Preserves the `JobBackend` trait (`RealBackend` and `MockBackend`) and tests.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc::Sender, Arc};
use std::time::{Duration, Instant};

use base64::Engine as _;

use crate::models_local::scale_from_filename;

/// Output container format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OutputFormat {
    Png,
    Jpg,
    Webp,
    Avif,
}

impl OutputFormat {
    pub fn ext(self) -> &'static str {
        match self {
            Self::Png => "png",
            Self::Jpg => "jpg",
            Self::Webp => "webp",
            Self::Avif => "avif",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::Png => "PNG",
            Self::Jpg => "JPG",
            Self::Webp => "WebP",
            Self::Avif => "AVIF",
        }
    }

    pub fn mime(self) -> &'static str {
        match self {
            Self::Png => "image/png",
            Self::Jpg => "image/jpeg",
            Self::Webp => "image/webp",
            Self::Avif => "image/avif",
        }
    }

    pub fn supports_quality(self) -> bool {
        !matches!(self, Self::Png)
    }

    pub fn supports_chroma(self) -> bool {
        matches!(self, Self::Jpg | Self::Avif)
    }
}

/// Chroma subsampling mode for JPG / AVIF.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChromaSubsampling {
    Yuv444,
    Yuv422,
    Yuv420,
}

impl ChromaSubsampling {
    pub fn label(self) -> &'static str {
        match self {
            Self::Yuv444 => "4:4:4",
            Self::Yuv422 => "4:2:2",
            Self::Yuv420 => "4:2:0",
        }
    }

    pub fn to_img_subsampling(self) -> i32 {
        match self {
            Self::Yuv444 => 444,
            Self::Yuv422 => 422,
            Self::Yuv420 => 420,
        }
    }
}

/// Request sent to `JobBackend::spawn`.
#[derive(Clone, Debug)]
pub struct JobRequest {
    pub inputs: Vec<PathBuf>,
    pub model_name: String,
    pub models_dir: PathBuf,
    pub format: OutputFormat,
    pub quality: i32,
    pub chroma: ChromaSubsampling,
    pub avif_speed: i32,
    pub custom_out_dir: Option<PathBuf>,
    pub use_gpu: bool,
    pub tile_size: i32,
    pub force: bool,
    pub use_daemon: bool,
    pub cancel: Arc<AtomicBool>,
}

/// Result for a single completed item in a batch.
#[derive(Clone, Debug)]
pub struct JobItemOutcome {
    pub index: usize,
    pub input_path: PathBuf,
    pub output_path: PathBuf,
    pub out_w: u32,
    pub out_h: u32,
    pub skipped: bool,
    pub elapsed_ms: u64,
}

/// Events emitted from worker thread back to the UI `Store`.
#[derive(Clone, Debug)]
pub enum EngineEvent {
    Progress {
        item_index: usize,
        item_total: usize,
        fraction: f32,
        stage: String,
    },
    ItemDone(JobItemOutcome),
    BatchDone {
        completed: usize,
        skipped: usize,
        elapsed_ms: u64,
    },
    Cancelled,
    Failed(String),
}

/// Pluggable job execution backend.
pub trait JobBackend: Send + Sync {
    fn spawn(&self, req: JobRequest, tx: Sender<EngineEvent>);
}

/// Probes image dimensions without keeping decoded pixels in memory.
pub fn probe_dimensions(path: &Path) -> Option<(u32, u32)> {
    #[cfg(feature = "integration")]
    {
        if let Ok((img, _)) = upix_img::decode_path(path) {
            return Some((img.width as u32, img.height as u32));
        }
    }
    let bytes = std::fs::read(path).ok()?;
    probe_header_dims(&bytes)
}

fn probe_header_dims(bytes: &[u8]) -> Option<(u32, u32)> {
    // PNG IHDR
    if bytes.len() >= 24 && &bytes[0..8] == b"\x89PNG\r\n\x1a\n" {
        let w = u32::from_be_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]);
        let h = u32::from_be_bytes([bytes[20], bytes[21], bytes[22], bytes[23]]);
        return Some((w, h));
    }
    None
}

/// Encodes raw file bytes as a base64 `data:` URI.
pub fn data_uri(bytes: &[u8], mime: &str) -> String {
    format!(
        "data:{mime};base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    )
}

/// Computes target output path for an input image (`<stem>_<model>.<ext>` or `<stem>_upix_x<scale>.<ext>`).
pub fn compute_output_path(
    input: &Path,
    model_name: &str,
    scale: i32,
    format: OutputFormat,
    custom_out_dir: Option<&Path>,
) -> PathBuf {
    let stem = input
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "output".into());
    let suffix = if model_name.is_empty() {
        format!("upix_x{scale}")
    } else {
        model_name.to_owned()
    };
    let filename = format!("{stem}_{suffix}.{}", format.ext());
    if let Some(dir) = custom_out_dir {
        dir.join(filename)
    } else if let Some(parent) = input.parent() {
        parent.join(filename)
    } else {
        PathBuf::from(filename)
    }
}

/// Real backend backed by `upix-core`, `upix-engine-ffi`, `upix-img`, and `upix-host::serve`.
#[cfg(feature = "integration")]
pub struct RealBackend;

#[cfg(feature = "integration")]
impl JobBackend for RealBackend {
    fn spawn(&self, req: JobRequest, tx: Sender<EngineEvent>) {
        std::thread::spawn(move || {
            if let Err(msg) = run_real_batch(req, &tx) {
                let _ = tx.send(EngineEvent::Failed(msg));
            }
        });
    }
}

#[cfg(feature = "integration")]
pub fn run_internal_daemon_from_argv(argv: &[String]) -> u8 {
    use upix_host::{ServeConfig, ServeDevice};
    if argv.len() < 7 || argv[1] != "--upix-internal-daemon" {
        return 1;
    }
    let socket_path = argv[2].clone();
    let models_dir = PathBuf::from(&argv[3]);
    let model_name = argv[4].clone();
    let device = match argv[5].as_str() {
        "cpu" => ServeDevice::Cpu,
        _ => ServeDevice::Gpu,
    };
    let idle_timeout_s = argv[6].parse::<i64>().unwrap_or(600);
    let pid_path = format!("{socket_path}.pid");

    let cfg = ServeConfig {
        socket_path: socket_path.clone(),
        models_dir: models_dir.clone(),
        model_name,
        device,
        idle_timeout_s,
        backend: Arc::new(crate::host_bridge::real_host::FfiBackendShim { models_dir }),
        framer: Arc::new(crate::host_bridge::real_host::NetFramer),
        uds: Box::new(upix_host::transport_std::StdUdsFactory),
    };

    let uds: Box<dyn upix_host::seams::UdsFactory> =
        Box::new(upix_host::transport_std::StdUdsFactory);
    if uds.listen(Path::new(&socket_path)).is_err() {
        return 1;
    }
    let _ = upix_host::write_pid_file(&socket_path);
    let Ok(handle) = upix_host::run_daemon(cfg) else {
        let _ = std::fs::remove_file(&pid_path);
        return 1;
    };
    while Path::new(&socket_path).exists() {
        std::thread::sleep(Duration::from_millis(100));
    }
    handle.stop();
    let _ = std::fs::remove_file(&pid_path);
    0
}

#[cfg(feature = "integration")]
fn run_real_batch(req: JobRequest, tx: &Sender<EngineEvent>) -> Result<(), String> {
    use upix_core::{initialize_engine, process_image_u8, EngineStrategy, ProcessingConfig};

    let t_batch = Instant::now();
    let total = req.inputs.len();
    if total == 0 {
        return Err("没有待处理的图片".into());
    }

    let scale_hint = scale_from_filename(&req.model_name).unwrap_or(2);
    let param = req.models_dir.join(format!("{}.param", req.model_name));
    let bin = req.models_dir.join(format!("{}.bin", req.model_name));
    if !param.exists() || !bin.exists() {
        return Err(format!(
            "模型文件缺失：{}.param / .bin（目录 {}）",
            req.model_name,
            req.models_dir.display()
        ));
    }

    let sub = req.chroma.to_img_subsampling();
    let enc_opts = upix_img::EncoderOptions {
        png_level: 6,
        jpeg_subsampling: sub,
        webp_method: 4,
        webp_lossless: req.quality >= 100,
        avif_speed: req.avif_speed.clamp(0, 10),
        avif_subsampling: sub,
        avif_threads: 0,
    };

    let mut completed = 0usize;
    let mut skipped_count = 0usize;

    // Lazy engine initialization for in-process fallback
    let mut local_engine: Option<Box<dyn upix_core::engine::Engine>> = None;

    for (idx, input_path) in req.inputs.iter().enumerate() {
        if req.cancel.load(Ordering::Relaxed) {
            let _ = tx.send(EngineEvent::Cancelled);
            return Ok(());
        }

        let t_item = Instant::now();
        let out_path = compute_output_path(
            input_path,
            &req.model_name,
            scale_hint,
            req.format,
            req.custom_out_dir.as_deref(),
        );
        if let Some(parent) = out_path.parent() {
            if !parent.as_os_str().is_empty() {
                let _ = std::fs::create_dir_all(parent);
            }
        }

        // Try warm daemon first when enabled
        if req.use_daemon {
            let sock = upix_host::serve::default_serve_socket_path();
            if upix_host::serve::status(&sock).running {
                let _ = tx.send(EngineEvent::Progress {
                    item_index: idx,
                    item_total: total,
                    fraction: 0.15,
                    stage: format!("常驻进程推理 ({}/{})", idx + 1, total),
                });
                let args = upix_host::serve::ServeRequestArgs {
                    input_path: input_path.clone(),
                    output_path: Some(out_path.clone()),
                    model_name: Some(req.model_name.clone()),
                    output_format: Some(req.format.ext().to_string()),
                    tile_size: req.tile_size,
                    tile_overlap: 16,
                    output_quality: req.quality,
                    encoder: enc_opts,
                    force: req.force,
                };
                let tx_prog = tx.clone();
                let on_prog = move |sp: &upix_net::protocol::ServeProgress| {
                    if sp.total_tiles > 0 {
                        let frac = (sp.completed_tiles as f32) / (sp.total_tiles as f32);
                        let _ = tx_prog.send(EngineEvent::Progress {
                            item_index: idx,
                            item_total: total,
                            fraction: 0.05 + frac.clamp(0.0, 1.0) * 0.9,
                            stage: format!(
                                "常驻推理 ({}/{}) · {}/{}",
                                idx + 1,
                                total,
                                sp.completed_tiles,
                                sp.total_tiles
                            ),
                        });
                    }
                };
                if let Ok(outcome) = upix_host::serve::request(
                    &upix_host::transport_std::StdUdsFactory,
                    &crate::host_bridge::real_host::NetFramer,
                    Path::new(&sock),
                    &args,
                    Some(&on_prog),
                ) {
                    if outcome.skipped {
                        skipped_count += 1;
                    } else {
                        completed += 1;
                    }
                    let final_out = if outcome.out_path.as_os_str().is_empty() {
                        input_path.clone()
                    } else {
                        outcome.out_path
                    };
                    let _ = tx.send(EngineEvent::ItemDone(JobItemOutcome {
                        index: idx,
                        input_path: input_path.clone(),
                        output_path: final_out,
                        out_w: outcome.width.max(0) as u32,
                        out_h: outcome.height.max(0) as u32,
                        skipped: outcome.skipped,
                        elapsed_ms: t_item.elapsed().as_millis() as u64,
                    }));
                    continue;
                }
            }
        }

        // In-process execution
        let raw_bytes = std::fs::read(input_path)
            .map_err(|e| format!("读取失败 {}: {e}", input_path.display()))?;

        // Check SR marker unless force is true
        if !req.force {
            if let Some(marker) = upix_img::sr_marker::probe(&raw_bytes) {
                let (iw, ih) = probe_dimensions(input_path).unwrap_or((0, 0));
                log::info!(
                    "跳过已超分图片 {} (model={}, scale={})",
                    input_path.display(),
                    marker.model,
                    marker.scale
                );
                skipped_count += 1;
                let _ = tx.send(EngineEvent::ItemDone(JobItemOutcome {
                    index: idx,
                    input_path: input_path.clone(),
                    output_path: input_path.clone(),
                    out_w: iw,
                    out_h: ih,
                    skipped: true,
                    elapsed_ms: t_item.elapsed().as_millis() as u64,
                }));
                continue;
            }
        }

        let _ = tx.send(EngineEvent::Progress {
            item_index: idx,
            item_total: total,
            fraction: 0.02,
            stage: format!("解码图片 ({}/{})", idx + 1, total),
        });

        let (mut decoded, orientation) =
            upix_img::decode_path(input_path).map_err(|e| format!("解码失败：{}", e.message))?;
        if let Some(o) = orientation {
            upix_img::apply_orientation(&mut decoded, o);
        }

        if local_engine.is_none() {
            let _ = tx.send(EngineEvent::Progress {
                item_index: idx,
                item_total: total,
                fraction: 0.05,
                stage: format!("加载模型 {}", req.model_name),
            });
            let prepared = upix_engine_ffi::NcnnEngine::new()
                .map_err(|e| format!("引擎创建失败：{}", e.message))?;
            let slot = std::sync::Mutex::new(Some(prepared));
            let strategy = if req.use_gpu {
                EngineStrategy::GpuThenCpu
            } else {
                EngineStrategy::Cpu
            };
            let mut engine = initialize_engine(
                move || {
                    Box::new(slot.lock().unwrap().take().expect("engine factory runs once"))
                        as Box<dyn upix_core::engine::Engine>
                },
                strategy,
            )
            .map_err(|e| format!("引擎初始化失败：{e}"))?;
            engine
                .load_model(&param, &bin)
                .map_err(|e| format!("加载模型失败：{}", e.message))?;
            local_engine = Some(engine);
        }

        let engine = local_engine.as_ref().unwrap();
        let scale = engine.get_scale_factor().max(scale_hint).max(1);
        let cfg = ProcessingConfig {
            model_name: req.model_name.clone(),
            output_format: req.format.ext().to_string(),
            output_quality: req.quality,
            tile_size: req.tile_size,
            use_gpu: req.use_gpu,
            ..ProcessingConfig::default()
        };

        let tx_step = tx.clone();
        let progress_cb = move |p: &upix_core::engine::ProcessingProgress| {
            let frac = if p.total_tiles > 0 {
                (p.completed_tiles as f32) / (p.total_tiles as f32)
            } else {
                p.percentage() / 100.0
            };
            let _ = tx_step.send(EngineEvent::Progress {
                item_index: idx,
                item_total: total,
                fraction: 0.05 + frac.clamp(0.0, 1.0) * 0.85,
                stage: format!(
                    "分块推理 ({}/{}) · {}/{}",
                    idx + 1,
                    total,
                    p.completed_tiles,
                    p.total_tiles
                ),
            });
        };

        let scaled = process_image_u8(
            engine.as_ref(),
            &cfg,
            &decoded.data,
            decoded.width,
            decoded.height,
            decoded.channels,
            Some(&progress_cb),
            &req.cancel,
        )
        .map_err(|e| format!("推理失败：{}", e.message))?;

        if req.cancel.load(Ordering::Relaxed) {
            let _ = tx.send(EngineEvent::Cancelled);
            return Ok(());
        }

        let out_w = scaled.width as u32;
        let out_h = scaled.height as u32;
        let out_img = upix_img::Image {
            width: scaled.width,
            height: scaled.height,
            channels: scaled.channels,
            data: scaled.data,
        };

        let _ = tx.send(EngineEvent::Progress {
            item_index: idx,
            item_total: total,
            fraction: 0.93,
            stage: format!("编码 {}", req.format.label()),
        });

        let now_ts = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0);
        let xmp = upix_img::sr_marker::build_xmp(&req.model_name, scale, now_ts);
        let encoded = upix_img::encode(
            &out_img,
            req.format.ext(),
            req.quality,
            &enc_opts,
            Some(&xmp),
        )
        .map_err(|e| format!("编码失败：{}", e.message))?;

        std::fs::write(&out_path, &encoded)
            .map_err(|e| format!("保存失败 {}: {e}", out_path.display()))?;

        completed += 1;
        let _ = tx.send(EngineEvent::ItemDone(JobItemOutcome {
            index: idx,
            input_path: input_path.clone(),
            output_path: out_path,
            out_w,
            out_h,
            skipped: false,
            elapsed_ms: t_item.elapsed().as_millis() as u64,
        }));
    }

    let _ = tx.send(EngineEvent::BatchDone {
        completed,
        skipped: skipped_count,
        elapsed_ms: t_batch.elapsed().as_millis() as u64,
    });
    Ok(())
}

/// Deterministic mock backend for non-integration / test runs.
pub struct MockBackend;

impl JobBackend for MockBackend {
    fn spawn(&self, req: JobRequest, tx: Sender<EngineEvent>) {
        std::thread::spawn(move || {
            let t_batch = Instant::now();
            let total = req.inputs.len();
            let scale = scale_from_filename(&req.model_name).unwrap_or(2) as u32;
            let mut completed = 0usize;

            for (idx, input_path) in req.inputs.iter().enumerate() {
                for step in 1..=5 {
                    if req.cancel.load(Ordering::Relaxed) {
                        let _ = tx.send(EngineEvent::Cancelled);
                        return;
                    }
                    let frac = (step as f32) / 5.0;
                    let _ = tx.send(EngineEvent::Progress {
                        item_index: idx,
                        item_total: total,
                        fraction: frac,
                        stage: format!("模拟推理 ({}/{}) · tile {step}/5", idx + 1, total),
                    });
                    std::thread::sleep(Duration::from_millis(30));
                }
                let (iw, ih) = probe_dimensions(input_path).unwrap_or((512, 512));
                let out_path = compute_output_path(
                    input_path,
                    &req.model_name,
                    scale as i32,
                    req.format,
                    req.custom_out_dir.as_deref(),
                );
                let _ = std::fs::copy(input_path, &out_path);
                completed += 1;
                let _ = tx.send(EngineEvent::ItemDone(JobItemOutcome {
                    index: idx,
                    input_path: input_path.clone(),
                    output_path: out_path,
                    out_w: iw * scale,
                    out_h: ih * scale,
                    skipped: false,
                    elapsed_ms: 150,
                }));
            }

            let _ = tx.send(EngineEvent::BatchDone {
                completed,
                skipped: 0,
                elapsed_ms: t_batch.elapsed().as_millis() as u64,
            });
        });
    }
}

#[cfg(all(test, feature = "integration"))]
mod smoke {
    use super::*;
    use std::sync::mpsc;

    #[test]
    fn cpu_upscale_writes_sibling_png() {
        let dir = crate::models_local::models_dir();
        let (groups, _) = crate::models_local::scan_installed_groups(&dir);
        let Some(first) = groups.iter().flat_map(|g| g.items.iter()).next() else {
            eprintln!("smoke skipped: no models installed in {}", dir.display());
            return;
        };

        let tmp = std::env::temp_dir().join(format!("upix-gui-smoke-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&tmp);
        let src_path = tmp.join("tiny.png");
        let tiny = upix_img::Image {
            width: 8,
            height: 8,
            channels: 3,
            data: vec![120u8; 8 * 8 * 3],
        };
        let png = upix_img::encode(
            &tiny,
            "png",
            95,
            &upix_img::EncoderOptions::default(),
            None,
        )
        .expect("encode tiny png");
        std::fs::write(&src_path, &png).expect("write tiny png");

        let (tx, rx) = mpsc::channel();
        RealBackend.spawn(
            JobRequest {
                inputs: vec![src_path.clone()],
                model_name: first.name.clone(),
                models_dir: dir,
                format: OutputFormat::Png,
                quality: 95,
                chroma: ChromaSubsampling::Yuv420,
                avif_speed: 6,
                custom_out_dir: None,
                use_gpu: false,
                tile_size: 0,
                force: true,
                use_daemon: false,
                cancel: Arc::new(AtomicBool::new(false)),
            },
            tx,
        );

        let mut item_done = None;
        while let Ok(ev) = rx.recv_timeout(Duration::from_secs(30)) {
            match ev {
                EngineEvent::ItemDone(out) => item_done = Some(out),
                EngineEvent::BatchDone { .. } => break,
                EngineEvent::Failed(e) => panic!("smoke failed: {e}"),
                EngineEvent::Cancelled => panic!("unexpected cancel"),
                EngineEvent::Progress { .. } => {}
            }
        }
        let out = item_done.expect("item done");
        assert!(out.output_path.exists());
        assert_eq!(out.out_w, 8 * (first.scale as u32));
        assert_eq!(out.out_h, 8 * (first.scale as u32));
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
