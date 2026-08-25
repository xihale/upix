#pragma once

#include "config.hpp"
#include "utils/logger.hpp"
#include "utils/benchmark.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace crossscale {

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
// Auto-tile probe cache
//
// The probe benchmarks up to 8 tile sizes with (2 warmup + 5 timed) full
// forward passes each — a large fixed cost that dominates wall time when a
// directory contains many images of the same shape (screenshots, anime
// frames). Tile size only affects performance, never output correctness, so
// the probe decision is cached per (model, device type, dimension bucket)
// and reused. Dimensions are bucketed by rounding up to multiples of 256:
// images in the same bucket produce the same tile grid at every candidate
// size, so the probe result transfers. Inline functions with function-local
// statics are shared across translation units (ODR), and the mutex guards
// the multi-threaded batch path.
// ---------------------------------------------------------------------------
namespace detail {

struct TileProbeKey {
    std::string model_name;
    int device = 0;          // DeviceType as int
    int width_bucket = 0;    // ceil(width / 256)
    int height_bucket = 0;   // ceil(height / 256)

    bool operator==(const TileProbeKey& o) const {
        return device == o.device
            && width_bucket == o.width_bucket
            && height_bucket == o.height_bucket
            && model_name == o.model_name;
    }
};

struct TileProbeKeyHash {
    size_t operator()(const TileProbeKey& k) const {
        size_t h = std::hash<int>{}((k.device * 31 + k.width_bucket) * 31 + k.height_bucket);
        return h ^ (std::hash<std::string>{}(k.model_name) << 1);
    }
};

inline auto tile_probe_cache() -> std::unordered_map<TileProbeKey, int, TileProbeKeyHash>& {
    static std::unordered_map<TileProbeKey, int, TileProbeKeyHash> cache;
    return cache;
}

inline auto tile_probe_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

} // namespace detail

inline auto TileSizeOptimizer::find_optimal_tile_size(
    Engine& engine,
    const ModelConstraints& model,
    const ProcessingConfig& config,
    int image_width,
    int image_height
) -> int {

    // Probe cache lookup: (model, device type, quantized dimensions).
    constexpr int DIM_BUCKET = 256;
    detail::TileProbeKey key{
        model.model_name,
        static_cast<int>(engine.current_device()),
        (image_width + DIM_BUCKET - 1) / DIM_BUCKET,
        (image_height + DIM_BUCKET - 1) / DIM_BUCKET,
    };

    {
        std::lock_guard<std::mutex> lock(detail::tile_probe_cache_mutex());
        auto& cache = detail::tile_probe_cache();
        if (auto it = cache.find(key); it != cache.end()) {
            LOG_DEBUG("TileOptimizer", "Auto-tile cache HIT: model={} bucket={}x{} -> tile_size={}",
                      key.model_name, key.width_bucket, key.height_bucket, it->second);
            return it->second;
        }
    }
    LOG_DEBUG("TileOptimizer", "Auto-tile cache MISS: model={} bucket={}x{} -> running probe",
              key.model_name, key.width_bucket, key.height_bucket);

    LOG_INFO("TileOptimizer", "Finding optimal tile size...");

    // 获取设备信息
    auto profile = profile_device(engine);

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

    for (int size : test_sizes) {
        LOG_INFO("TileOptimizer", "Benchmarking tile_size={}", size);

        auto bench = benchmark_tile_size(engine, size);

        if (!bench.success) {
            LOG_WARNING("TileOptimizer", "Tile size {} failed", size);
            continue;
        }

        // 内存约束检查：必须不超过可用内存的 80%
        if (bench.memory_usage_mb > usable_memory_mb) {
            LOG_WARNING("TileOptimizer", "  Skipping: memory usage {}MB exceeds usable {}MB",
                        bench.memory_usage_mb, usable_memory_mb);
            continue;
        }

        // 计算该尺寸下的 tile 数量（考虑重叠）
        int step = size - config.tile_overlap;
        if (step <= 0) {
            LOG_WARNING("TileOptimizer", "  Skipping: invalid step (tile_size <= overlap)");
            continue;
        }

        int tiles_x = (image_width + step - 1) / step;
        int tiles_y = (image_height + step - 1) / step;
        int total_tiles = tiles_x * tiles_y;

        // 估算总处理时间（秒）
        float estimated_time_sec = total_tiles / bench.tiles_per_second;

        LOG_INFO("TileOptimizer", "  tiles/sec={} memory={}MB total_tiles={} est_time={}s",
                 bench.tiles_per_second, bench.memory_usage_mb, total_tiles,
                 estimated_time_sec);

        candidates.push_back({size, estimated_time_sec, bench.tiles_per_second, bench.memory_usage_mb});
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
    LOG_INFO("TileOptimizer", "Optimal tile size: {} (min_time={}s, penalty={}%)",
             best_tile, min_time, actual_penalty);

    {
        std::lock_guard<std::mutex> lock(detail::tile_probe_cache_mutex());
        detail::tile_probe_cache().emplace(key, best_tile);
    }

    return best_tile;
}

} // namespace crossscale
