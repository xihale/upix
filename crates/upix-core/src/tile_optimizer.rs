//! TileSizeOptimizer — Rust port of `legacy/share/core/tile_optimizer.hpp`
//! (header-only in C++): candidate {64..512} benchmarking + memory-budget
//! elimination, the ×1.05 tolerance band, and the persistent per-hardware
//! probe cache at `$XDG_CACHE_HOME|$HOME/.cache/upix/auto_tile_cache.json`.

use crate::config::{ProcessingConfig, DEFAULT_TILE_SIZE, TILE_SIZE_MIN};
use crate::engine::{DeviceType, Engine};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::{LazyLock, Mutex};
use std::time::Instant;

// Compile-time pin: the optimizer's floor is the config module's bound.
const _: () = assert!(TILE_SIZE_MIN == 16);

/// 设备性能档案（auto-tile 探测用，仅保留实际消费的字段）
#[derive(Debug, Clone, Default)]
pub struct DevicePerformanceProfile {
    pub available_memory_mb: u64,
}

#[derive(Debug, Clone, Default)]
pub struct TileBenchmark {
    pub tile_size: i32,
    pub tiles_per_second: f32,
    pub memory_usage_mb: u64,
    pub success: bool,
    /// Unix seconds of the measurement; 0 = unknown, treated as stale so the
    /// size gets revalidated on the next probe instead of a legacy entry
    /// living forever.
    pub measured_at_sec: i64,
}

/// 模型约束
#[derive(Debug, Clone)]
pub struct ModelConstraints {
    pub model_name: String,
    pub model_size_mb: u64,
    pub memory_multiplier: f32,
    pub min_tile_size: i32,
    pub max_tile_size: i32,
}

impl Default for ModelConstraints {
    fn default() -> Self {
        Self {
            model_name: String::new(),
            model_size_mb: 0,
            memory_multiplier: 10.0,
            min_tile_size: 0,
            max_tile_size: 1024,
        }
    }
}

pub fn calculate_tile_memory(
    tile_size: i32,
    channels: i32,
    scale_factor: i32,
    model: &ModelConstraints,
) -> u64 {
    // 输入 tile 内存
    let input_mem = tile_size as u64
        * tile_size as u64
        * channels as u64
        * std::mem::size_of::<f32>() as u64;

    // 输出 tile 内存
    let out_size = (tile_size * scale_factor) as u64;
    let output_mem = out_size * out_size * channels as u64 * std::mem::size_of::<f32>() as u64;

    // 中间激活内存（模型相关）
    let activation_mem = (input_mem + output_mem) * model.memory_multiplier as u64;

    // 返回 MB
    (input_mem + output_mem + activation_mem + model.model_size_mb * 1024 * 1024)
        / (1024 * 1024)
}

// ---------------------------------------------------------------------------
// Auto-tile probe cache (in-memory + disk)
//
// The probe benchmarks up to 8 tile sizes with (2 warmup + 10 timed) full
// forward passes each — a large fixed cost (~28s on CPU). What it measures is
// a property of (hardware, model, tile size) alone: the synthetic benchmark
// input never varies with the image being processed. The only image-dependent
// term in the size choice is the tile count, which is O(1) to recompute per
// image. So the per-size benchmark results are cached under
// (hardware_id, model, device, tile_size) and reused across every image —
// portrait vs landscape, or any resolution change, never re-measures hardware
// performance a sibling image just measured.
//
// The cache persists to $XDG_CACHE_HOME (or ~/.cache)/upix/auto_tile_cache.json
// so a warm entry survives process restarts. hardware_id() (GPU name + driver,
// CPU cores + SIMD flags) is part of the key: swapping the card or the driver
// invalidates the affected rows automatically instead of feeding timings from
// different silicon.
//
// Load policy: external contention can only depress a measurement, never
// inflate it, so re-measures keep the faster value (max-merge); a re-measure
// landing far below the cached rate is treated as a busy/throttled device —
// warn and keep the cached timing rather than persisting the polluted one.
// Entries carry a timestamp and revalidate after PROBE_TTL_SEC so even a
// first-ever probe measured under load self-heals within the TTL window.
// Failed benchmarks stay process-local (a failure can be a transient OOM
// under external memory pressure, which must not freeze the size out across
// runs). A global mutex also serializes benchmarking, because concurrent
// probes would contend for the device and pollute each other.
// ---------------------------------------------------------------------------

