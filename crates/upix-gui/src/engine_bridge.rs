//! The seam between pages and the upscale engine.
//!
//! `UpscaleBackend::start` runs the whole job on a `std::thread` and reports
//! through an `mpsc::Sender`; the UI drains the channel on the engine thread
//! (a 100 ms tick in the shell) and never blocks. Cancellation is a shared
//! `AtomicBool` checked at every tile boundary — the thread just exits and
//! the UI shows 已取消.
//!
//! - `MockBackend` (`--no-default-features`): PNG in, nearest-neighbor scale
//!   out, per-tile pacing so the progress page has real tiles to count.
//! - `RealBackend` (default `integration` feature): upix-img decode, CPU
//!   `process_image_u8`, SR-marker XMP, upix-img encode. Sibling path
//!   `<stem>_upix_x<scale>.<ext>` so the source file is never overwritten.

use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::thread;

use base64::Engine as _;

/// Where a job's input came from, with everything the UI needs to show it.
#[derive(Clone, Debug, PartialEq)]
pub struct SourceImage {
    pub path: PathBuf,
    /// `data:` URI of the file bytes, for the image element.
    pub display_src: String,
    pub width: i32,
    pub height: i32,
}

/// One pipeline stage (desktop Progress.qml / daemon ServeProgress grammar).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Stage {
    Loading,
    Processing,
    Encoding,
}

impl Stage {
    pub fn label(self) -> &'static str {
        match self {
            Stage::Loading => "读取图片",
            Stage::Processing => "分块推理",
            Stage::Encoding => "编码输出",
        }
    }
}

/// What one finished job produced.
#[derive(Clone, Debug, PartialEq)]
pub struct OutputImage {
    pub path: PathBuf,
    pub display_src: String,
    pub width: i32,
    pub height: i32,
    pub format: String,
    /// Bytes as written to `path` (encoding already applied).
    pub bytes: Arc<Vec<u8>>,
}

/// Events a job streams back to the UI.
#[derive(Clone, Debug)]
pub enum JobEvent {
    Progress {
        stage: Stage,
        done: i32,
        total: i32,
    },
    Done(Box<OutputImage>),
    Failed(String),
}

/// A requested upscale.
#[derive(Clone, Debug)]
pub struct JobSpec {
    pub input_path: PathBuf,
    /// Scale factor (2 or 4).
    pub scale: i32,
    /// "png" | "jpeg" | "webp" | "avif" (normalized).
    pub format: String,
    /// 1..=100, default 90 (config.cpp DEFAULT_OUTPUT_QUALITY).
    pub quality: i32,
    /// Model name, when the user picked one; None = the backend's default.
    pub model: Option<String>,
}

/// Runs one job on its own thread.
pub trait UpscaleBackend: 'static {
    fn start(&self, spec: JobSpec, events: Sender<JobEvent>, cancel: Arc<AtomicBool>);
}

/// `data:` URI for the image element (base64, standard alphabet).
pub fn data_uri(bytes: &[u8], mime: &str) -> String {
    format!("data:{mime};base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes))
}

/// Output path derived like the CLI's: `<dir>/<stem>_upix_x<scale>.<ext>`
/// (CONTRACT pins `upix_core::resolve_output_path` for the real backend).
pub fn derive_output_path(input: &Path, scale: i32, format: &str) -> PathBuf {
    let stem = input
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "image".to_owned());
    input.with_file_name(format!("{stem}_upix_x{scale}.{format}"))
}

/// Reads a PNG's IHDR (width/height) without decoding pixels. Other formats
/// answer (0, 0) until the img crate integration lands.
pub fn probe_dimensions(path: &Path) -> (i32, i32) {
    if let Ok(data) = std::fs::read(path)
        && let Some((w, h)) = png_dimensions(&data)
    {
        return (w, h);
    }
    (0, 0)
}

fn png_dimensions(data: &[u8]) -> Option<(i32, i32)> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(data));
    decoder.set_transformations(png::Transformations::EXPAND);
    let reader = decoder.read_info().ok()?;
    let info = reader.info();
    Some((info.width as i32, info.height as i32))
}

// ── Mock backend ─────────────────────────────────────────────────────────

/// Deterministic stand-in: decode the PNG, nearest-neighbor scale it
/// tile-by-tile at a pace that exercises the progress page, encode like the
/// real pipeline would (PNG or JPEG; WebP/AVIF fall back to PNG with a log
/// line until upix-img lands).
pub struct MockBackend;

impl UpscaleBackend for MockBackend {
    fn start(&self, spec: JobSpec, events: Sender<JobEvent>, cancel: Arc<AtomicBool>) {
        thread::spawn(move || run_mock(spec, events, cancel));
    }
}

