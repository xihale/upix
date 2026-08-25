#pragma once

#include <string>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>
#include "utils/expected.hpp"
#include "engine.hpp"

namespace upix {

// Compile-time defaults and validation bounds shared by ProcessingConfig,
// the JSON loader shape, and validate(). Single source of truth.
inline constexpr int kDefaultTileSize = 256;
inline constexpr int kDefaultTileOverlap = 16;
inline constexpr int kDefaultBatchSize = 1;
inline constexpr int kDefaultMinTileSize = 64;
inline constexpr int kDefaultMaxTileSize = 512;
inline constexpr int kDefaultOutputQuality = 90;

inline constexpr int kTileSizeMin = 16;
inline constexpr int kTileSizeMax = 2048;
inline constexpr int kBatchSizeMin = 1;
inline constexpr int kBatchSizeMax = 64;

// Supported normalized output formats.
inline constexpr std::array<std::string_view, 5> kSupportedOutputFormats{
    "png", "jpg", "jpeg", "webp", "avif"};

// 处理配置
struct ProcessingConfig {
    // 瓦片设置
    int tile_size = kDefaultTileSize;
    int tile_overlap = kDefaultTileOverlap;
    bool auto_tile_size = false;
    int min_tile_size = kDefaultMinTileSize;
    int max_tile_size = kDefaultMaxTileSize;

    // 批处理设置
    int batch_size = kDefaultBatchSize;

    // 性能设置
    bool use_gpu = true;

    // 模型设置
    std::string model_name;

    // 输出设置
    int output_quality = kDefaultOutputQuality;
    std::string output_format;

    // 从 JSON 文件加载
    static auto load(const std::filesystem::path& path)
        -> expected<ProcessingConfig, std::string>;

    // 验证配置
    auto validate() const -> std::vector<std::string>;
};

// Output format helpers shared by CLI / Android.
auto normalize_output_format(std::string_view format) -> std::string;
constexpr auto is_supported_output_format(std::string_view format) -> bool {
    for (std::string_view candidate : kSupportedOutputFormats) {
        if (format == candidate) {
            return true;
        }
    }
    return false;
}

// Filesystem-safe tag from a model name ("Real-ESRGANv3-anime" stays as-is,
// separators/spaces become '_'). Used to derive default output filenames.
auto sanitize_model_for_filename(std::string_view model) -> std::string;

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

} // namespace upix