// Probe results older than this are re-measured (and max-merged) once, so a
// measurement polluted by load, or a machine that genuinely changed under the
// same fingerprint, cannot persist forever.
const PROBE_TTL_SEC: i64 = 14 * 24 * 3600;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct TileBenchKey {
    hw: String, // Engine::hardware_id(); empty = unknown device
    model_name: String,
    device: i32, // DeviceType as int
    tile_size: i32,
}

// On-disk row (glaze layout: hw, model, device, tile, tps, mem_mb, t).
#[derive(Debug, serde::Serialize, serde::Deserialize, Default)]
struct TileCacheEntry {
    hw: String,
    model: String,
    device: i32,
    tile: i32,
    tps: f32,
    mem_mb: u64,
    t: i64,
}

#[derive(Debug, serde::Serialize, serde::Deserialize, Default)]
struct TileCacheFile {
    version: i32,
    entries: Vec<TileCacheEntry>,
}

pub(crate) struct TileBenchCache {
    map: HashMap<TileBenchKey, TileBenchmark>,
    loaded: bool,
}

static CACHE: LazyLock<Mutex<TileBenchCache>> = LazyLock::new(|| {
    Mutex::new(TileBenchCache { map: HashMap::new(), loaded: false })
});
// Serializes benchmarking process-wide (concurrent probes would contend for
// the device and pollute each other's timings).
static PROBE_LOCK: Mutex<()> = Mutex::new(());

fn unix_now_sec() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

fn user_home() -> Option<String> {
    if let Some(home) = std::env::var_os("HOME") {
        if !home.is_empty() {
            return Some(home.to_string_lossy().into_owned());
        }
    }
    std::env::var_os("USERPROFILE")
        .filter(|h| !h.is_empty())
        .map(|h| h.to_string_lossy().into_owned())
}

/// $UPIX_TILE_CACHE overrides (tests), else $XDG_CACHE_HOME/upix/, else
/// $HOME/.cache/upix/. None disables persistence silently.
fn tile_cache_path() -> Option<PathBuf> {
    if let Ok(env) = std::env::var("UPIX_TILE_CACHE") {
        if !env.is_empty() {
            return Some(PathBuf::from(env));
        }
    }
    let xdg = std::env::var("XDG_CACHE_HOME").ok().filter(|s| !s.is_empty());
    let base = match xdg {
        Some(x) => PathBuf::from(x),
        None => PathBuf::from(user_home()?).join(".cache"),
    };
    Some(base.join("upix").join("auto_tile_cache.json"))
}

// Merge disk rows into the cache. Once per process, under the cache mutex.
// Unparsable or foreign-version files are ignored and rewritten on save.
fn tile_bench_load_once(state: &mut TileBenchCache, path: Option<&Path>) {
    if state.loaded {
        return;
    }
    state.loaded = true;

    let Some(path) = path else { return };
    let Ok(json) = std::fs::read_to_string(path) else {
        return;
    };

    let Ok(parsed) = serde_json::from_str::<TileCacheFile>(&json) else {
        log::debug!(target: "TileOptimizer", "Auto-tile cache unreadable, starting fresh");
        return;
    };
    if parsed.version != 1 {
        return;
    }

    let mut merged = 0;
    for e in parsed.entries {
        let key =
            TileBenchKey { hw: e.hw, model_name: e.model, device: e.device, tile_size: e.tile };
        if state.map.contains_key(&key) {
            continue; // a measurement taken this process is fresher
        }
        state.map.insert(
            key,
            TileBenchmark {
                tile_size: e.tile,
                tiles_per_second: e.tps,
                memory_usage_mb: e.mem_mb,
                success: true,
                measured_at_sec: e.t,
            },
        );
        merged += 1;
    }
    if merged > 0 {
        log::debug!(target: "TileOptimizer",
            "Auto-tile cache: {} entries loaded from {}", merged, path.display());
    }
}