fn run_mock(spec: JobSpec, events: Sender<JobEvent>, cancel: Arc<AtomicBool>) {
    let _ = events.send(JobEvent::Progress {
        stage: Stage::Loading,
        done: 0,
        total: 1,
    });

    let Ok(bytes) = std::fs::read(&spec.input_path) else {
        let _ = events.send(JobEvent::Failed(format!(
            "读取失败: {}",
            spec.input_path.display()
        )));
        return;
    };
    let (width, height, channels, pixels) = match decode_png(&bytes) {
        Ok(img) => img,
        Err(e) => {
            let _ = events.send(JobEvent::Failed(format!(
                "模拟引擎仅支持 PNG 输入（真实引擎接入后支持全部格式）: {e}"
            )));
            return;
        }
    };

    let out_w = width * spec.scale;
    let out_h = height * spec.scale;
    // Same geometry the real pipeline uses (tile 256 / overlap 16, config.cpp).
    let (tile, _overlap) = (256, 16);
    let cols = (out_w + tile - 1) / tile;
    let rows = (out_h + tile - 1) / tile;
    let total = cols * rows;

    let _ = events.send(JobEvent::Progress {
        stage: Stage::Processing,
        done: 0,
        total,
    });
    let scaled = nearest_scale(&pixels, width, height, channels, spec.scale);
    for done in 0..total {
        if cancel.load(std::sync::atomic::Ordering::Relaxed) {
            // Canceled: no Done, no Failed — the UI already showed 已取消.
            return;
        }
        thread::sleep(std::time::Duration::from_millis(40));
        let _ = events.send(JobEvent::Progress {
            stage: Stage::Processing,
            done: done as i32 + 1,
            total,
        });
    }

    let _ = events.send(JobEvent::Progress {
        stage: Stage::Encoding,
        done: 0,
        total: 1,
    });
    // The mock keeps RGBA/RGB straight through; encoding picks the container.
    let out_path = derive_output_path(&spec.input_path, spec.scale, &spec.format);
    let encoded = match encode_output(&scaled, out_w, out_h, channels, &spec) {
        Ok((bytes, format)) => {
            if format != spec.format {
                log::warn!(
                    "mock backend encodes PNG only for {format:?}; the real integration uses upix-img"
                );
            }
            (bytes, format)
        }
        Err(e) => {
            let _ = events.send(JobEvent::Failed(format!("编码失败: {e}")));
            return;
        }
    };
    if let Err(e) = std::fs::write(&out_path, &encoded.0) {
        let _ = events.send(JobEvent::Failed(format!("写出失败: {e}")));
        return;
    }
    let _ = events.send(JobEvent::Done(Box::new(OutputImage {
        display_src: data_uri(&encoded.0, "image/png"),
        path: out_path,
        width: out_w,
        height: out_h,
        format: encoded.1,
        bytes: Arc::new(encoded.0),
    })));
}

type RawImage = (i32, i32, i32, Vec<u8>);

fn decode_png(bytes: &[u8]) -> Result<RawImage, String> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(bytes));
    decoder.set_transformations(png::Transformations::EXPAND);
    let mut reader = decoder
        .read_info()
        .map_err(|e| format!("png: {e}"))?;
    let mut buf = vec![0u8; reader.output_buffer_size()];
    let info = reader
        .next_frame(&mut buf)
        .map_err(|e| format!("png: {e}"))?;
    buf.truncate(info.buffer_size());
    let channels = match info.color_type {
        png::ColorType::Grayscale => 1,
        png::ColorType::GrayscaleAlpha => 2,
        png::ColorType::Rgb => 3,
        png::ColorType::Rgba => 4,
        png::ColorType::Indexed => 3,
    };
    // EXPAND leaves 8-bit non-paletted output.
    Ok((info.width as i32, info.height as i32, channels, buf))
}

/// Nearest-neighbor integer scale (deterministic; the real engine's
/// float pipeline is what produces the actual quality).
fn nearest_scale(pixels: &[u8], width: i32, height: i32, channels: i32, scale: i32) -> Vec<u8> {
    let out_w = (width * scale) as usize;
    let out_h = (height * scale) as usize;
    let ch = channels as usize;
    let mut out = vec![0u8; out_w * out_h * ch];
    for oy in 0..out_h {
        let sy = (oy / scale as usize).min(height as usize - 1);
        for ox in 0..out_w {
            let sx = (ox / scale as usize).min(width as usize - 1);
            let src = (sy * width as usize + sx) * ch;
            let dst = (oy * out_w + ox) * ch;
            out[dst..dst + ch].copy_from_slice(&pixels[src..src + ch]);
        }
    }
    out
}

