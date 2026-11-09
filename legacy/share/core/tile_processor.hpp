#pragma once

#include <vector>
#include <functional>
#include <atomic>
#include <string>
#include <cstdint>
#include "core/tile_types.hpp"
#include "utils/expected.hpp"

namespace upix {

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
    
    // Float tiled pipeline. A 4-channel image fed to an engine whose model
    // takes 3 input channels (engine.model_input_channels() == 3) has its alpha
    // split off: the net sees RGB and the output's 4th channel is carried by
    // the same host-side bicubic the u8 path's alpha bypass uses.
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
        const std::atomic<bool>& cancel_flag = {},
        bool enable_alpha_bicubic = true
    ) -> expected<void, TileProcessorError>;
    
    // Extract one tile as interleaved floats. `src_channels` is the source
    // pixel stride, `dst_channels` the tile stride; copying only the leading
    // `dst_channels` is how an RGBA source feeds an RGB model (or vice versa)
    // without a whole-image conversion.
    static auto extract_tile(
        const float* image_data,
        int image_width,
        int src_channels,
        int dst_channels,
        const TileRegion& region
    ) -> std::vector<float>;

    // Extract tile from u8 data and convert to float per-tile (avoids whole-image conversion)
    static auto extract_tile_u8(
        const uint8_t* image_data,
        int image_width,
        int src_channels,
        int dst_channels,
        const TileRegion& region
    ) -> std::vector<float>;

    // Bicubic interpolation for a single-channel u8 image (used for alpha bypass)
    static void bicubic_scale_u8(
        const uint8_t* src, uint32_t src_w, uint32_t src_h,
        uint8_t* dst, uint32_t dst_w, uint32_t dst_h
    );
    
    auto blend_tile(
        const float* tile_data,
        float* output_data,
        float* weight_data,
        int output_width,
        int output_height,
        int channels,
        const TileRegion& region,
        int scale_factor,
        int overlap
    ) -> void;

private:
};

} // namespace upix
