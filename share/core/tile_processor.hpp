#pragma once

#include <vector>
#include <functional>
#include <atomic>
#include <string>
#include <cstdint>
#include "core/tile_types.hpp"
#include "utils/expected.hpp"

namespace crossscale {

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

    auto process_image_u8(
        const uint8_t* input_data,
        uint8_t* output_data,
        int input_width,
        int input_height,
        int channels,
        Engine& engine,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback = {},
        const std::atomic<bool>& cancel_flag = {}
    ) -> expected<void, TileProcessorError>;
    
    static auto extract_tile(
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
