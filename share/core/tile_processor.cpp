#include "tile_processor.hpp"
#include "engine.hpp"
#include "utils/logger.hpp"
#include <cmath>
#include <algorithm>
#include <future>
#include <optional>
#include <cstring>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

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
    int scale_factor = engine.get_scale_factor();
    
    ProcessingProgress progress;
    progress.total_tiles = static_cast<int>(regions.size());
    progress.completed_tiles = 0;

    if (engine.supports_gpu_tile_pipeline()) {
        TileConfig gpu_config = config;
        if (gpu_config.batch_size < 1) {
            gpu_config.batch_size = 1;
        }
        if (engine.current_device() == DeviceType::GPU_VULKAN && gpu_config.batch_size == 1) {
            gpu_config.batch_size = 2;
        }
        auto result = engine.process_tiles_gpu(
            input_data,
            output_data,
            input_width,
            input_height,
            channels,
            regions,
            gpu_config,
            progress_callback,
            cancel_flag
        );
        if (!result.has_value()) {
            return TileProcessorError{1, "GPU tile processing failed: " + result.error().message};
        }
        return {};
    }
    
    int output_width = input_width * scale_factor;
    int output_height = input_height * scale_factor;
    
    std::vector<float> output_buffer(output_width * output_height * channels, 0.0f);
    
    int effective_batch_size = std::max(1, config.batch_size);
    if (effective_batch_size == 1 && engine.current_device() == DeviceType::GPU_VULKAN) {
        // A small implicit batch helps keep GPU busy when callers keep the default batch_size.
        effective_batch_size = 2;
    }

    struct BatchWork {
        size_t begin = 0;
        size_t end = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::vector<float>> input_buffers;
        std::vector<const float*> batch_inputs;
        std::vector<std::vector<float>> output_buffers;
        std::vector<float*> batch_outputs;
    };

    struct CompletedBatch {
        size_t begin = 0;
        std::vector<std::vector<float>> output_buffers;
    };

    auto build_batch = [&](size_t start_idx) -> std::optional<BatchWork> {
        if (start_idx >= regions.size() || cancel_flag) {
            return std::nullopt;
        }

        BatchWork batch;
        batch.begin = start_idx;
        batch.width = regions[start_idx].width;
        batch.height = regions[start_idx].height;
        batch.end = start_idx + 1;

        while (batch.end < regions.size() &&
               batch.end - start_idx < static_cast<size_t>(effective_batch_size) &&
               regions[batch.end].width == batch.width &&
               regions[batch.end].height == batch.height) {
            ++batch.end;
        }

        size_t batch_count = batch.end - batch.begin;
        batch.input_buffers.reserve(batch_count);
        batch.batch_inputs.reserve(batch_count);
        batch.output_buffers.reserve(batch_count);
        batch.batch_outputs.reserve(batch_count);

        for (size_t j = batch.begin; j < batch.end; ++j) {
            batch.input_buffers.push_back(
                extract_tile(input_data, input_width, input_height, channels, regions[j])
            );
            batch.batch_inputs.push_back(batch.input_buffers.back().data());

            int tile_out_w = static_cast<int>(regions[j].width) * scale_factor;
            int tile_out_h = static_cast<int>(regions[j].height) * scale_factor;
            batch.output_buffers.emplace_back(static_cast<size_t>(tile_out_w) * tile_out_h * channels);
            batch.batch_outputs.push_back(batch.output_buffers.back().data());
        }

        return batch;
    };

    std::deque<CompletedBatch> completed_queue;
    std::mutex completed_mutex;
    std::condition_variable completed_cv;
    bool process_done = false;
    std::string process_error;

    std::thread blend_thread([&]() {
        while (true) {
            CompletedBatch completed_batch;
            {
                std::unique_lock<std::mutex> lock(completed_mutex);
                completed_cv.wait(lock, [&]() {
                    return !completed_queue.empty() || process_done;
                });
                if (completed_queue.empty() && process_done) {
                    break;
                }
                completed_batch = std::move(completed_queue.front());
                completed_queue.pop_front();
            }

            for (size_t idx = 0; idx < completed_batch.output_buffers.size(); ++idx) {
                const auto& region = regions[completed_batch.begin + idx];
                blend_tile(completed_batch.output_buffers[idx].data(), output_buffer.data(),
                           output_width, output_height, channels, region, scale_factor, config.overlap);
                progress.completed_tiles++;
                progress.current_tile_x = region.x;
                progress.current_tile_y = region.y;
                if (progress_callback) progress_callback(progress);
            }
        }
    });

    auto current_batch = build_batch(0);
    while (current_batch.has_value() && !cancel_flag) {
        std::future<std::optional<BatchWork>> next_batch_future;
        if (current_batch->end < regions.size()) {
            size_t next_start = current_batch->end;
            next_batch_future = std::async(std::launch::async, [&, next_start]() {
                return build_batch(next_start);
            });
        }

        auto result = engine.process_batch(
            current_batch->batch_inputs,
            current_batch->batch_outputs,
            static_cast<int>(current_batch->width),
            static_cast<int>(current_batch->height),
            channels,
            cancel_flag
        );
        if (!result.has_value()) {
            process_error = "Batch processing failed: " + result.error().message;
            break;
        }

        CompletedBatch completed_batch;
        completed_batch.begin = current_batch->begin;
        completed_batch.output_buffers = std::move(current_batch->output_buffers);
        {
            std::lock_guard<std::mutex> lock(completed_mutex);
            completed_queue.emplace_back(std::move(completed_batch));
        }
        completed_cv.notify_one();

        if (next_batch_future.valid()) {
            current_batch = next_batch_future.get();
        } else {
            current_batch = std::nullopt;
        }
    }

    {
        std::lock_guard<std::mutex> lock(completed_mutex);
        process_done = true;
    }
    completed_cv.notify_all();
    blend_thread.join();

    if (!process_error.empty()) {
        return TileProcessorError{1, process_error};
    }
    
    if (cancel_flag) {
        return TileProcessorError{2, "Processing cancelled"};
    }
    
    std::copy(output_buffer.begin(), output_buffer.end(), output_data);
    return {};
}

