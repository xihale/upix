#pragma once

#include "config.hpp"
#include "utils/logger.hpp"
#include "utils/benchmark.hpp"
#include "glaze/glaze.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace upix {

// Member-function definitions below are `inline` because this header is
// included by multiple TUs in the CLI binary. `inline` only relaxes the
// one-definition rule; the generated code is unchanged.
inline auto TileSizeOptimizer::calculate_tile_memory(
    int tile_size,
    int channels,
    int scale_factor,
    const ModelConstraints& model
) const -> size_t {
    // 输入 tile 内存
    size_t input_mem = static_cast<size_t>(tile_size) * tile_size * channels * sizeof(float);

    // 输出 tile 内存
    int out_size = tile_size * scale_factor;
    size_t output_mem = static_cast<size_t>(out_size) * out_size * channels * sizeof(float);

    // 中间激活内存（模型相关）
    size_t activation_mem = (input_mem + output_mem) *
        static_cast<size_t>(model.memory_multiplier);

    // 返回 MB
    return (input_mem + output_mem + activation_mem + model.model_size_mb * 1024 * 1024)
           / (1024 * 1024);
}

inline auto TileSizeOptimizer::benchmark_tile_size(
    Engine& engine,
    int tile_size,
    int iterations,
    int warmup
) -> DevicePerformanceProfile::TileBenchmark {

    DevicePerformanceProfile::TileBenchmark result;
    result.tile_size = tile_size;
    result.success = false;

    int scale = engine.get_scale_factor();

    try {
        std::vector<float> input(static_cast<size_t>(tile_size) * tile_size * 3, 0.5f);
        std::vector<float> output(static_cast<size_t>(tile_size) * tile_size * 3 * scale * scale);

        std::atomic<bool> cancel = false;

        auto bench_result = benchmark("tile_bench", [&]() {
            // fire-and-forget: benchmark measures timing only; on failure
            // success stays false and the tile size is rejected below.
            (void)engine.process(input.data(), output.data(),
                                 tile_size, tile_size, 3, cancel);
        }, iterations, warmup);

        result.tiles_per_second = 1000.0f / static_cast<float>(bench_result.avg_time_ms);
        result.success = true;

        // 估算内存使用
        ModelConstraints dummy_model;
        dummy_model.memory_multiplier = 10.0f;
        result.memory_usage_mb = calculate_tile_memory(tile_size, 3, scale, dummy_model);

    } catch (const std::exception& e) {
        LOG_WARNING("TileOptimizer", "Benchmark failed for tile_size={}: {}",
                    tile_size, e.what());
        result.success = false;
    }

    return result;
}

inline auto TileSizeOptimizer::profile_device(Engine& engine) -> DevicePerformanceProfile {
    DevicePerformanceProfile profile;
    profile.available_memory_mb = engine.get_memory_info().second;

    return profile;
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
// Entries carry a timestamp and revalidate after kProbeTtlSec so even a
// first-ever probe measured under load self-heals within the TTL window.
// Failed benchmarks stay process-local (a failure can be a transient OOM
// under external memory pressure, which must not freeze the size out across
// runs). Inline functions with function-local statics are shared across
// translation units (ODR); the mutex also serializes benchmarking, because
// concurrent probes would contend for the device and pollute each other.
// ---------------------------------------------------------------------------
namespace detail {

// Probe results older than this are re-measured (and max-merged) once, so a
// measurement polluted by load, or a machine that genuinely changed under the
// same fingerprint, cannot persist forever.
constexpr int64_t kProbeTtlSec = 14 * 24 * 3600;

struct TileBenchKey {
    std::string hw;          // Engine::hardware_id(); empty = unknown device
    std::string model_name;
    int device = 0;          // DeviceType as int
    int tile_size = 0;

    bool operator==(const TileBenchKey& o) const {
        return tile_size == o.tile_size
            && device == o.device
            && model_name == o.model_name
            && hw == o.hw;
    }
};

struct TileBenchKeyHash {
    size_t operator()(const TileBenchKey& k) const {
        size_t h = std::hash<int>{}(k.device * 31 + k.tile_size);
        h ^= std::hash<std::string>{}(k.model_name) << 1;
        return h ^ (std::hash<std::string>{}(k.hw) << 2);
    }
};

// On-disk row. Kept in a named (non-anonymous) namespace: glaze's reflection
// requires external linkage. Only successful measurements are written.
struct TileCacheEntry {
    std::string hw;
    std::string model;
    int device = 0;
    int tile = 0;
    float tps = 0.0f;
    size_t mem_mb = 0;
    int64_t t = 0;
};

struct TileCacheFile {
    int version = 1;
    std::vector<TileCacheEntry> entries;
};

using TileBenchCache = std::unordered_map<TileBenchKey, DevicePerformanceProfile::TileBenchmark, TileBenchKeyHash>;

inline auto tile_bench_cache() -> TileBenchCache& {
    static TileBenchCache cache;
    return cache;
}

inline auto tile_bench_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

// $UPIX_TILE_CACHE overrides (tests), else $XDG_CACHE_HOME/upix/, else
// $HOME/.cache/upix/. An empty return disables persistence silently.
inline auto tile_cache_path() -> std::filesystem::path {
    if (const char* env = std::getenv("UPIX_TILE_CACHE"); env && *env) {
        return {env};
    }
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path base = (xdg && *xdg) ? std::filesystem::path(xdg)
                                               : std::filesystem::path();
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            return {};
        }
        base = std::filesystem::path(home) / ".cache";
    }
    return base / "upix" / "auto_tile_cache.json";
}