/// Encodes like the real pipeline: PNG always honors the request; JPEG honors
/// quality (RGB/RGBA direct; gray stays Luma); WebP/AVIF are not encodable
/// before the img integration, so they answer PNG bytes under the requested
/// extension (logged).
fn encode_output(
    pixels: &[u8],
    width: i32,
    height: i32,
    channels: i32,
    spec: &JobSpec,
) -> Result<(Vec<u8>, String), String> {
    match spec.format.as_str() {
        "jpeg" | "jpg" => {
            let color = match channels {
                1 => jpeg_encoder::ColorType::Luma,
                3 => jpeg_encoder::ColorType::Rgb,
                _ => jpeg_encoder::ColorType::Rgba, // alpha ignored by the encoder
            };
            let mut out = Vec::new();
            let encoder = jpeg_encoder::Encoder::new(&mut out, spec.quality.clamp(1, 100) as u8);
            encoder
                .encode(pixels, width as u16, height as u16, color)
                .map_err(|e| format!("jpeg: {e}"))?;
            Ok((out, "jpg".to_owned()))
        }
        "png" | "webp" | "avif" => {
            let mut out = Vec::new();
            {
                let mut encoder = png::Encoder::new(&mut out, width as u32, height as u32);
                let color = match channels {
                    1 => png::ColorType::Grayscale,
                    2 => png::ColorType::GrayscaleAlpha,
                    3 => png::ColorType::Rgb,
                    _ => png::ColorType::Rgba,
                };
                encoder.set_color(color);
                encoder.set_depth(png::BitDepth::Eight);
                let mut writer = encoder.write_header().map_err(|e| format!("png: {e}"))?;
                writer
                    .write_image_data(pixels)
                    .map_err(|e| format!("png: {e}"))?;
            }
            out.flush().map_err(|e| format!("png: {e}"))?;
            Ok((out, "png".to_owned()))
        }
        other => Err(format!("不支持的输出格式: {other}")),
    }
}

// ── Real backend (integration feature) ───────────────────────────────────

/// The CONTRACT-pinned pipeline: upix-img decode + orientation bake,
/// upix-core `process_image_u8` (GPU fast path → OOM tile ladder → generic
/// three-stage pipeline) on a CPU-initialized engine, XMP SR marker, upix-img
/// encode. Progress forwards the engine's tile counts 1:1.
#[cfg(feature = "integration")]
pub struct RealBackend;

#[cfg(feature = "integration")]
impl UpscaleBackend for RealBackend {
    fn start(&self, spec: JobSpec, events: Sender<JobEvent>, cancel: Arc<AtomicBool>) {
        thread::spawn(move || run_real(spec, events, cancel));
    }
}