// Rewrite the whole file from the in-memory map (atomic tmp+rename). Any
// failure is debug-logged only — the cache is a pure optimization and must
// never fail a job.
fn tile_bench_save(cache: &HashMap<TileBenchKey, TileBenchmark>, path: Option<&Path>) {
    let Some(path) = path else { return };

    let mut out = TileCacheFile { version: 1, entries: Vec::with_capacity(cache.len()) };
    for (key, bench) in cache {
        if !bench.success || key.hw.is_empty() {
            continue; // failures / fingerprint-less devices stay process-local
        }
        out.entries.push(TileCacheEntry {
            hw: key.hw.clone(),
            model: key.model_name.clone(),
            device: key.device,
            tile: key.tile_size,
            tps: bench.tiles_per_second,
            mem_mb: bench.memory_usage_mb,
            t: bench.measured_at_sec,
        });
    }
    let Ok(json) = serde_json::to_string(&out) else {
        return;
    };

    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let tmp = PathBuf::from(format!("{}.tmp", path.display()));
    if let Err(e) = std::fs::write(&tmp, json.as_bytes()) {
        log::debug!(target: "TileOptimizer", "Auto-tile cache: cannot open {} for writing: {e}", tmp.display());
        return;
    }
    if let Err(e) = std::fs::rename(&tmp, path) {
        // rename-over-existing is not portable everywhere; fall back once.
        let _ = std::fs::remove_file(path);
        if let Err(e2) = std::fs::rename(&tmp, path) {
            log::debug!(target: "TileOptimizer",
                "Auto-tile cache: failed to store {}: {e} / {e2}", path.display());
        }
    }
}

pub struct TileSizeOptimizer;

impl TileSizeOptimizer {
    pub fn profile_device(engine: &dyn Engine) -> DevicePerformanceProfile {
        DevicePerformanceProfile { available_memory_mb: engine.get_memory_info().1 }
    }

    fn benchmark_tile_size(
        engine: &dyn Engine,
        tile_size: i32,
        iterations: i32,
        warmup: i32,
    ) -> TileBenchmark {
        let mut result = TileBenchmark { tile_size, success: false, ..Default::default() };

        let scale = engine.get_scale_factor();
        let input = vec![0.5f32; tile_size as usize * tile_size as usize * 3];
        // Note the C++ output sizing: tile² * 3 * scale² (no +1 rounding).
        let mut output =
            vec![0.0f32; tile_size as usize * tile_size as usize * 3 * scale as usize * scale as usize];
        let cancel = AtomicBool::new(false);

        // fire-and-forget: the benchmark measures timing only; the engine
        // call's own failure is ignored and success is set unconditionally
        // after the run (in C++ only an exception could flip it).
        for _ in 0..warmup.max(0) {
            let _ = engine.process(&input, &mut output, tile_size, tile_size, 3, &cancel);
        }
        let mut total_ms = 0.0f64;
        for _ in 0..iterations.max(1) {
            let start = Instant::now();
            let _ = engine.process(&input, &mut output, tile_size, tile_size, 3, &cancel);
            total_ms += start.elapsed().as_secs_f64() * 1000.0;
        }
        let avg_ms = total_ms / iterations.max(1) as f64;
        // An instant engine measures 0ms; a zero (or sub-µs) divisor would
        // produce an infinite rate, which JSON cannot carry. Clamp to 1ns.
        result.tiles_per_second = (1000.0 / avg_ms.max(1e-6)) as f32;
        result.success = true;

        // 估算内存使用
        let dummy_model = ModelConstraints { memory_multiplier: 10.0, ..Default::default() };
        result.memory_usage_mb = calculate_tile_memory(tile_size, 3, scale, &dummy_model);

        result
    }

    /// Full auto-tile probe: benchmarks the surviving candidate sizes (cache
    /// aware), then picks the largest size within +5% of the best estimated
    /// time. Falls back inside the caller's window when nothing survives.
    pub fn find_optimal_tile_size(
        engine: &dyn Engine,
        model: &ModelConstraints,
        config: &ProcessingConfig,
        image_width: i32,
        image_height: i32,
    ) -> i32 {
        // Probe + selection run under one lock: benchmarking must be
        // serialized process-wide (concurrent probes would contend for the
        // device and pollute each other's timings), and a worker that loses
        // that race re-finds the cache filled here and measures nothing.
        let _probe_guard = PROBE_LOCK.lock().unwrap();
        let mut state = CACHE.lock().unwrap();
        let path = tile_cache_path();
        Self::find_optimal_tile_size_inner(
            engine, model, config, image_width, image_height, &mut state, path.as_deref(),
        )
    }