inline auto unix_now_sec() -> int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Merge disk rows into the cache. Once per process, under the probe mutex.
// Unparsable or foreign-version files are ignored and rewritten on save.
inline auto tile_bench_load_once() -> void {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;

    const auto path = tile_cache_path();
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    TileCacheFile parsed;
    if (auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, json); err) {
        LOG_DEBUG("TileOptimizer", "Auto-tile cache unreadable ({}), starting fresh",
                  glz::format_error(err, json));
        return;
    }
    if (parsed.version != 1) {
        return;
    }

    size_t merged = 0;
    auto& cache = tile_bench_cache();
    for (const auto& e : parsed.entries) {
        TileBenchKey key{e.hw, e.model, e.device, e.tile};
        if (cache.contains(key)) {
            continue;  // a measurement taken this process is fresher
        }
        cache.emplace(std::move(key), DevicePerformanceProfile::TileBenchmark{
            e.tile, e.tps, e.mem_mb, true, e.t});
        ++merged;
    }
    if (merged > 0) {
        LOG_DEBUG("TileOptimizer", "Auto-tile cache: {} entries loaded from {}",
                  merged, path.string());
    }
}

// Rewrite the whole file from the in-memory map (atomic tmp+rename). Any
// failure is debug-logged only — the cache is a pure optimization and must
// never fail a job. Called under the probe mutex.
inline auto tile_bench_save() -> void {
    const auto path = tile_cache_path();
    if (path.empty()) {
        return;
    }

    TileCacheFile out;
    out.entries.reserve(tile_bench_cache().size());
    for (const auto& [key, bench] : tile_bench_cache()) {
        if (!bench.success || key.hw.empty()) {
            continue;  // failures / fingerprint-less devices stay process-local
        }
        out.entries.push_back({key.hw, key.model_name, key.device, key.tile_size,
                               bench.tiles_per_second, bench.memory_usage_mb,
                               bench.measured_at_sec});
    }
    auto json = glz::write_json(out);
    if (!json) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::string tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_DEBUG("TileOptimizer", "Auto-tile cache: cannot open {} for writing", tmp);
            return;
        }
        file << *json;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // rename-over-existing is not portable everywhere; fall back once.
        std::error_code rm_ec;
        std::filesystem::remove(path, rm_ec);
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        LOG_DEBUG("TileOptimizer", "Auto-tile cache: failed to store {}: {}",
                  path.string(), ec.message());
    }
}

} // namespace detail

