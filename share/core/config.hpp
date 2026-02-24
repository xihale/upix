#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <vector>
#include "utils/expected.hpp"
#include "engine.hpp"

namespace crossscale {

// 处理配置
struct ProcessingConfig {
    // 瓦片设置
    int tile_size = 256;
    int tile_overlap = 16;
    bool auto_tile_size = false;
    int min_tile_size = 64;
    int max_tile_size = 512;

    // 批处理设置
    int batch_size = 1;

    // 内存约束 (MB)
    size_t max_memory_mb = 0;  // 0 = 自动检测

    // 性能设置
    int num_threads = 0;  // 0 = 自动
    bool use_gpu = true;

    // 模型设置
    std::string model_name;
    std::filesystem::path models_dir = "./models";

    // 输出设置
    int output_quality = 90;
    std::string output_format;

    // Network settings
    size_t network_buffer_size = 0;  // 0 = auto (based on tile size)

    // 从 JSON 文件加载
    static auto load(const std::filesystem::path& path)
        -> expected<ProcessingConfig, std::string>;

    // 保存到 JSON 文件
    auto save(const std::filesystem::path& path) const
        -> expected<void, std::string>;

    // 验证配置
    auto validate() const -> std::vector<std::string>;
};

// Output format helpers shared by CLI / Android.
auto normalize_output_format(std::string format) -> std::string;
auto is_supported_output_format(const std::string& format) -> bool;
auto resolve_output_path(
    const std::filesystem::path& requested_path,
    const ProcessingConfig& config
) -> std::filesystem::path;

// 设备性能档案
struct DevicePerformanceProfile {
    DeviceType device_type = DeviceType::CPU;
    std::string device_name;

    size_t total_memory_mb = 0;
    size_t available_memory_mb = 0;

    int optimal_tile_size = 256;

    struct TileBenchmark {
        int tile_size = 0;
        float tiles_per_second = 0.0f;
        size_t memory_usage_mb = 0;
        bool success = false;
    };
    std::vector<TileBenchmark> tile_benchmarks;

    float performance_score = 0.0f;
};

// 模型约束
struct ModelConstraints {
    std::string model_name;
    int input_channels = 3;
    int scale_factor = 2;
    size_t model_size_mb = 0;
    float memory_multiplier = 10.0f;
    int min_tile_size = 0;
    int max_tile_size = 1024;
};

// 瓦片大小优化器
class TileSizeOptimizer {
public:
    auto find_optimal_tile_size(
        Engine& engine,
        const ModelConstraints& model,
        const ProcessingConfig& config,
        int image_width,
        int image_height
    ) -> int;

    auto estimate_tile_size(
        const DevicePerformanceProfile& device,
        const ModelConstraints& model
    ) const -> int;

    auto profile_device(Engine& engine) -> DevicePerformanceProfile;

    auto calculate_tile_memory(
        int tile_size,
        int channels,
        int scale_factor,
        const ModelConstraints& model
    ) const -> size_t;

    private:
        auto benchmark_tile_size(
            Engine& engine,
            int tile_size,
            int iterations = 10,
            int warmup = 2
        ) -> DevicePerformanceProfile::TileBenchmark;
};

} // namespace crossscale
