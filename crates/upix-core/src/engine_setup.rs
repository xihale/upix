//! engine_setup — Rust port of `engine/core/engine_setup.{hpp,cpp}`.
//!
//! Device strategies (GPU / CPU / GPU_THEN_CPU / engine default), lazy
//! `switch_model`, full engine+model setup, and the import sanity probe.
//!
//! Deviation from the C++ (forced by the workspace layout): `upix-core` has no
//! ncnn dependency, so engine construction is injected via a factory closure
//! instead of the C++ `create_ncnn_engine()`. Control flow and error strings
//! are otherwise verbatim.

use crate::engine::{DeviceType, Engine};
use crate::model_graph::MAX_GRAPH_SCALE_FACTOR;
use crate::model_manager::{ModelInfo, ModelManager};
use std::path::{Path, PathBuf};

/// `SetupDevice` from engine_setup.hpp.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EngineStrategy {
    Gpu,
    Cpu,
    GpuThenCpu,
    /// Let the engine pick (NCNNEngine tries GPU→CPU internally).
    EngineDefault,
}

/// `EngineSetupResult`: the initialized engine, the scanned model catalog, and
/// the selected model.
pub struct EngineSetupResult {
    pub engine: Box<dyn Engine>,
    pub models: ModelManager,
    pub selected: Option<ModelInfo>,
}

/// Initialize one engine with the requested strategy. `factory` constructs the
/// backend (the ncnn engine in production; mocks in tests).
pub fn initialize_engine(
    factory: impl Fn() -> Box<dyn Engine>,
    strategy: EngineStrategy,
) -> Result<Box<dyn Engine>, String> {
    let mut engine = factory();

    // ── initialize ───────────────────────────────────────────────────────────
    match strategy {
        EngineStrategy::Gpu => {
            if let Err(r) = engine.initialize(DeviceType::GpuVulkan) {
                return Err(format!("engine initialize (GPU) failed: {}", r.message));
            }
        }
        EngineStrategy::Cpu => {
            if let Err(r) = engine.initialize(DeviceType::Cpu) {
                return Err(format!("engine initialize (CPU) failed: {}", r.message));
            }
        }
        EngineStrategy::GpuThenCpu => {
            if let Err(gpu) = engine.initialize(DeviceType::GpuVulkan) {
                log::warn!(target: "Engine", "GPU not available, falling back to CPU: {}", gpu.message);
                if let Err(cpu) = engine.initialize(DeviceType::Cpu) {
                    return Err(format!(
                        "engine initialize failed on both GPU and CPU: {}",
                        cpu.message
                    ));
                }
            }
        }
        // The frozen Engine trait has no argument-less initialize(); the C++
        // engine-default path (NCNNEngine tries GPU→CPU internally) maps to
        // the same explicit fallback here.
        EngineStrategy::EngineDefault => {
            let err = match engine.initialize(DeviceType::GpuVulkan) {
                Ok(()) => None,
                Err(gpu) => match engine.initialize(DeviceType::Cpu) {
                    Ok(()) => None,
                    Err(cpu) => Some(format!(
                        "engine initialize failed: {} (GPU: {})",
                        cpu.message, gpu.message
                    )),
                },
            };
            if let Some(e) = err {
                return Err(e);
            }
        }
    }

    Ok(engine)
}

/// Lazy model switch: empty name = "use whatever is loaded" (caller didn't
/// specify); same name = no-op; otherwise load and remember. Returns false on
/// unknown model / load failure (logged, caller decides how to fail).
pub fn switch_model(
    engine: &mut dyn Engine,
    models: &ModelManager,
    loaded_model_name: &mut String,
    model_name: &str,
    log_tag: &str,
) -> bool {
    if model_name.is_empty() || model_name == *loaded_model_name {
        return true;
    }
    let Some(info) = models.get_model(model_name) else {
        log::error!(target: log_tag, "unknown model requested: {}", model_name);
        return false;
    };
    if let Err(e) = engine.load_model(&info.param_path, &info.bin_path) {
        log::error!(target: log_tag, "model load failed: {}", e.message);
        return false;
    }
    *loaded_model_name = model_name.to_string();
    log::info!(target: log_tag, "switched model to {}", model_name);
    true
}