    /// Cache-store-injected variant (tests / caller-owned persistence).
    pub(crate) fn find_optimal_tile_size_inner(
        engine: &dyn Engine,
        model: &ModelConstraints,
        config: &ProcessingConfig,
        image_width: i32,
        image_height: i32,
        state: &mut TileBenchCache,
        cache_path: Option<&Path>,
    ) -> i32 {
        // 测试的 tile 大小列表
        let mut test_sizes: Vec<i32> = vec![64, 128, 192, 256, 320, 384, 448, 512];

        // Fallback used whenever no candidate survives: stay inside the window
        // the caller gave, because a size outside [min_tile_size,
        // max_tile_size] would silently violate a config the caller already
        // validated (the CLI rejects such configs, and the probe is the only
        // place that can honor it here).
        let fallback_tile_size = || -> i32 {
            let lo = config.min_tile_size.max(model.min_tile_size).max(TILE_SIZE_MIN);
            let hi = config.max_tile_size.min(model.max_tile_size);
            if hi < lo {
                return DEFAULT_TILE_SIZE; // contradictory window: validate() owns it
            }
            DEFAULT_TILE_SIZE.clamp(lo, hi)
        };

        // 根据配置过滤
        test_sizes.retain(|&s| s >= config.min_tile_size && s <= config.max_tile_size);

        // 根据模型约束过滤
        if model.min_tile_size > 0 {
            test_sizes.retain(|&s| s >= model.min_tile_size);
        }
        test_sizes.retain(|&s| s <= model.max_tile_size);

        if test_sizes.is_empty() {
            let fallback = fallback_tile_size();
            log::warn!(target: "TileOptimizer",
                "No valid tile sizes after filtering, using {} (clamped to the configured window)", fallback);
            return fallback;
        }

        let device = match engine.current_device() {
            DeviceType::Cpu => 0,
            DeviceType::GpuVulkan => 1,
        };
        let hw = engine.hardware_id();

        // 获取设备信息
        let profile = Self::profile_device(engine);

        // 计算可用内存上限 (MB). An unknown total (0 on platforms without a
        // memory probe, see Engine::get_memory_info) must not reject every
        // candidate: the memory filter is skipped, exactly like the
        // host-memory preflight does for an unknown amount.
        let memory_known = profile.available_memory_mb > 0;
        let usable_memory_mb = (profile.available_memory_mb as f64 * 0.8) as u64;

        // 第一阶段：基准测试，收集所有通过内存约束的候选尺寸及其估算时间
        struct Candidate {
            size: i32,
            est_time: f32,
        }
        let mut candidates: Vec<Candidate> = Vec::new();
        let mut probed = false;
        let mut dirty = false;

        {
            tile_bench_load_once(state, cache_path);
            let now_sec = unix_now_sec();

            for size in test_sizes {
                let key = TileBenchKey {
                    hw: hw.clone(),
                    model_name: model.model_name.clone(),
                    device,
                    tile_size: size,
                };
                let revalidate = state
                    .map
                    .get(&key)
                    .is_some_and(|b| b.success && now_sec - b.measured_at_sec > PROBE_TTL_SEC);

                if !state.map.contains_key(&key) || revalidate {
                    if !probed {
                        log::info!(target: "TileOptimizer",
                            "Finding optimal tile size (model={})...", model.model_name);
                        probed = true;
                    }
                    log::info!(target: "TileOptimizer", "Benchmarking tile_size={}{}",
                        size, if revalidate { " (cached timing stale, revalidating)" } else { "" });

                    let mut bench = Self::benchmark_tile_size(engine, size, 10, 2);
                    bench.measured_at_sec = now_sec;

                    if !bench.success {
                        if revalidate {
                            // Keep the previously measured rate (still the
                            // best data we have); just re-arm the TTL.
                            if let Some(prev) = state.map.get_mut(&key) {
                                prev.measured_at_sec = now_sec;
                            }
                            dirty = true;
                        } else {
                            log::warn!(target: "TileOptimizer", "Tile size {} failed", size);
                            // Failures are process-local: never written to
                            // disk (a benchmark OOM under external memory
                            // pressure must not freeze the size out across
                            // runs).
                            state.map.insert(key.clone(), bench);
                        }
                    } else if revalidate {
                        let Some(entry) = state.map.get_mut(&key) else {
                            continue;
                        };
                        let cached_tps = entry.tiles_per_second;
                        if bench.tiles_per_second < cached_tps * 0.7 {
                            log::warn!(target: "TileOptimizer",
                                "Tile size {} re-measured at {:.0}% of the cached rate — device busy or throttled, keeping the cached timing",
                                size, 100.0 * bench.tiles_per_second / cached_tps);
                            entry.measured_at_sec = now_sec;
                        } else {
                            // Contention can only depress a measurement, never
                            // inflate it, so keep the faster of the two.
                            bench.tiles_per_second = bench.tiles_per_second.max(cached_tps);
                            *entry = bench;
                        }
                        dirty = true;
                    } else {
                        state.map.insert(key.clone(), bench);
                        dirty = true;
                    }
                }

                let Some(bench) = state.map.get(&key) else {
                    continue;
                };
                if !bench.success {
                    // Measured-failed earlier in this process; retried never.
                    continue;
                }

                // 内存约束检查：必须不超过可用内存的 80%
                if memory_known && bench.memory_usage_mb > usable_memory_mb {
                    log::warn!(target: "TileOptimizer",
                        "  Skipping tile_size={}: memory usage {}MB exceeds usable {}MB",
                        size, bench.memory_usage_mb, usable_memory_mb);
                    continue;
                }

                // 计算该尺寸下的 tile 数量（考虑重叠）
                let step = size - config.tile_overlap;
                if step <= 0 {
                    log::warn!(target: "TileOptimizer",
                        "  Skipping tile_size={}: invalid step (tile_size <= overlap)", size);
                    continue;
                }

                let tiles_x = (image_width + step - 1) / step;
                let tiles_y = (image_height + step - 1) / step;
                let total_tiles = tiles_x * tiles_y;

                // 估算总处理时间（秒）
                let estimated_time_sec = if bench.tiles_per_second > 0.0 {
                    total_tiles as f32 / bench.tiles_per_second
                } else {
                    f32::MAX
                };

                log::debug!(target: "TileOptimizer",
                    "  tile_size={} tiles/sec={} memory={}MB total_tiles={} est_time={}s",
                    size, bench.tiles_per_second, bench.memory_usage_mb, total_tiles,
                    estimated_time_sec);

                candidates.push(Candidate { size, est_time: estimated_time_sec });
            }

            if dirty {
                tile_bench_save(&state.map, cache_path);
            }
        }

        if candidates.is_empty() {
            let fallback = fallback_tile_size();
            log::warn!(target: "TileOptimizer",
                "No valid tile size candidates, using {} (clamped to the configured window)", fallback);
            return fallback;
        }

        // 找出最小估算时间
        let min_time = candidates.iter().map(|c| c.est_time).fold(f32::MAX, f32::min);

        // 容忍阈值：在最小时间的 +5% 范围内，选择最大的 tile size（以提高 GPU 利用率）
        const TOLERANCE: f32 = 1.05;
        let mut best_tile = candidates[0].size;
        for c in &candidates {
            if c.est_time <= min_time * TOLERANCE && c.size > best_tile {
                best_tile = c.size;
            }
        }

        // 记录选择原因
        let actual_penalty = candidates
            .iter()
            .find(|c| c.size == best_tile)
            .map(|c| (c.est_time - min_time) / min_time * 100.0)
            .unwrap_or(0.0);
        // Callers log the chosen size per image already; this stays INFO only
        // on the run that actually benchmarked.
        if probed {
            log::info!(target: "TileOptimizer",
                "Optimal tile size: {} (min_time={}s, penalty={}%)",
                best_tile, min_time, actual_penalty);
        } else {
            log::debug!(target: "TileOptimizer",
                "Optimal tile size: {} (min_time={}s, penalty={}%)",
                best_tile, min_time, actual_penalty);
        }

        best_tile
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockEngine;
    use crate::engine::EngineError;

    /// Instant fake engine with a configurable memory budget and hardware id,
    /// wrapping MockEngine for the real process math.
    struct FakeEngine {
        mock: MockEngine,
        budget_mb: u64,
        hw: String,
        /// Per-call sleep (throttled-device heuristic tests).
        call_delay_ms: u64,
    }

    impl FakeEngine {
        fn new(budget_mb: u64, hw: &str) -> Self {
            Self { mock: MockEngine::new(), budget_mb, hw: hw.into(), call_delay_ms: 0 }
        }
    }

    impl Engine for FakeEngine {
        fn initialize(&mut self, d: DeviceType) -> Result<(), EngineError> {
            self.mock.initialize(d)
        }
        fn list_devices(&self) -> Vec<crate::engine::DeviceCapability> {
            self.mock.list_devices()
        }
        fn load_model(&mut self, p: &Path, b: &Path) -> Result<(), EngineError> {
            self.mock.load_model(p, b)
        }
        fn process(
            &self,
            input: &[f32],
            output: &mut [f32],
            width: i32,
            height: i32,
            channels: i32,
            cancel: &AtomicBool,
        ) -> Result<(), EngineError> {
            if self.call_delay_ms > 0 {
                std::thread::sleep(std::time::Duration::from_millis(self.call_delay_ms));
            }
            self.mock.process(input, output, width, height, channels, cancel)
        }
        fn benchmark(&self, t: i32) -> f32 {
            self.mock.benchmark(t)
        }
        fn unload_model(&mut self) {
            self.mock.unload_model()
        }
        fn current_device(&self) -> DeviceType {
            self.mock.current_device()
        }
        fn is_initialized(&self) -> bool {
            self.mock.is_initialized()
        }
        fn model_name(&self) -> String {
            self.mock.model_name()
        }
        fn get_scale_factor(&self) -> i32 {
            self.mock.get_scale_factor()
        }
        fn get_memory_info(&self) -> (u64, u64) {
            (self.budget_mb, self.budget_mb)
        }
        fn hardware_id(&self) -> String {
            self.hw.clone()
        }
    }

    fn config() -> ProcessingConfig {
        ProcessingConfig {
            min_tile_size: 64,
            max_tile_size: 512,
            tile_overlap: 16,
            ..Default::default()
        }
    }

    fn model() -> ModelConstraints {
        ModelConstraints { model_name: "mock-x2".into(), ..Default::default() }
    }

    fn empty_state() -> TileBenchCache {
        TileBenchCache { map: HashMap::new(), loaded: false }
    }

    fn key(hw: &str, tile: i32) -> TileBenchKey {
        TileBenchKey { hw: hw.into(), model_name: "mock-x2".into(), device: 0, tile_size: tile }
    }

    fn seeded(hw: &str, rates: &[(i32, f32, u64)]) -> TileBenchCache {
        let mut state = empty_state();
        for &(tile, tps, mem) in rates {
            state.map.insert(
                key(hw, tile),
                TileBenchmark {
                    tile_size: tile,
                    tiles_per_second: tps,
                    memory_usage_mb: mem,
                    success: true,
                    measured_at_sec: unix_now_sec(),
                },
            );
        }
        state
    }

    #[test]
    fn tolerance_band_prefers_largest_within_5_percent() {
        let engine = FakeEngine::new(0, "hw");

        // Craft rates so every size's estimated time is identical (E=1s):
        // tiles/sec = tile_count / E. The band must then pick the largest
        // candidate, 512.
        let image = 4096i32;
        let overlap = 16;
        let tiles = |size: i32| -> f32 {
            let step = size - overlap;
            let tx = (image + step - 1) / step;
            (tx * tx) as f32
        };
        let sizes = [64, 128, 192, 256, 320, 384, 448, 512];
        let rates: Vec<(i32, f32, u64)> =
            sizes.iter().map(|&s| (s, tiles(s) / 1.0, 1)).collect();

        let mut state = seeded("hw", &rates);
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), image, image, &mut state, None,
        );
        assert_eq!(best, 512, "equal est times → the largest candidate wins");

