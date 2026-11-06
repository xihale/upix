//! Real-engine acceptance tests (CONTRACT §7):
//!   - CPU initialize + NomosUni-SPAN-x2 load + small random RGB process,
//!     run twice → byte-identical output (determinism).
//!   - Real-ESRGANv3-anime-x2: scale_factor_detected()/get_scale_factor()
//!     reasonable (param-graph detection, not filename).
//!   - get_recommended_tile_size() > 0.
//!   - GPU paths behind #[ignore]; set UPIX_GPU_TESTS=1 to run.
//! Models are skipped gracefully when missing.

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;

use upix_core::engine::{DeviceType, Engine};
use upix_engine_ffi::NcnnEngine;

fn models_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("UPIX_MODELS_DIR") {
        return PathBuf::from(dir);
    }
    let home = std::env::var("HOME").expect("HOME unset; set UPIX_MODELS_DIR");
    PathBuf::from(home).join(".local/share/upix/models")
}

fn model_pair(name: &str) -> Option<(PathBuf, PathBuf)> {
    let dir = models_dir();
    let param = dir.join(format!("{name}.param"));
    let bin = dir.join(format!("{name}.bin"));
    (param.is_file() && bin.is_file()).then_some((param, bin))
}

fn cpu_engine_with_model(name: &str) -> (NcnnEngine, i32, i32) {
    let Some((param, bin)) = model_pair(name) else {
        eprintln!("skipping: model {name} not found in {}", models_dir().display());
        return (NcnnEngine::new().expect("engine create"), 0, 0);
    };
    let mut engine = NcnnEngine::new().expect("engine create");
    engine
        .initialize(DeviceType::Cpu)
        .expect("CPU initialize failed");
    engine.load_model(&param, &bin).expect("load_model failed");
    let scale = engine.get_scale_factor();
    (engine, scale, 1)
}

/// 12x8 RGB random image → input float buffer (HWC).
fn random_rgb_f32(width: i32, height: i32) -> Vec<f32> {
    // Deterministic xorshift; no rand dependency needed.
    let mut state: u64 = 0x9E3779B97F4A7C15;
    let count = (width * height * 3) as usize;
    (0..count)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            (state % 256) as f32 / 255.0
        })
        .collect()
}

#[test]
fn cpu_process_deterministic_nomosuni() {
    let Some((param, bin)) = model_pair("NomosUni-SPAN-x2") else {
        eprintln!("skipping: NomosUni-SPAN-x2 missing");
        return;
    };
    let mut engine = NcnnEngine::new().expect("engine create");
    engine
        .initialize(DeviceType::Cpu)
        .expect("CPU initialize failed");
    engine.load_model(&param, &bin).expect("load_model failed");
    assert_eq!(engine.current_device(), DeviceType::Cpu);
    assert!(engine.is_initialized());
    assert_eq!(engine.model_name(), "NomosUni-SPAN-x2");

    // Small image keeps runtime short; 2x model → 24x16 output.
    let (w, h, c) = (12, 8, 3);
    let input = random_rgb_f32(w, h);
    let out_w = w * 2;
    let out_h = h * 2;
    let out_len = (out_w * out_h * c) as usize;

    let cancel = AtomicBool::new(false);
    let mut out1 = vec![0f32; out_len];
    let mut out2 = vec![0f32; out_len];
    engine
        .process(&input, &mut out1, w, h, c, &cancel)
        .expect("process #1 failed");
    engine
        .process(&input, &mut out2, w, h, c, &cancel)
        .expect("process #2 failed");
    // Byte-identical across two identical runs (CONTRACT acceptance).
    assert_eq!(out1.len(), out2.len());
    assert!(
        out1.iter().zip(out2.iter()).all(|(a, b)| a.to_bits() == b.to_bits()),
        "CPU inference is not deterministic across runs"
    );
    // Sanity: output is not all zeros.
    assert!(out1.iter().any(|v| *v != 0.0), "output is all zeros");
}

#[test]
fn realesrgan_scale_detection() {
    let (engine, _scale, loaded) = cpu_engine_with_model("Real-ESRGANv3-anime-x2");
    if loaded == 0 {
        return; // skipped gracefully above
    }
    // Real-ESRGANv3 x2: param-graph detection must fire and agree with the
    // model's actual 2x upsample trunk.
    assert!(
        engine.scale_factor_detected(),
        "expected param-graph scale detection for Real-ESRGANv3-anime-x2"
    );
    assert_eq!(engine.get_scale_factor(), 2);
    assert_eq!(engine.model_input_channels(), 3);
}

#[test]
fn recommended_tile_size_positive() {
    let (engine, _scale, loaded) = cpu_engine_with_model("NomosUni-SPAN-x2");
    if loaded == 0 {
        return;
    }
    assert!(
        engine.get_recommended_tile_size() > 0,
        "recommended tile size must be > 0"
    );
    assert!(!engine.hardware_id().is_empty());
    let (total, _avail) = engine.get_memory_info();
    assert!(total > 0, "host memory info should report total");
}

