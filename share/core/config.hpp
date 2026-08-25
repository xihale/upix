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

    // 性能设置
    bool use_gpu = true;

    // 模型设置
    std::string model_name;

    // 输出设置
    int output_quality = 90;
    std::string output_format;

    // 从 JSON 文件加载
    static auto load(const std::filesystem::path& path)
        -> expected<ProcessingConfig, std::string>;

    // 验证配置
    auto validate() const -> std::vector<std::string>;
};

// Output format helpers shared by CLI / Android.
auto normalize_output_format(std::string format) -> std::string;
auto is_supported_output_format(const std::string& format) -> bool;

// Filesystem-safe tag from a model name ("Real-ESRGANv3-anime" stays as-is,
// separators/spaces become '_'). Used to derive default output filenames.
auto sanitize_model_for_filename(const std::string& model) -> std::string;

auto resolve_output_path(
    const std::filesystem::path& requested_path,
    const ProcessingConfig& config
) -> std::filesystem::path;

// 设备性能档案（auto-tile 探测用，仅保留实际消费的字段）
struct DevicePerformanceProfile {
    size_t available_memory_mb = 0;

    struct TileBenchmark {
        int tile_size = 0;
        float tiles_per_second = 0.0f;
        size_t memory_usage_mb = 0;
        bool success = false;
    };
};

// 模型约束
struct ModelConstraints {
    std::string model_name;
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
