#pragma once

#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include <string>
#include "utils/expected.hpp"

namespace crossscale {

struct TileRegion {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t overlap;
};

struct TileConfig {
    int tile_size = 256;
    int overlap = 16;
    int batch_size = 1;
};

struct ProcessingProgress {
    int total_tiles = 0;
    int completed_tiles = 0;
    uint32_t current_tile_x = 0;
    uint32_t current_tile_y = 0;
    
    float percentage() const {
        if (total_tiles == 0) return 0.0f;
        return static_cast<float>(completed_tiles) / total_tiles * 100.0f;
    }
};

struct TileProcessorError {
    int code;
    std::string message;
};

class Image;
class Engine;

class TileProcessor {
public:
    auto compute_tile_layout(
        int image_width,
        int image_height,
        const TileConfig& config
    ) -> std::vector<TileRegion>;
    
    auto process_image(
        const float* input_data,
        float* output_data,
        int input_width,
        int input_height,
        int channels,
        Engine& engine,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback = {},
        const std::atomic<bool>& cancel_flag = {}
    ) -> expected<void, TileProcessorError>;
    
    auto extract_tile(
        const float* image_data,
        int image_width,
        int image_height,
        int channels,
        const TileRegion& region
    ) -> std::vector<float>;
    
    auto blend_tile(
        const float* tile_data,
        float* output_data,
        int output_width,
        int output_height,
        int channels,
        const TileRegion& region,
        int scale_factor,
        int overlap
    ) -> void;

private:
    auto compute_blend_weight(int pos, int size, int overlap) -> float;
};

} // namespace crossscale