#[test]
fn load_missing_model_reports_error() {
    let mut engine = NcnnEngine::new().expect("engine create");
    engine
        .initialize(DeviceType::Cpu)
        .expect("CPU initialize failed");
    let result = engine.load_model(
        &models_dir().join("definitely-not-a-model.param"),
        &models_dir().join("definitely-not-a-model.bin"),
    );
    assert!(result.is_err(), "loading a missing model must fail cleanly");
}

#[test]
fn list_devices_includes_cpu() {
    let engine = NcnnEngine::new().expect("engine create");
    let devices = engine.list_devices();
    assert!(
        devices.iter().any(|d| d.dtype == DeviceType::Cpu),
        "device list must contain the CPU"
    );
}

#[test]
fn unload_then_reload_model() {
    let Some((param, bin)) = model_pair("NomosUni-SPAN-x2") else {
        eprintln!("skipping: NomosUni-SPAN-x2 missing");
        return;
    };
    let mut engine = NcnnEngine::new().expect("engine create");
    engine
        .initialize(DeviceType::Cpu)
        .expect("CPU initialize failed");
    engine.load_model(&param, &bin).expect("load failed");
    engine.unload_model();
    assert!(!engine.is_initialized() || engine.model_name().is_empty());
    engine.load_model(&param, &bin).expect("reload failed");
    assert_eq!(engine.model_name(), "NomosUni-SPAN-x2");
}

#[test]
fn gpu_tile_pipeline_capabilities() {
    let engine = NcnnEngine::new().expect("engine create");
    // Before initialize, GPU support flags reflect ncnn build + device probe.
    let _gpu = engine.supports_gpu_tile_pipeline();
    let _u8 = engine.supports_gpu_tile_pipeline_u8();
    let _queue = engine.supports_gpu_tile_pipeline_u8_queue();
}

// --- GPU paths: opt-in via UPIX_GPU_TESTS=1 (need a Vulkan device) ----------

fn gpu_tests_enabled() -> bool {
    matches!(std::env::var("UPIX_GPU_TESTS").as_deref(), Ok("1"))
}

fn gpu_engine_with_model(name: &str) -> Option<(NcnnEngine, i32)> {
    if !gpu_tests_enabled() {
        return None;
    }
    let Some((param, bin)) = model_pair(name) else {
        eprintln!("skipping: model {name} not found");
        return None;
    };
    let mut engine = NcnnEngine::new().expect("engine create");
    if engine.initialize(DeviceType::GpuVulkan).is_err() {
        eprintln!("skipping: Vulkan initialize failed");
        return None;
    }
    engine.load_model(&param, &bin).expect("load_model failed");
    let scale = engine.get_scale_factor();
    Some((engine, scale))
}

#[test]
#[ignore] // run with UPIX_GPU_TESTS=1
fn gpu_process_whole_image() {
    let Some((engine, scale)) = gpu_engine_with_model("NomosUni-SPAN-x2") else {
        eprintln!("skipping: GPU tests disabled (UPIX_GPU_TESTS=1 to enable)");
        return;
    };
    assert_eq!(engine.current_device(), DeviceType::GpuVulkan);
    let (w, h, c) = (16, 16, 3);
    let input = random_rgb_f32(w, h);
    let mut output = vec![0f32; ((w * scale) * (h * scale) * c) as usize];
    let cancel = AtomicBool::new(false);
    engine
        .process(&input, &mut output, w, h, c, &cancel)
        .expect("GPU process failed");
    assert!(output.iter().any(|v| *v != 0.0), "GPU output all zeros");
}

#[test]
#[ignore] // run with UPIX_GPU_TESTS=1
fn gpu_tile_pipeline_u8() {
    let Some((engine, scale)) = gpu_engine_with_model("Real-ESRGANv3-anime-x2") else {
        eprintln!("skipping: GPU tests disabled (UPIX_GPU_TESTS=1 to enable)");
        return;
    };
    if !engine.supports_gpu_tile_pipeline_u8() {
        eprintln!("skipping: engine reports no u8 GPU tile pipeline");
        return;
    }
    let (w, h, c) = (64, 64, 3);
    let input: Vec<u8> = (0..(w * h * c) as usize).map(|i| (i % 251) as u8).collect();
    let mut output = vec![0u8; ((w * scale) * (h * scale) * c) as usize];
    // 64x64 image, tile 32 / overlap 8 → step 24; mirrors the C++
    // compute_tile_layout output (region covers the tile incl. overlap).
    let mk = |x: u32, y: u32| upix_core::engine::TileRegion {
        x,
        y,
        width: 32,
        height: 32,
        overlap: 8,
    };
    let regions = vec![mk(0, 0), mk(24, 0), mk(0, 24), mk(24, 24)];
    let cancel = AtomicBool::new(false);
    let progress = |_p: &upix_core::engine::ProcessingProgress| {};
    engine
        .process_tiles_gpu_u8(
            &input,
            &mut output,
            w,
            h,
            c,
            &regions,
            upix_core::engine::TileConfig { tile_size: 32, overlap: 8, batch_size: 1 },
            Some(&progress),
            &cancel,
        )
        .expect("GPU u8 tile pipeline failed");
    assert!(output.iter().any(|v| *v != 0));
}
