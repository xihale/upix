//! Pure-logic tests: the compare clamp/seam math, the QR pipeline, the mock
//! job lifecycle (progress ordering, cancel), the scene table and the model
//! scan. No engine needed — the real UI is exercised by `nsh run`.

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::sync::mpsc;

use upix_gui::engine_bridge::{
    compute_output_path, ChromaSubsampling, EngineEvent, JobBackend, JobRequest, MockBackend,
    OutputFormat,
};
use upix_gui::models_local;
use upix_gui::pages::workbench::{clamp_pos, divider_px, GRAB_BAND_PX, POS_MAX, POS_MIN};
use upix_gui::qr;

fn req(path: &std::path::Path, cancel: Arc<AtomicBool>) -> JobRequest {
    JobRequest {
        inputs: vec![path.to_path_buf()],
        model_name: "NomosUni-SPAN-x2".into(),
        models_dir: PathBuf::from("/tmp/upix-test-models"),
        format: OutputFormat::Png,
        quality: 90,
        chroma: ChromaSubsampling::Yuv444,
        avif_speed: 6,
        custom_out_dir: None,
        use_gpu: false,
        tile_size: 256,
        use_daemon: false,
        force: true,
        cancel,
    }
}

fn tiny_png(path: &std::path::Path, w: u32, h: u32) {
    let mut bytes = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut bytes, w, h);
        encoder.set_color(png::ColorType::Rgb);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().unwrap();
        let data = vec![90u8; (w * h * 3) as usize];
        writer.write_image_data(&data).unwrap();
    }
    std::fs::write(path, bytes).unwrap();
}

#[test]
fn compare_clamp_matches_desktop() {
    // Desktop drag clamps 0.02..0.98; nudge steps stay inside.
    assert_eq!(clamp_pos(0.5), 0.5);
    assert_eq!(clamp_pos(-0.4), POS_MIN);
    assert_eq!(clamp_pos(1.4), POS_MAX);
    assert_eq!(clamp_pos(0.0), POS_MIN);
    assert_eq!(clamp_pos(1.0), POS_MAX);
    assert!((GRAB_BAND_PX - 14.0).abs() < f32::EPSILON);
}

#[test]
fn divider_is_one_rounded_column() {
    assert_eq!(divider_px(300.0, 0.62), 186.0);
    assert_eq!(divider_px(301.0, 0.5), 151.0);
    assert_eq!(divider_px(0.0, 0.9), 0.0);
}

#[test]
fn qr_renders_real_payload_to_png() {
    let payload = format!(
        r#"{{"v":2,"ip":"192.168.1.20","port":9177,"fp":"AB12CD34EF567890","pk":"{}"}}"#,
        "A".repeat(44)
    );
    let png_bytes = qr::qr_png(&payload).expect("qr renders");
    let mut decoder = png::Decoder::new(std::io::Cursor::new(&png_bytes));
    decoder.set_transformations(png::Transformations::EXPAND);
    let mut reader = decoder.read_info().unwrap();
    let (w, h) = (reader.info().width, reader.info().height);
    assert_eq!(w, h);
    assert!(w >= 21 + 8, "qr too small: {w}");
    let mut buf = vec![0u8; reader.output_buffer_size()];
    reader.next_frame(&mut buf).unwrap();
    assert!(buf.contains(&255u8) && buf.contains(&0u8));
    let uri = qr::qr_png_data_uri(&payload).unwrap();
    assert!(uri.starts_with("data:image/png;base64,"));
}

#[test]
fn fingerprint_is_sha256_prefix_uppercase() {
    let fp = upix_gui::host_bridge::fingerprint_of(&[0u8; 32]);
    let expected: String = {
        use sha2::{Digest, Sha256};
        let d = Sha256::digest([0u8; 32]);
        d.iter().take(8).map(|b| format!("{b:02X}")).collect()
    };
    assert_eq!(fp, expected);
    assert_eq!(fp.len(), 16);
}