/// The CONTRACT-pinned pipeline: upix-img decode + orientation bake,
/// upix-core `process_image_u8` on a CPU-initialized engine (GPU fast path →
/// OOM tile ladder → generic three-stage pipeline, all inside the core),
/// SR-marker XMP, upix-img encode. Progress forwards tile counts 1:1.
#[cfg(feature = "integration")]
fn run_real(spec: JobSpec, events: Sender<JobEvent>, cancel: Arc<AtomicBool>) {
    let fail = |events: &Sender<JobEvent>, message: String| {
        if cancel.load(std::sync::atomic::Ordering::Relaxed) {
            return;
        }
        let _ = events.send(JobEvent::Failed(message));
    };

    let _ = events.send(JobEvent::Progress {
        stage: Stage::Loading,
        done: 0,
        total: 1,
    });

    // Factory runs once inside initialize_engine. Build the ncnn engine here
    // so a constructor error stays a Result instead of a panic in the closure.
    let prepared = match upix_engine_ffi::NcnnEngine::new() {
        Ok(engine) => engine,
        Err(e) => {
            fail(&events, format!("引擎创建失败: {}", e.message));
            return;
        }
    };
    let slot = std::sync::Mutex::new(Some(prepared));
    let mut engine = match upix_core::initialize_engine(
        move || {
            Box::new(slot.lock().unwrap().take().expect("engine factory runs once"))
                as Box<dyn upix_core::engine::Engine>
        },
        upix_core::EngineStrategy::Cpu,
    ) {
        Ok(engine) => engine,
        Err(e) => {
            fail(&events, format!("引擎初始化失败: {e}"));
            return;
        }
    };

    let model_name = spec.model.clone().unwrap_or_else(|| "NomosUni-SPAN-x2".into());
    let dir = crate::models_local::models_dir();
    let param = dir.join(format!("{model_name}.param"));
    let bin = dir.join(format!("{model_name}.bin"));
    if let Err(e) = engine.load_model(&param, &bin) {
        fail(&events, format!("加载模型失败: {}", e.message));
        return;
    }
    let scale = engine.get_scale_factor().max(spec.scale).max(1);

    let (mut img, orientation) = match upix_img::decode_path(&spec.input_path) {
        Ok(v) => v,
        Err(e) => {
            fail(&events, format!("解码失败: {}", e.message));
            return;
        }
    };
    if let Some(o) = orientation {
        upix_img::apply_orientation(&mut img, o);
    }

    let cfg = upix_core::ProcessingConfig {
        model_name: model_name.clone(),
        output_format: spec.format.clone(),
        output_quality: spec.quality,
        use_gpu: false,
        ..upix_core::ProcessingConfig::default()
    };
    let events_progress = events.clone();
    let progress = move |p: &upix_core::engine::ProcessingProgress| {
        let _ = events_progress.send(JobEvent::Progress {
            stage: Stage::Processing,
            done: p.completed_tiles,
            total: p.total_tiles,
        });
    };
    let scaled = match upix_core::process_image_u8(
        engine.as_ref(),
        &cfg,
        &img.data,
        img.width,
        img.height,
        img.channels,
        Some(&progress),
        &cancel,
    ) {
        Ok(v) => v,
        Err(e) => {
            fail(&events, format!("推理失败: {}", e.message));
            return;
        }
    };
    if cancel.load(std::sync::atomic::Ordering::Relaxed) {
        return;
    }

    let _ = events.send(JobEvent::Progress {
        stage: Stage::Encoding,
        done: 0,
        total: 1,
    });
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let xmp = upix_img::sr_marker::build_xmp(&model_name, scale, ts as i64);
    let out_img = upix_img::Image {
        width: scaled.width,
        height: scaled.height,
        channels: scaled.channels,
        data: scaled.data.clone(),
    };
    let encoded = match upix_img::encode(
        &out_img,
        &cfg.output_format,
        cfg.output_quality,
        &upix_img::EncoderOptions::default(),
        Some(&xmp),
    ) {
        Ok(v) => v,
        Err(e) => {
            fail(&events, format!("编码失败: {}", e.message));
            return;
        }
    };
    // Sibling name, then the core's extension fold (jpeg → jpg). Never the
    // source path: resolve_output_path only swaps the extension.
    let requested = derive_output_path(&spec.input_path, scale, &cfg.output_format);
    let out_path = upix_core::resolve_output_path(&requested, &cfg);
    if let Some(parent) = out_path.parent() {
        if !parent.as_os_str().is_empty() {
            let _ = std::fs::create_dir_all(parent);
        }
    }
    if let Err(e) = std::fs::write(&out_path, &encoded) {
        fail(&events, format!("写出失败: {e}"));
        return;
    }
    let mime = match cfg.output_format.as_str() {
        "jpg" | "jpeg" => "image/jpeg",
        "webp" => "image/webp",
        "avif" => "image/avif",
        _ => "image/png",
    };
    let _ = events.send(JobEvent::Done(Box::new(OutputImage {
        display_src: data_uri(&encoded, mime),
        path: out_path,
        width: scaled.width,
        height: scaled.height,
        format: cfg.output_format,
        bytes: Arc::new(encoded),
    })));
}

#[cfg(all(test, feature = "integration"))]
mod smoke {
    use super::*;

    /// One real CPU upscale when NomosUni-SPAN-x2 is installed. Missing model
    /// skips (same rule as engine-ffi). Proves the GUI pipeline writes a
    /// sibling file and does not replace the source.
    #[test]
    fn cpu_upscale_writes_sibling_png() {
        let param = crate::models_local::models_dir().join("NomosUni-SPAN-x2.param");
        if !param.is_file() {
            eprintln!("skip: NomosUni-SPAN-x2 not installed");
            return;
        }
        let dir = std::env::temp_dir().join(format!("upix-gui-smoke-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let input = dir.join("tiny.png");
        let mut png_bytes = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut png_bytes, 8, 8);
            encoder.set_color(png::ColorType::Rgb);
            encoder.set_depth(png::BitDepth::Eight);
            let mut writer = encoder.write_header().unwrap();
            writer.write_image_data(&[180u8; 8 * 8 * 3]).unwrap();
        }
        std::fs::write(&input, &png_bytes).unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        run_real(
            JobSpec {
                input_path: input.clone(),
                scale: 2,
                format: "png".into(),
                quality: 90,
                model: Some("NomosUni-SPAN-x2".into()),
            },
            tx,
            Arc::new(AtomicBool::new(false)),
        );
        let mut done = None;
        while let Ok(event) = rx.recv() {
            match event {
                JobEvent::Done(output) => done = Some(output),
                JobEvent::Failed(message) => panic!("upscale failed: {message}"),
                JobEvent::Progress { .. } => {}
            }
        }
        let output = done.expect("pipeline ended without Done");
        assert!(output.path.is_file(), "missing {}", output.path.display());
        assert_ne!(output.path, input);
        assert!(output.path.ends_with("tiny_upix_x2.png") || output
            .path
            .file_name()
            .unwrap()
            .to_string_lossy()
            .contains("_upix_x"));
        assert!(output.width >= 16 && output.height >= 16);
        assert!(std::fs::read(&input).unwrap() == png_bytes);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