auto TileProcessor::process_image_u8(
    const uint8_t* input_data,
    uint8_t* output_data,
    int input_width,
    int input_height,
    int channels,
    Engine& engine,
    const TileConfig& config,
    std::function<void(const ProcessingProgress&)> progress_callback,
    const std::atomic<bool>& cancel_flag
) -> expected<void, TileProcessorError> {
    if (input_data == nullptr || output_data == nullptr) {
        return TileProcessorError{3, "Null image buffer for u8 processing"};
    }

    auto regions = compute_tile_layout(input_width, input_height, config);
    if (engine.supports_gpu_tile_pipeline_u8()) {
        TileConfig gpu_config = config;
        if (gpu_config.batch_size < 1) {
            gpu_config.batch_size = 1;
        }
        if (engine.current_device() == DeviceType::GPU_VULKAN && gpu_config.batch_size == 1) {
            gpu_config.batch_size = 2;
        }

        auto result = engine.process_tiles_gpu_u8(
            input_data,
            output_data,
            input_width,
            input_height,
            channels,
            regions,
            gpu_config,
            progress_callback,
            cancel_flag
        );
        if (!result.has_value()) {
            return TileProcessorError{1, "GPU u8 tile processing failed: " + result.error().message};
        }
        return {};
    }

    const size_t input_size = static_cast<size_t>(input_width) * input_height * channels;
    std::vector<float> input_float(input_size);
    constexpr float kInv255 = 1.0f / 255.0f;
    for (size_t i = 0; i < input_size; ++i) {
        input_float[i] = static_cast<float>(input_data[i]) * kInv255;
    }

    const int scale = engine.get_scale_factor();
    const size_t output_size = static_cast<size_t>(input_width) * scale * input_height * scale * channels;
    std::vector<float> output_float(output_size, 0.0f);

    auto result = process_image(
        input_float.data(),
        output_float.data(),
        input_width,
        input_height,
        channels,
        engine,
        config,
        progress_callback,
        cancel_flag
    );
    if (!result.has_value()) {
        return result;
    }

    for (size_t i = 0; i < output_size; ++i) {
        const float v = std::clamp(output_float[i] * 255.0f, 0.0f, 255.0f);
        output_data[i] = static_cast<uint8_t>(v);
    }

    return {};
}

auto TileProcessor::extract_tile(const float* image_data, int image_width, int image_height,
                                  int channels, const TileRegion& region) -> std::vector<float> {
    
    std::vector<float> tile(static_cast<size_t>(region.width) * region.height * channels);
    
    for (uint32_t y = 0; y < region.height; ++y) {
        const size_t src_idx = (static_cast<size_t>(region.y + y) * image_width + region.x) * channels;
        const size_t dst_idx = static_cast<size_t>(y) * region.width * channels;
        std::memcpy(
            tile.data() + dst_idx,
            image_data + src_idx,
            static_cast<size_t>(region.width) * channels * sizeof(float)
        );
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
    const bool has_left_neighbor = out_x > 0;
    const bool has_top_neighbor = out_y > 0;
    const bool has_right_neighbor = (out_x + out_w) < static_cast<uint32_t>(output_width);
    const bool has_bottom_neighbor = (out_y + out_h) < static_cast<uint32_t>(output_height);

    if (scaled_overlap <= 0) {
        for (uint32_t y = 0; y < out_h; ++y) {
            size_t dst_idx = (static_cast<size_t>(out_y + y) * output_width + out_x) * channels;
            size_t src_idx = static_cast<size_t>(y) * out_w * channels;
            const size_t row_size = static_cast<size_t>(out_w) * channels;
            for (size_t i = 0; i < row_size; ++i) {
                output_data[dst_idx + i] += tile_data[src_idx + i];
            }
        }
        return;
    }

    std::vector<float> x_weight(out_w);
    std::vector<float> y_weight(out_h);
    const uint32_t overlap_x = std::min<uint32_t>(static_cast<uint32_t>(std::max(0, scaled_overlap)), out_w);
    const uint32_t overlap_y = std::min<uint32_t>(static_cast<uint32_t>(std::max(0, scaled_overlap)), out_h);
    for (uint32_t x = 0; x < out_w; ++x) {
        float w = 1.0f;
        if (overlap_x > 0) {
            if (has_left_neighbor && x < overlap_x) {
                w = static_cast<float>(x + 1) / static_cast<float>(overlap_x);
            } else if (has_right_neighbor && x >= out_w - overlap_x) {
                w = static_cast<float>(out_w - x) / static_cast<float>(overlap_x);
            }
        }
        x_weight[x] = w * w;
    }
    for (uint32_t y = 0; y < out_h; ++y) {
        float w = 1.0f;
        if (overlap_y > 0) {
            if (has_top_neighbor && y < overlap_y) {
                w = static_cast<float>(y + 1) / static_cast<float>(overlap_y);
            } else if (has_bottom_neighbor && y >= out_h - overlap_y) {
                w = static_cast<float>(out_h - y) / static_cast<float>(overlap_y);
            }
        }
        y_weight[y] = w * w;
    }
    
    for (uint32_t y = 0; y < out_h; ++y) {
        const float yw = y_weight[y];
        for (uint32_t x = 0; x < out_w; ++x) {
            float weight = x_weight[x] * yw;
            
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