#[test]
fn mock_job_streams_progress_then_done() {
    let dir = std::env::temp_dir().join("upix-gui-test-mockjob");
    std::fs::create_dir_all(&dir).unwrap();
    let input = dir.join("in.png");
    tiny_png(&input, 40, 30);

    let (tx, rx) = mpsc::channel();
    let cancel = Arc::new(AtomicBool::new(false));
    MockBackend.spawn(req(&input, cancel), tx);
    let mut events = Vec::new();
    while let Ok(event) = rx.recv() {
        let done = matches!(event, EngineEvent::BatchDone { .. });
        events.push(event);
        if done {
            break;
        }
    }
    let has_progress = events
        .iter()
        .any(|e| matches!(e, EngineEvent::Progress { item_total, .. } if *item_total > 0));
    assert!(has_progress, "no progress event: {events:?}");
    let has_item_done = events.iter().any(|e| matches!(e, EngineEvent::ItemDone(_)));
    assert!(has_item_done, "no item done event: {events:?}");
    assert!(matches!(
        events.last(),
        Some(EngineEvent::BatchDone { completed: 1, .. })
    ));
    let out = dir.join("in_NomosUni-SPAN-x2.png");
    assert!(out.exists());
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn mock_job_cancel_stops_before_done() {
    let dir = std::env::temp_dir().join("upix-gui-test-cancel");
    std::fs::create_dir_all(&dir).unwrap();
    let input = dir.join("in.png");
    tiny_png(&input, 200, 200);

    let (tx, rx) = mpsc::channel();
    let cancel = Arc::new(AtomicBool::new(false));
    let flag = cancel.clone();
    MockBackend.spawn(req(&input, cancel), tx);
    flag.store(true, std::sync::atomic::Ordering::Relaxed);
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(600);
    let mut batch_done = false;
    while std::time::Instant::now() < deadline {
        match rx.try_recv() {
            Ok(EngineEvent::BatchDone { .. }) => {
                batch_done = true;
                break;
            }
            Ok(EngineEvent::Cancelled) => break,
            Ok(_) => {}
            Err(_) => std::thread::sleep(std::time::Duration::from_millis(20)),
        }
    }
    assert!(!batch_done, "cancel must prevent BatchDone");
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn scene_table_matches_core_order() {
    let (scene, rec) = models_local::classify_scene("Real-ESRGANv3-anime-x2").unwrap();
    assert_eq!(scene, "动漫");
    assert!(rec);
    let (scene, _) = models_local::classify_scene("Real-ESRGAN-x4plus").unwrap();
    assert_eq!(scene, "照片");
    let (scene, _) = models_local::classify_scene("1x_DeJPG sr_official").unwrap();
    assert_eq!(scene, "照片·修复");
    let (scene, rec) = models_local::classify_scene("NomosUni-SPAN-x2").unwrap();
    assert_eq!(scene, "照片");
    assert!(!rec);
    assert_eq!(models_local::classify_scene("totally-unknown"), None);
}

#[test]
fn scale_from_filename_handles_catalog_forms() {
    assert_eq!(models_local::scale_from_filename("Real-ESRGAN-x2plus"), Some(2));
    assert_eq!(models_local::scale_from_filename("4x-UltraSharp"), None);
    assert_eq!(models_local::scale_from_filename("UltraSharp-x4"), Some(4));
    assert_eq!(models_local::scale_from_filename("1x-DeJPG"), None);
    assert_eq!(models_local::scale_from_filename("DeJPG-x1"), Some(1));
    assert_eq!(models_local::scale_from_filename("NomosUni-SPAN-x2"), Some(2));
    assert_eq!(models_local::scale_from_filename("noscale"), None);
    assert_eq!(models_local::scale_from_filename("x2"), Some(2));
    assert_eq!(models_local::scale_from_filename("x99999999999"), None);
    assert_eq!(models_local::scale_from_filename("x2-y4"), Some(2));
    assert_eq!(models_local::scale_from_filename("x2-x4"), Some(4));
}

#[test]
fn scan_lists_param_files_with_metadata() {
    let dir = std::env::temp_dir().join("upix-gui-test-scan");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("NomosUni-SPAN-x2.param"), b"p").unwrap();
    std::fs::write(dir.join("NomosUni-SPAN-x2.bin"), b"b").unwrap();
    std::fs::write(dir.join("orphan.bin"), b"b").unwrap();
    let (groups, installed) = models_local::scan_installed_groups(&dir);
    assert!(installed.contains("NomosUni-SPAN-x2"));
    assert_eq!(groups.len(), 1);
    let item = &groups[0].items[0];
    assert_eq!(item.name, "NomosUni-SPAN-x2");
    assert_eq!(item.scale, 2);
    assert!(item.installed);
    assert_eq!(item.scene, "照片");
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn import_copies_param_and_bin_only() {
    let dir = std::env::temp_dir().join("upix-gui-test-import");
    let models = dir.join("models");
    std::fs::create_dir_all(&dir).unwrap();
    let src = dir.join("m.param");
    let txt = dir.join("m.txt");
    std::fs::write(&src, b"p").unwrap();
    std::fs::write(&txt, b"t").unwrap();
    let lines = models_local::import_files(&[src.clone(), txt], &models);
    assert!(models.join("m.param").exists());
    assert_eq!(lines.len(), 2);
    assert!(lines[1].contains("跳过"));
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn output_path_follows_the_cli_grammar() {
    let p = compute_output_path(
        &PathBuf::from("/tmp/pics/cat.png"),
        "NomosUni-SPAN-x2",
        2,
        OutputFormat::Png,
        None,
    );
    assert_eq!(p, PathBuf::from("/tmp/pics/cat_NomosUni-SPAN-x2.png"));
    let p_fallback = compute_output_path(
        &PathBuf::from("/tmp/pics/cat.png"),
        "",
        2,
        OutputFormat::Png,
        None,
    );
    assert_eq!(p_fallback, PathBuf::from("/tmp/pics/cat_upix_x2.png"));
}
