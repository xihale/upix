#include "tile_processor.hpp"
#include "engine.hpp"
#include "utils/logger.hpp"
#include <cmath>
#include <algorithm>

namespace crossscale {

auto TileProcessor::compute_tile_layout(int image_width, int image_height,
                                         const TileConfig& config) -> std::vector<TileRegion> {
    std::vector<TileRegion> regions;
    
    int tile_size = config.tile_size;
    int overlap = config.overlap;
    int step = tile_size - overlap;
    
    int tiles_x = std::max(1, (image_width - overlap + step - 1) / step);
    int tiles_y = std::max(1, (image_height - overlap + step - 1) / step);
    
    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            TileRegion region;
            region.x = static_cast<uint32_t>(std::min(tx * step, std::max(0, image_width - tile_size)));
            region.y = static_cast<uint32_t>(std::min(ty * step, std::max(0, image_height - tile_size)));
            region.width = static_cast<uint32_t>(std::min(tile_size, image_width - static_cast<int>(region.x)));
            region.height = static_cast<uint32_t>(std::min(tile_size, image_height - static_cast<int>(region.y)));
            region.overlap = static_cast<uint32_t>(overlap);
            
            regions.push_back(region);
        }
    }
    
    return regions;
}

auto TileProcessor::process_image(const float* input_data, float* output_data,
                                   int input_width, int input_height, int channels,
                                   Engine& engine, const TileConfig& config,
                                   std::function<void(const ProcessingProgress&)> progress_callback,
                                   const std::atomic<bool>& cancel_flag)
    -> expected<void, TileProcessorError> {
    
    auto regions = compute_tile_layout(input_width, input_height, config);
    int scale_factor = 2;
    
    ProcessingProgress progress;
    progress.total_tiles = static_cast<int>(regions.size());
    progress.completed_tiles = 0;
    
    int output_width = input_width * scale_factor;
    int output_height = input_height * scale_factor;
    
    std::vector<float> output_buffer(output_width * output_height * channels, 0.0f);
    std::vector<float> weight_buffer(output_width * output_height * channels, 0.0f);
    
    for (size_t i = 0; i < regions.size() && !cancel_flag; ++i) {
        const auto& region = regions[i];
        
        auto tile_input = extract_tile(input_data, input_width, input_height, 
                                        channels, region);
        
        int tile_out_width = region.width * scale_factor;
        int tile_out_height = region.height * scale_factor;
        std::vector<float> tile_output(tile_out_width * tile_out_height * channels);
        
        auto result = engine.process(tile_input.data(), tile_output.data(),
                                      region.width, region.height, channels, cancel_flag);
        
        if (!result.has_value()) {
            return TileProcessorError{1, "Processing failed"};
        }
        
        blend_tile(tile_output.data(), output_buffer.data(),
                   output_width, output_height, channels,
                   region, scale_factor, config.overlap);
        
        progress.completed_tiles = static_cast<int>(i + 1);
        progress.current_tile_x = region.x;
        progress.current_tile_y = region.y;
        
        if (progress_callback) {
            progress_callback(progress);
        }
    }
    
    if (cancel_flag) {
        return TileProcessorError{2, "Processing cancelled"};
    }
    
    std::copy(output_buffer.begin(), output_buffer.end(), output_data);
    return {};
}

auto TileProcessor::extract_tile(const float* image_data, int image_width, int image_height,
                                  int channels, const TileRegion& region) -> std::vector<float> {
    
    std::vector<float> tile(static_cast<size_t>(region.width) * region.height * channels);
    
    for (uint32_t y = 0; y < region.height; ++y) {
        for (uint32_t x = 0; x < region.width; ++x) {
            size_t src_idx = (static_cast<size_t>(region.y + y) * image_width + region.x + x) * channels;
            size_t dst_idx = (static_cast<size_t>(y) * region.width + x) * channels;
            
            for (int c = 0; c < channels; ++c) {
                tile[dst_idx + c] = image_data[src_idx + c];
            }
        }
    }
    
    return tile;
}

auto TileProcessor::blend_tile(const float* tile_data, float* output_data,
                                int output_width, int output_height, int channels,
                                const TileRegion& region, int scale_factor,
                                int overlap) -> void {
    
    uint32_t out_x = region.x * scale_factor;
    uint32_t out_y = region.y * scale_factor;
    uint32_t out_w = region.width * scale_factor;
    uint32_t out_h = region.height * scale_factor;
    int scaled_overlap = overlap * scale_factor;
    
    for (uint32_t y = 0; y < out_h; ++y) {
        for (uint32_t x = 0; x < out_w; ++x) {
            float weight = compute_blend_weight(x, out_w, scaled_overlap) *
                          compute_blend_weight(y, out_h, scaled_overlap);
            
            size_t dst_idx = (static_cast<size_t>(out_y + y) * output_width + out_x + x) * channels;
            size_t src_idx = (static_cast<size_t>(y) * out_w + x) * channels;
            
            for (int c = 0; c < channels; ++c) {
                output_data[dst_idx + c] += tile_data[src_idx + c] * weight;
            }
        }
    }
}

auto TileProcessor::compute_blend_weight(int pos, int size, int overlap) -> float {
    if (overlap <= 0) return 1.0f;
    
    float w = 1.0f;
    
    if (pos < overlap) {
        w = static_cast<float>(pos + 1) / overlap;
    } else if (pos >= size - overlap) {
        w = static_cast<float>(size - pos) / overlap;
    }
    
    return w * w;
}

} // namespace crossscale