        // Now push 512 outside the band (+10% est time): 448 must win.
        let mut rates2 = rates.clone();
        *rates2.last_mut().unwrap() = (512, tiles(512) / 1.10, 1);
        let mut state = seeded("hw", &rates2);
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), image, image, &mut state, None,
        );
        assert_eq!(best, 448, "a +10% outlier must fall outside the 1.05 band");

        // Border case: 512 at exactly +4.9% stays inside → still 512.
        let mut rates3 = rates.clone();
        *rates3.last_mut().unwrap() = (512, tiles(512) / 1.049, 1);
        let mut state = seeded("hw", &rates3);
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), image, image, &mut state, None,
        );
        assert_eq!(best, 512);
    }

    #[test]
    fn filtered_window_and_fallbacks() {
        let engine = FakeEngine::new(0, "hw");

        // Config window excludes every candidate → fallback clamps the
        // default 256 into [576, 1024] = 576.
        let mut cfg = config();
        cfg.min_tile_size = 576;
        cfg.max_tile_size = 1024;
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &cfg, 1024, 1024, &mut empty_state(), None,
        );
        assert_eq!(best, 576);

        // Model max below every candidate: window [64, 48] is contradictory →
        // default 256 (validate() owns the contradiction).
        let mut m = model();
        m.max_tile_size = 48;
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &m, &config(), 1024, 1024, &mut empty_state(), None,
        );
        assert_eq!(best, 256);

        // Empty-image probe still returns a sane size from the unfiltered set
        // (rates are near-instant here, so only sanity-check the range).
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 8, 8, &mut empty_state(), None,
        );
        assert!((64..=512).contains(&best));
    }

    #[test]
    fn memory_budget_eliminates_big_tiles() {
        // mem(tile,3,2,mult10): 64→2MB, 128→10, 192→23, 256→41, 320→64,
        // 448→126, 512→412. Budget 100MB → usable 80: 448/512 eliminated;
        // all others equal-timed → largest survivor 320.
        let engine = FakeEngine::new(100, "hw");
        let state = seeded(
            "hw",
            &[(64, 100.0, 2), (128, 100.0, 10), (192, 100.0, 23), (256, 100.0, 41),
              (320, 100.0, 64), (448, 100.0, 126), (512, 100.0, 412)],
        );
        let mut state = state;
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 4096, 4096, &mut state, None,
        );
        assert_eq!(best, 320, "budget must cap the choice at 320, got {best}");

        // Budget 20MB → usable 16: only 64/128 survive → 128.
        let engine = FakeEngine::new(20, "hw");
        let mut state = seeded(
            "hw",
            &[(64, 100.0, 2), (128, 100.0, 10), (192, 100.0, 23), (256, 100.0, 41)],
        );
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 4096, 4096, &mut state, None,
        );
        assert_eq!(best, 128);

        // Unknown budget (0) → memory filter skipped entirely.
        let engine = FakeEngine::new(0, "hw");
        let mut state = seeded(
            "hw",
            &[(64, 100.0, 2), (512, 100.0, 412)],
        );
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 4096, 4096, &mut state, None,
        );
        assert_eq!(best, 512);
    }

    #[test]
    fn benchmark_runs_and_rates_are_positive() {
        let engine = FakeEngine::new(0, "hw");
        let bench = TileSizeOptimizer::benchmark_tile_size(&engine, 64, 3, 1);
        assert!(bench.success);
        assert!(bench.tiles_per_second.is_finite() && bench.tiles_per_second > 0.0);
        assert_eq!(bench.memory_usage_mb, 2);
    }

    #[test]
    fn cache_roundtrip_and_hw_invalidation() {
        let dir = std::env::temp_dir().join(format!("upix-core-opt-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let cache_path = dir.join("auto_tile_cache.json");
        let engine = FakeEngine::new(0, "test-hw-001");

        // Run once: benchmarks all 8 sizes and persists (hw is set).
        let mut state = empty_state();
        let best = TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 256, 256, &mut state, Some(&cache_path),
        );
        assert!((64..=512).contains(&best));
        assert!(cache_path.exists(), "cache file must be written");
        let written = std::fs::read_to_string(&cache_path).unwrap();
        let parsed: TileCacheFile = serde_json::from_str(&written).unwrap();
        assert_eq!(parsed.version, 1);
        assert_eq!(parsed.entries.len(), 8);
        assert!(parsed.entries.iter().all(|e| e.hw == "test-hw-001"));
        // Field names match the C++ glaze layout.
        assert!(written.contains(r#""version":1,"entries":[{"hw":"test-hw-001","model":"mock-x2","device":0,"tile":"#));

        // Reload from disk into a fresh store: entries merge.
        let mut state2 = empty_state();
        TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 256, 256, &mut state2, Some(&cache_path),
        );
        assert_eq!(state2.map.len(), 8, "disk rows must merge into the fresh store");

        // Foreign-version / unparsable files are ignored by the loader
        // (tested directly on tile_bench_load_once; a full probe would simply
        // re-measure and rewrite a fresh v1 file, like the C++).
        std::fs::write(&cache_path, r#"{"version":2,"entries":[]}"#).unwrap();
        let mut state3 = empty_state();
        tile_bench_load_once(&mut state3, Some(&cache_path));
        assert!(state3.map.is_empty(), "foreign-version cache must be ignored");

        std::fs::write(&cache_path, "not json").unwrap();
        let mut state4 = empty_state();
        tile_bench_load_once(&mut state4, Some(&cache_path));
        assert!(state4.map.is_empty());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn stale_entries_revalidate_and_max_merge() {
        let engine = FakeEngine::new(0, "hw");
        let mut state = empty_state();
        let k = key("hw", 256);
        // Stale but plausible entry (older than TTL).
        state.map.insert(
            k.clone(),
            TileBenchmark {
                tile_size: 256,
                tiles_per_second: 30.0,
                memory_usage_mb: 10,
                success: true,
                measured_at_sec: unix_now_sec() - (PROBE_TTL_SEC + 3600),
            },
        );
        // The fake engine is instant → re-measure yields a much higher rate;
        // max-merge keeps the faster value and the TTL re-arms.
        TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 512, 512, &mut state, None,
        );
        let merged = &state.map[&k];
        assert!(merged.tiles_per_second >= 30.0, "max-merge keeps the faster rate");
        assert!(merged.measured_at_sec >= unix_now_sec() - 2, "TTL re-armed");

        // A re-measure landing far below the cached rate keeps the cached
        // timing (busy/throttled device heuristic).
        let mut throttled = FakeEngine::new(0, "thw");
        throttled.call_delay_ms = 15; // ≈66 tps
        let mut state = empty_state();
        let k = key("thw", 256);
        state.map.insert(
            k.clone(),
            TileBenchmark {
                tile_size: 256,
                tiles_per_second: 1000.0,
                memory_usage_mb: 10,
                success: true,
                measured_at_sec: unix_now_sec() - (PROBE_TTL_SEC + 3600),
            },
        );
        TileSizeOptimizer::find_optimal_tile_size_inner(
            &throttled, &model(), &config(), 512, 512, &mut state, None,
        );
        assert_eq!(state.map[&k].tiles_per_second, 1000.0,
            "66tps < 70% of 1000 → cached timing kept");
    }

    #[test]
    fn fingerprintless_devices_stay_process_local() {
        let dir = std::env::temp_dir().join(format!("upix-core-opt2-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let cache_path = dir.join("auto_tile_cache.json");
        let engine = FakeEngine::new(0, ""); // hw empty = unknown device

        let mut state = empty_state();
        TileSizeOptimizer::find_optimal_tile_size_inner(
            &engine, &model(), &config(), 128, 128, &mut state, Some(&cache_path),
        );
        assert!(!state.map.is_empty(), "in-memory entries exist");
        // The C++ save writes the file regardless (only failing /
        // fingerprint-less ROWS are skipped), so the file exists but carries
        // zero entries.
        let written = std::fs::read_to_string(&cache_path).unwrap();
        let parsed: TileCacheFile = serde_json::from_str(&written).unwrap();
        assert_eq!(parsed.version, 1);
        assert!(parsed.entries.is_empty(), "fingerprint-less rows must not persist");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn failed_entries_never_reach_disk() {
        // Benchmark failures (success=false) live in the process store only:
        // the save filter must drop them even when the hardware id is set.
        let dir = std::env::temp_dir().join(format!("upix-core-opt3-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let cache_path = dir.join("auto_tile_cache.json");

        let mut state = seeded("fhw", &[(256, 100.0, 10)]);
        state.map.insert(
            key("fhw", 512),
            TileBenchmark {
                tile_size: 512,
                tiles_per_second: 0.0,
                memory_usage_mb: 0,
                success: false,
                measured_at_sec: unix_now_sec(),
            },
        );
        tile_bench_save(&state.map, Some(&cache_path));

        let written = std::fs::read_to_string(&cache_path).unwrap();
        let parsed: TileCacheFile = serde_json::from_str(&written).unwrap();
        assert_eq!(parsed.entries.len(), 1, "only the successful row persists");
        assert_eq!(parsed.entries[0].tile, 256);

        let _ = std::fs::remove_dir_all(&dir);
    }
}