inline auto TileSizeOptimizer::find_optimal_tile_size(
    Engine& engine,
    const ModelConstraints& model,
    const ProcessingConfig& config,
    int image_width,
    int image_height
) -> int {

    // 测试的 tile 大小列表
    std::vector<int> test_sizes = {64, 128, 192, 256, 320, 384, 448, 512};

    // 根据配置过滤
    test_sizes.erase(
        std::remove_if(test_sizes.begin(), test_sizes.end(),
            [&](int s) { return s < config.min_tile_size || s > config.max_tile_size; }),
        test_sizes.end()
    );

    // 根据模型约束过滤
    if (model.min_tile_size > 0) {
        test_sizes.erase(
            std::remove_if(test_sizes.begin(), test_sizes.end(),
                [&](int s) { return s < model.min_tile_size; }),
            test_sizes.end()
        );
    }
    test_sizes.erase(
        std::remove_if(test_sizes.begin(), test_sizes.end(),
            [&](int s) { return s > model.max_tile_size; }),
        test_sizes.end()
    );

    if (test_sizes.empty()) {
        LOG_WARNING("TileOptimizer", "No valid tile sizes after filtering, using default 256");
        return 256;
    }

    // Probe + selection run under one lock: benchmarking must be serialized
    // process-wide (concurrent probes would contend for the device and
    // pollute each other's timings), and a worker that loses that race
    // re-finds the cache filled here and measures nothing.
    const int device = static_cast<int>(engine.current_device());
    const std::string hw = engine.hardware_id();

    // 获取设备信息
    auto profile = profile_device(engine);

    // 计算可用内存上限 (MB)
    size_t usable_memory_mb = static_cast<size_t>(profile.available_memory_mb * 0.8);

    // 第一阶段：基准测试，收集所有通过内存约束的候选尺寸及其估算时间
    struct Candidate {
        int size;
        float est_time;
        float tiles_per_second;
        size_t memory_mb;
    };
    std::vector<Candidate> candidates;
    bool probed = false;
    bool dirty = false;

    {
        std::lock_guard<std::mutex> lock(detail::tile_bench_mutex());
        detail::tile_bench_load_once();
        auto& cache = detail::tile_bench_cache();
        const int64_t now_sec = detail::unix_now_sec();

        for (int size : test_sizes) {
            auto key = detail::TileBenchKey{hw, model.model_name, device, size};
            auto it = cache.find(key);
            const bool revalidate = it != cache.end() && it->second.success
                && now_sec - it->second.measured_at_sec > detail::kProbeTtlSec;

            if (it == cache.end() || revalidate) {
                if (!probed) {
                    LOG_INFO("TileOptimizer", "Finding optimal tile size (model={})...",
                             model.model_name);
                    probed = true;
                }
                LOG_INFO("TileOptimizer", "Benchmarking tile_size={}{}", size,
                         revalidate ? " (cached timing stale, revalidating)" : "");

                auto bench = benchmark_tile_size(engine, size);
                bench.measured_at_sec = now_sec;

                if (!bench.success) {
                    if (revalidate) {
                        // Keep the previously measured rate (still the best
                        // data we have); just re-arm the TTL.
                        it->second.measured_at_sec = now_sec;
                        dirty = true;
                    } else {
                        LOG_WARNING("TileOptimizer", "Tile size {} failed", size);
                        // Failures are process-local: never written to disk
                        // (a benchmark OOM under external memory pressure
                        // must not freeze the size out across runs).
                        it = cache.emplace(std::move(key), std::move(bench)).first;
                    }
                } else if (revalidate) {
                    const float cached_tps = it->second.tiles_per_second;
                    if (bench.tiles_per_second < cached_tps * 0.7f) {
                        LOG_WARNING("TileOptimizer",
                                    "Tile size {} re-measured at {:.0f}% of the cached rate — "
                                    "device busy or throttled, keeping the cached timing",
                                    size, 100.0f * bench.tiles_per_second / cached_tps);
                        it->second.measured_at_sec = now_sec;
                    } else {
                        // Contention can only depress a measurement, never
                        // inflate it, so keep the faster of the two.
                        bench.tiles_per_second = std::max(bench.tiles_per_second, cached_tps);
                        it->second = std::move(bench);
                    }
                    dirty = true;
                } else {
                    it = cache.emplace(std::move(key), std::move(bench)).first;
                    dirty = true;
                }
            }

            const auto& bench = it->second;
            if (!bench.success) {
                // Measured-failed earlier in this process; retried never.
                continue;
            }

            // 内存约束检查：必须不超过可用内存的 80%
            if (bench.memory_usage_mb > usable_memory_mb) {
                LOG_WARNING("TileOptimizer", "  Skipping tile_size={}: memory usage {}MB exceeds usable {}MB",
                            size, bench.memory_usage_mb, usable_memory_mb);
                continue;
            }

            // 计算该尺寸下的 tile 数量（考虑重叠）
            int step = size - config.tile_overlap;
            if (step <= 0) {
                LOG_WARNING("TileOptimizer", "  Skipping tile_size={}: invalid step (tile_size <= overlap)",
                            size);
                continue;
            }

            int tiles_x = (image_width + step - 1) / step;
            int tiles_y = (image_height + step - 1) / step;
            int total_tiles = tiles_x * tiles_y;

            // 估算总处理时间（秒）
            float estimated_time_sec = total_tiles / bench.tiles_per_second;

            LOG_DEBUG("TileOptimizer", "  tile_size={} tiles/sec={} memory={}MB total_tiles={} est_time={}s",
                      size, bench.tiles_per_second, bench.memory_usage_mb, total_tiles,
                      estimated_time_sec);

            candidates.push_back({size, estimated_time_sec, bench.tiles_per_second, bench.memory_usage_mb});
        }

        if (dirty) {
            detail::tile_bench_save();
        }
    }

    if (candidates.empty()) {
        LOG_WARNING("TileOptimizer", "No valid tile size candidates, using fallback 256");
        return 256;
    }

    // 找出最小估算时间
    float min_time = std::numeric_limits<float>::max();
    for (const auto& c : candidates) {
        if (c.est_time < min_time) min_time = c.est_time;
    }

    // 容忍阈值：在最小时间的 +5% 范围内，选择最大的 tile size（以提高 GPU 利用率）
    const float TOLERANCE = 1.05f;
    int best_tile = candidates[0].size;
    for (const auto& c : candidates) {
        if (c.est_time <= min_time * TOLERANCE && c.size > best_tile) {
            best_tile = c.size;
        }
    }

    // 记录选择原因
    float actual_penalty = 0.0f;
    for (const auto& c : candidates) {
        if (c.size == best_tile) {
            actual_penalty = (c.est_time - min_time) / min_time * 100.0f;
            break;
        }
    }
    // Callers log the chosen size per image already; this stays INFO only on
    // the run that actually benchmarked.
    if (probed) {
        LOG_INFO("TileOptimizer", "Optimal tile size: {} (min_time={}s, penalty={}%)",
                 best_tile, min_time, actual_penalty);
    } else {
        LOG_DEBUG("TileOptimizer", "Optimal tile size: {} (min_time={}s, penalty={}%)",
                  best_tile, min_time, actual_penalty);
    }

    return best_tile;
}

} // namespace upix