pub fn setup_engine(
    factory: impl Fn() -> Box<dyn Engine>,
    strategy: EngineStrategy,
    models_dir: &str,
    model_name: &str,
) -> Result<EngineSetupResult, String> {
    let engine = initialize_engine(factory, strategy)?;

    // ── scan_models ──────────────────────────────────────────────────────────
    let resolved_models_dir =
        if models_dir.is_empty() { "./models".to_string() } else { models_dir.to_string() };
    let mut models = ModelManager::new(PathBuf::from(&resolved_models_dir));
    let scan = models.scan_models();
    let scanned = match scan {
        Ok(v) => v,
        Err(e) => return Err(format!("model scan failed: {}", e.message)),
    };
    if scanned.is_empty() {
        return Err(format!("no models found in: {}", resolved_models_dir));
    }

    // ── select ───────────────────────────────────────────────────────────────
    let selected = if model_name.is_empty() {
        scanned.first().cloned()
    } else {
        ModelManager::find_model(&scanned, model_name).cloned()
    };
    let Some(selected) = selected else {
        return Err(format!("model not found: {}", model_name));
    };

    // ── load_model ───────────────────────────────────────────────────────────
    let mut engine = engine;
    engine
        .load_model(&selected.param_path, &selected.bin_path)
        .map_err(|e| format!("model load failed: {}", e.message))?;

    Ok(EngineSetupResult { engine, models, selected: Some(selected) })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockEngine;
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    fn factory_with(counter: &'static AtomicUsize, gpu_ok: bool) -> impl Fn() -> Box<dyn Engine> {
        move || {
            counter.fetch_add(1, Ordering::Relaxed);
            let mut e = MockEngine::new();
            if gpu_ok {
                e.device = DeviceType::GpuVulkan;
                e.gpu_tiles = true;
                e.gpu_tiles_u8 = true;
            }
            Box::new(e)
        }
    }

    #[test]
    fn cpu_strategy_initializes_on_cpu() {
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        let engine = initialize_engine(factory_with(&CALLS, true), EngineStrategy::Cpu).unwrap();
        assert_eq!(engine.current_device(), DeviceType::Cpu);
        assert!(engine.is_initialized());
    }

    #[test]
    fn gpu_then_cpu_falls_back_when_gpu_init_fails() {
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        // factory builds CPU-only mocks → GPU initialize fails, CPU succeeds.
        let engine =
            initialize_engine(factory_with(&CALLS, false), EngineStrategy::GpuThenCpu).unwrap();
        assert_eq!(engine.current_device(), DeviceType::Cpu);
        assert_eq!(CALLS.load(Ordering::Relaxed), 1, "one engine object, two initialize attempts");
    }

    #[test]
    fn gpu_strategy_fails_hard_without_gpu() {
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        let err = match initialize_engine(factory_with(&CALLS, false), EngineStrategy::Gpu) {
            Err(e) => e,
            Ok(_) => panic!("GPU strategy without a GPU must fail"),
        };
        assert!(err.contains("engine initialize (GPU) failed:"));
    }

    #[test]
    fn engine_default_falls_through_both_devices() {
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        let engine =
            initialize_engine(factory_with(&CALLS, false), EngineStrategy::EngineDefault).unwrap();
        assert_eq!(engine.current_device(), DeviceType::Cpu);
    }

    #[test]
    fn switch_model_is_lazy_and_validated() {
        let dir = std::env::temp_dir().join(format!("upix-core-setup-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let param = dir.join("mock-x2.param");
        let bin = dir.join("mock-x2.bin");
        std::fs::write(&param, b"777 1\n").unwrap();
        std::fs::write(&bin, b"\0\0").unwrap();

        let mut models = ModelManager::new(dir.clone());
        models.scan_models().unwrap();

        let mut engine = MockEngine::new();
        let mut loaded = String::new();

        // Empty name = keep whatever is loaded.
        assert!(switch_model(&mut engine, &models, &mut loaded, "", "test"));
        assert_eq!(engine.load_calls.load(Ordering::Relaxed), 0);

        // Same name = no-op.
        loaded = "mock-x2".into();
        assert!(switch_model(&mut engine, &models, &mut loaded, "mock-x2", "test"));
        assert_eq!(engine.load_calls.load(Ordering::Relaxed), 0);

        // Different known name = load once, remember.
        assert!(switch_model(&mut engine, &models, &mut loaded, "Mock_X2", "test"));
        assert_eq!(engine.load_calls.load(Ordering::Relaxed), 1);
        assert_eq!(loaded, "Mock_X2");

        // Unknown name = logged failure.
        assert!(!switch_model(&mut engine, &models, &mut loaded, "nope", "test"));
        assert_eq!(engine.load_calls.load(Ordering::Relaxed), 1);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn setup_engine_selects_first_model_by_default() {
        let dir = std::env::temp_dir().join(format!("upix-core-setup2-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let param = dir.join("mock-x2.param");
        let bin = dir.join("mock-x2.bin");
        std::fs::write(&param, b"777 1\n").unwrap();
        std::fs::write(&bin, b"\0\0").unwrap();

        static CALLS: AtomicUsize = AtomicUsize::new(0);
        let result = setup_engine(factory_with(&CALLS, false), EngineStrategy::Cpu,
            dir.to_str().unwrap(), "")
            .expect("setup must succeed");
        assert_eq!(result.selected.as_ref().unwrap().name, "mock-x2");
        assert_eq!(result.engine.model_name(), "mock");

        let err = match setup_engine(factory_with(&CALLS, false), EngineStrategy::Cpu,
            dir.to_str().unwrap(), "does-not-exist") {
            Err(e) => e,
            Ok(_) => panic!("unknown model must fail"),
        };
        assert_eq!(err, "model not found: does-not-exist");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn probe_sanity_flags_unloadable_model_but_never_blocks() {
        let dir = std::env::temp_dir().join(format!("upix-core-setup3-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let param = dir.join("m.param");
        std::fs::write(&param, b"x").unwrap();
        // MockEngine::new() load_model always succeeds, so the probe returns
        // None (deterministic finite output).
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        assert!(probe_model_sanity(factory_with(&CALLS, false), &param, &dir.join("m.bin")).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }
}


/// Probe an imported model with a fresh CPU engine: load + one 64x64 forward
/// pass + non-finite scan. `Some(msg)` = the model looks broken (message is
/// shown to the user); `None` = fine (or probe unavailable, which must never
/// block an import).
pub fn probe_model_sanity(
    factory: impl Fn() -> Box<dyn Engine>,
    param_path: &Path,
    bin_path: &Path,
) -> Option<String> {
    let Ok(init) = initialize_engine(factory, EngineStrategy::Cpu) else {
        return None; // probe unavailable — never block the import
    };
    let mut engine = init;
    if let Err(e) = engine.load_model(param_path, bin_path) {
        return Some(format!(
            "sanity probe could not load the model ({}) — processing it will likely fail",
            e.message
        ));
    }

    const PROBE_SIZE: i32 = 64;
    let mut input = vec![0.0f32; (PROBE_SIZE * PROBE_SIZE * 3) as usize];
    for (i, v) in input.iter_mut().enumerate() {
        *v = (i % 251) as f32 / 250.0; // deterministic gradient
    }
    // Size the output exactly as the engine's shape check will: both bounds
    // are clamped and the multiply is done in usize, so a bogus graph scale
    // cannot overflow (or under-allocate) here.
    let scale = engine.get_scale_factor().clamp(1, MAX_GRAPH_SCALE_FACTOR);
    let probe_out_w = (PROBE_SIZE as usize) * (scale as usize);
    let mut output = vec![0.0f32; probe_out_w * probe_out_w * 3];
    let cancel = std::sync::atomic::AtomicBool::new(false);
    let run = engine.process(&input, &mut output, PROBE_SIZE, PROBE_SIZE, 3, &cancel);
    if let Err(e) = run {
        return Some(format!(
            "sanity probe forward pass failed ({}) — the model will likely fail on real inputs too",
            e.message
        ));
    }

    let nonfinite = output.iter().filter(|v| !v.is_finite()).count();
    if nonfinite > 0 {
        return Some(format!(
            "sanity probe produced {} non-finite output value(s) — this conversion likely mis-executes on the pinned ncnn build; upscales may come out as garbage",
            nonfinite
        ));
    }
    None
}

