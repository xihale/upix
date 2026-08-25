#include "tile_processor.hpp"
#include "engine.hpp"
#include "utils/logger.hpp"
#include <cmath>
#include <algorithm>
#include <format>
#include <future>
#include <optional>
#include <cstring>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stop_token>
#include <mdspan>

namespace crossscale {

auto TileProcessor::compute_tile_layout(int image_width, int image_height,
                                         const TileConfig& config) -> std::vector<TileRegion> {
    std::vector<TileRegion> regions;

    const int tile_size = std::max(1, config.tile_size);
    const int overlap = std::clamp(config.overlap, 0, tile_size - 1);
    const int step = std::max(1, tile_size - overlap);

    std::vector<int> starts_x;
    std::vector<int> starts_y;
    for (int x = 0; x < image_width; x += step) {
        starts_x.push_back(x);
        if (x + tile_size >= image_width) {
            break;
        }
    }
    for (int y = 0; y < image_height; y += step) {
        starts_y.push_back(y);
        if (y + tile_size >= image_height) {
            break;
        }
    }
    if (starts_x.empty()) {
        starts_x.push_back(0);
    }
    if (starts_y.empty()) {
        starts_y.push_back(0);
    }

    regions.reserve(starts_x.size() * starts_y.size());
    for (int y : starts_y) {
        for (int x : starts_x) {
            TileRegion region;
            region.x = static_cast<uint32_t>(x);
            region.y = static_cast<uint32_t>(y);
            region.width = static_cast<uint32_t>(std::min(tile_size, image_width - x));
            region.height = static_cast<uint32_t>(std::min(tile_size, image_height - y));
            region.overlap = static_cast<uint32_t>(overlap);
            regions.push_back(region);
        }
    }

    return regions;
}

namespace {

// Shared plumbing for the CPU tiled pipelines used by process_image (float)
// and process_image_u8. The two entry points differ only in tile extraction
// (float memcpy vs u8→float) and the final normalize/store step; batching,
// engine invocation, blending and error handling are identical.

// One batch of same-sized tiles staged between the load/proc/blend stages.
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

// A processed batch handed from the proc stage to the blend stage.
struct CompletedBatch {
    size_t begin = 0;
    std::vector<std::vector<float>> output_buffers;
};

// Driver for the CPU tile pipeline. ExtractFn extracts one tile as floats:
//   std::vector<float>(int image_width, int channels, const TileRegion&).
// FinalizeFn consumes the accumulated float output after blending:
//   void(std::vector<float>& output_buffer, const std::vector<float>& weight_buffer).
// UseLoadThread selects the scheduling shape, which differs between the two
// entry points and is preserved exactly:
//   true  — three stages: dedicated load thread feeding a bounded ready queue
//           (capacity 2), main-thread engine processing, blend thread
//           (float path).
//   false — two stages: one-ahead batch prefetch via std::async on the main
//           thread, blend thread (u8 path).
template <bool UseLoadThread, typename ExtractFn, typename FinalizeFn>
auto run_tiled_cpu_pipeline(
    TileProcessor& processor,
    Engine& engine,
    const std::vector<TileRegion>& regions,
    const TileConfig& config,
    int input_width,
    int input_height,
    int channels,
    const std::function<void(const ProcessingProgress&)>& progress_callback,
    const std::atomic<bool>& cancel_flag,
    ExtractFn extract_tile_fn,
    FinalizeFn finalize_fn
) -> expected<void, TileProcessorError> {
    const int scale_factor = engine.get_scale_factor();
    const int output_width = input_width * scale_factor;
    const int output_height = input_height * scale_factor;

    // Host RAM preflight for the full-output float accumulator + weight map:
    // refuse with a clean error instead of std::bad_alloc mid-run.
    {
        const auto [total_mb, avail_mb] = engine.get_memory_info();
        if (avail_mb > 0) {
            const size_t host_need = static_cast<size_t>(output_width) * output_height *
                                     (static_cast<size_t>(channels) + 1) * sizeof(float);
            if (host_need > avail_mb * 1024 * 1024) {
                return std::unexpected(TileProcessorError{
                    kErrOutOfMemory,
                    std::format("out of host memory: need ~{}MB, available {}MB",
                                host_need / (1024 * 1024), avail_mb)});
            }
        }
    }

    std::vector<float> output_buffer(static_cast<size_t>(output_width) * output_height * channels, 0.0f);
    std::vector<float> weight_buffer(static_cast<size_t>(output_width) * output_height, 0.0f);

    ProcessingProgress progress;
    progress.total_tiles = static_cast<int>(regions.size());
    progress.completed_tiles = 0;

    int effective_batch_size = std::max(1, config.batch_size);
    if (effective_batch_size == 1 && engine.current_device() == DeviceType::GPU_VULKAN) {
        // A small implicit batch helps keep GPU busy when callers keep the default batch_size.
        effective_batch_size = 2;
    }

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
            batch.input_buffers.push_back(extract_tile_fn(input_width, channels, regions[j]));
            batch.batch_inputs.push_back(batch.input_buffers.back().data());

            int tile_out_w = static_cast<int>(regions[j].width) * scale_factor;
            int tile_out_h = static_cast<int>(regions[j].height) * scale_factor;
            batch.output_buffers.emplace_back(static_cast<size_t>(tile_out_w) * tile_out_h * channels);
            batch.batch_outputs.push_back(batch.output_buffers.back().data());
        }

        return batch;
    };

    // Proc → blend handoff queue.
    // condition_variable_any (not condition_variable) so the stage threads can
    // wait with a std::stop_token and wake automatically on request_stop().
    std::deque<CompletedBatch> completed_queue;
    std::mutex completed_mutex;
    std::condition_variable_any completed_cv;
    bool process_done = false;
    std::string process_error;
    std::string blend_error;

    // --- Blend stage (blend/save thread) ---
    // jthread RAII: if anything below throws on the main thread, the destructor
    // requests stop and joins, instead of leaving an unjoined thread running
    // during exception unwind (previously called std::terminate and killed the
    // process). The lambda also catch-and-records so a throwing stage degrades
    // to a reported error rather than a crash.
    std::jthread blend_thread([&](std::stop_token stoken) {
        try {
            while (true) {
                CompletedBatch completed_batch;
                {
                    std::unique_lock<std::mutex> lock(completed_mutex);
                    completed_cv.wait(lock, stoken, [&]() {
                        return !completed_queue.empty() || process_done;
                    });
                    if (completed_queue.empty()) {
                        // Queue drained and producer done, or stop requested
                        // (cancel / exceptional shutdown): either way, exit.
                        break;
                    }
                    completed_batch = std::move(completed_queue.front());
                    completed_queue.pop_front();
                }

                for (size_t idx = 0; idx < completed_batch.output_buffers.size(); ++idx) {
                    const auto& region = regions[completed_batch.begin + idx];
                    processor.blend_tile(completed_batch.output_buffers[idx].data(), output_buffer.data(),
                                         weight_buffer.data(),
                                         output_width, output_height, channels, region, scale_factor, config.overlap);
                    progress.completed_tiles++;
                    progress.current_tile_x = region.x;
                    progress.current_tile_y = region.y;
                    if (progress_callback) progress_callback(progress);
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(completed_mutex);
            blend_error = e.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(completed_mutex);
            blend_error = "unknown exception in blend thread";
        }
    });

    if constexpr (UseLoadThread) {
        // --- Three-stage pipeline: Load | Proc | Save ---
        // Load thread extracts tiles, Proc (main thread) processes via engine,
        // blend thread blends. Bounded queues connect stages to keep all
        // hardware busy.

        // Bounded queue for Load → Proc pipeline (capacity 2: one processing, one ready)
        constexpr size_t kReadyQueueCapacity = 2;
        std::deque<std::optional<BatchWork>> ready_queue;
        std::mutex ready_mutex;
        std::condition_variable_any ready_cv;
        bool load_done = false;
        std::string load_error;

        // --- Stage 1: Load thread (tile extraction) ---
        // jthread + stop_token-aware wait: an exception on the main thread no
        // longer strands this thread during unwind (jthread RAII stops+joins,
        // and the stop token wakes the bounded-queue wait without notify_all).
        std::jthread load_thread([&](std::stop_token stoken) {
            size_t next_idx = 0;
            try {
                while (!cancel_flag && !stoken.stop_requested()) {
                    auto batch = build_batch(next_idx);
                    if (!batch.has_value()) {
                        break;
                    }
                    next_idx = batch->end;

                    {
                        std::unique_lock<std::mutex> lock(ready_mutex);
                        ready_cv.wait(lock, stoken, [&]() {
                            return ready_queue.size() < kReadyQueueCapacity || cancel_flag;
                        });
                        if (cancel_flag || stoken.stop_requested()) break;
                        ready_queue.emplace_back(std::move(batch));
                    }
                    ready_cv.notify_one();
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(ready_mutex);
                load_error = e.what();
            } catch (...) {
                std::lock_guard<std::mutex> lock(ready_mutex);
                load_error = "unknown exception in tile-load thread";
            }
            {
                std::lock_guard<std::mutex> lock(ready_mutex);
                load_done = true;
            }
            ready_cv.notify_one();
        });

        // --- Stage 2: Proc (main thread) - process batches through engine ---
        while (!cancel_flag) {
            std::optional<BatchWork> current_batch;
            {
                std::unique_lock<std::mutex> lock(ready_mutex);
                ready_cv.wait(lock, [&]() {
                    return !ready_queue.empty() || load_done;
                });
                if (ready_queue.empty() && load_done) {
                    break;
                }
                if (!ready_queue.empty()) {
                    current_batch = std::move(ready_queue.front());
                    ready_queue.pop_front();
                }
            }
            ready_cv.notify_one();  // Signal load thread to fill the queue

            if (!current_batch.has_value()) {
                break;
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
        }

        // Shutdown pipeline
        {
            std::lock_guard<std::mutex> lock(completed_mutex);
            process_done = true;
        }
        completed_cv.notify_all();
        load_thread.join();
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            if (!load_error.empty() && process_error.empty()) {
                process_error = "Tile loading failed: " + load_error;
            }
        }
    } else {
        // --- Two-stage pipeline: one-ahead batch prefetch | blend ---
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
    }

    blend_thread.join();
    {
        std::lock_guard<std::mutex> lock(completed_mutex);
        if (!blend_error.empty() && process_error.empty()) {
            process_error = "Blending failed: " + blend_error;
        }
    }

    if (!process_error.empty()) {
        return std::unexpected(TileProcessorError{1, process_error});
    }

    if (cancel_flag) {
        return std::unexpected(TileProcessorError{2, "Processing cancelled"});
    }

    finalize_fn(output_buffer, weight_buffer);
    return {};
}

} // namespace

auto TileProcessor::process_image(const float* input_data, float* output_data,
                                    int input_width, int input_height, int channels,
                                    Engine& engine, const TileConfig& config,
                                    std::function<void(const ProcessingProgress&)> progress_callback,
                                    const std::atomic<bool>& cancel_flag)
    -> expected<void, TileProcessorError> {

    auto regions = compute_tile_layout(input_width, input_height, config);

    if (engine.supports_gpu_tile_pipeline()) {
        TileConfig gpu_config = config;
        if (gpu_config.batch_size < 1) {
            gpu_config.batch_size = 1;
        }
        if (engine.current_device() == DeviceType::GPU_VULKAN && gpu_config.batch_size == 1) {
            gpu_config.batch_size = 2;
        }
        // OOM ladder: shrink the tile (the per-tile working set scales with
        // tile²); when that stops helping, fall through to the CPU pipeline
        // below, where the full-output accumulator lives in host RAM.
        while (true) {
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
            if (result.has_value()) {
                return {};
            }
            if (!is_oom_error(result.error())) {
                return std::unexpected(TileProcessorError{1, "GPU tile processing failed: " + result.error().message});
            }
            if (gpu_config.tile_size <= 64) {
                LOG_WARNING("TileProcessor",
                    "GPU out of memory at tile_size=64, falling back to CPU blending");
                break;
            }
            gpu_config.tile_size = std::max(64, gpu_config.tile_size / 2);
            gpu_config.overlap = std::min(gpu_config.overlap, gpu_config.tile_size / 4);
            LOG_WARNING("TileProcessor",
                "GPU out of memory, retrying with tile_size={}", gpu_config.tile_size);
            regions = compute_tile_layout(input_width, input_height, gpu_config);
        }
    }

    const int output_width = input_width * engine.get_scale_factor();
    const int output_height = input_height * engine.get_scale_factor();

    // CPU float path: dedicated load thread + bounded ready queue (deep
    // prefetch keeps the engine fed), memcpy tile extraction, float normalize.
    return run_tiled_cpu_pipeline</*UseLoadThread=*/true>(
        *this, engine, regions, config, input_width, input_height, channels,
        progress_callback, cancel_flag,
        [&](int image_width, int ch, const TileRegion& region) {
            return extract_tile(input_data, image_width, ch, region);
        },
        [&](std::vector<float>& output_buffer, const std::vector<float>& weight_buffer) {
            const size_t pixel_count = static_cast<size_t>(output_width) * output_height;
            for (size_t p = 0; p < pixel_count; ++p) {
                const float w = weight_buffer[p];
                if (w <= 1e-6f) {
                    continue;
                }
                const size_t base = p * static_cast<size_t>(channels);
                for (int c = 0; c < channels; ++c) {
                    output_buffer[base + static_cast<size_t>(c)] /= w;
                }
            }

            std::copy(output_buffer.begin(), output_buffer.end(), output_data);
        });
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
    const std::atomic<bool>& cancel_flag,
    bool enable_alpha_bicubic
) -> expected<void, TileProcessorError> {
    if (input_data == nullptr || output_data == nullptr) {
        return std::unexpected(TileProcessorError{3, "Null image buffer for u8 processing"});
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

        // OOM ladder (same as the float path): shrink the tile, then blend on
        // the CPU with only tile-sized GPU work.
        while (true) {
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
            if (result.has_value()) {
                return {};
            }
            if (!is_oom_error(result.error())) {
                return std::unexpected(TileProcessorError{1, "GPU u8 tile processing failed: " + result.error().message});
            }
            if (gpu_config.tile_size <= 64) {
                LOG_WARNING("TileProcessor",
                    "GPU out of memory at tile_size=64, falling back to CPU blending");
                break;
            }
            gpu_config.tile_size = std::max(64, gpu_config.tile_size / 2);
            gpu_config.overlap = std::min(gpu_config.overlap, gpu_config.tile_size / 4);
            LOG_WARNING("TileProcessor",
                "GPU out of memory, retrying with tile_size={}", gpu_config.tile_size);
            regions = compute_tile_layout(input_width, input_height, gpu_config);
        }
    }

    // Per-tile CPU path: extract from u8, convert to float per-tile, process, blend.
    // This avoids the full-image u8→float copy, saving O(W*H*C) float allocation.
    const int scale_factor = engine.get_scale_factor();
    const int output_width = input_width * scale_factor;
    const int output_height = input_height * scale_factor;

    // Alpha channel bicubic bypass for RGBA images (saves 25% GPU compute)
    std::vector<uint8_t> alpha_output;
    const bool has_alpha = (channels == 4);
    if (has_alpha && enable_alpha_bicubic) {
        std::vector<uint8_t> alpha_input(static_cast<size_t>(input_width) * input_height);
        for (size_t i = 0; i < alpha_input.size(); ++i) {
            alpha_input[i] = input_data[i * 4 + 3];
        }
        alpha_output.resize(static_cast<size_t>(output_width) * output_height);
        bicubic_scale_u8(alpha_input.data(), input_width, input_height,
                         alpha_output.data(), output_width, output_height);
        LOG_INFO("TileProcessor", "Alpha channel bypassed neural network (bicubic)");
    }

    // CPU u8 path: one-ahead batch prefetch (per-tile u8→float extraction keeps
    // the memory footprint low), u8 normalize/quantize on the way out.
    return run_tiled_cpu_pipeline</*UseLoadThread=*/false>(
        *this, engine, regions, config, input_width, input_height, channels,
        progress_callback, cancel_flag,
        [&](int image_width, int ch, const TileRegion& region) {
            // Per-tile u8→float extraction (avoids full-image float copy)
            return extract_tile_u8(input_data, image_width, ch, region);
        },
        [&](std::vector<float>& output_float, const std::vector<float>& weight_buffer) {
            // Normalize and convert float output to u8
            const size_t pixel_count = static_cast<size_t>(output_width) * output_height;
            for (size_t p = 0; p < pixel_count; ++p) {
                const float w = weight_buffer[p];
                if (w <= 1e-6f) {
                    continue;
                }
                const size_t base = p * static_cast<size_t>(channels);
                for (int c = 0; c < channels; ++c) {
                    const float v = std::clamp((output_float[base + c] / w) * 255.0f, 0.0f, 255.0f);
                    output_data[base + c] = static_cast<uint8_t>(v);
                }
            }

            // Replace alpha with bicubic-scaled alpha (bypasses neural network for alpha)
            if (has_alpha && enable_alpha_bicubic && !alpha_output.empty()) {
                for (size_t i = 0; i < pixel_count; ++i) {
                    output_data[i * 4 + 3] = alpha_output[i];
                }
                LOG_INFO("TileProcessor", "Merged bicubic alpha into output");
            }
        });
}

auto TileProcessor::extract_tile(const float* image_data, int image_width,
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

// Per-tile extraction from u8 data with u8→float conversion on-the-fly.
// Avoids the full-image u8→float copy, saving O(W*H*C) float allocation.
auto TileProcessor::extract_tile_u8(const uint8_t* image_data, int image_width,
                                     int channels, const TileRegion& region) -> std::vector<float> {
    const size_t tile_pixels = static_cast<size_t>(region.width) * region.height * channels;
    std::vector<float> tile(tile_pixels);
    constexpr float kInv255 = 1.0f / 255.0f;

    for (uint32_t y = 0; y < region.height; ++y) {
        const size_t src_idx = (static_cast<size_t>(region.y + y) * image_width + region.x) * channels;
        const size_t dst_idx = static_cast<size_t>(y) * region.width * channels;
        for (size_t c = 0, cw = static_cast<size_t>(region.width) * channels; c < cw; ++c) {
            tile[dst_idx + c] = static_cast<float>(image_data[src_idx + c]) * kInv255;
        }
    }

    return tile;
}

auto TileProcessor::blend_tile(const float* tile_data, float* output_data,
                                float* weight_data,
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
            for (uint32_t x = 0; x < out_w; ++x) {
                const size_t dst_pixel = static_cast<size_t>(out_y + y) * output_width + out_x + x;
                const size_t src_idx = (static_cast<size_t>(y) * out_w + x) * channels;
                const size_t dst_idx = dst_pixel * static_cast<size_t>(channels);
                for (int c = 0; c < channels; ++c) {
                    output_data[dst_idx + static_cast<size_t>(c)] += tile_data[src_idx + static_cast<size_t>(c)];
                }
                weight_data[dst_pixel] += 1.0f;
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
            const size_t dst_pixel = static_cast<size_t>(out_y + y) * output_width + out_x + x;
            weight_data[dst_pixel] += weight;
        }
    }
}

// Bicubic interpolation kernel (Mitchell-Netravali, B=1/3, C=1/3)
static inline float bicubic_kernel(float t) {
    float a = -0.5f;
    t = std::abs(t);
    if (t <= 1.0f) {
        return (a + 2.0f) * t * t * t - (a + 3.0f) * t * t + 1.0f;
    } else if (t < 2.0f) {
        return a * t * t * t - 5.0f * a * t * t + 8.0f * a * t - 4.0f * a;
    }
    return 0.0f;
}

// Bicubic scale for a single channel (u8 in, u8 out) - class method
void TileProcessor::bicubic_scale_u8(
    const uint8_t* src, uint32_t src_w, uint32_t src_h,
    uint8_t* dst, uint32_t dst_w, uint32_t dst_h)
{
    float scale_x = static_cast<float>(src_w) / dst_w;
    float scale_y = static_cast<float>(src_h) / dst_h;

    // 2D mdspan views over the single-channel planes: [row, col] indexing
    // replaces manual y*width+x arithmetic in the gather/scatter kernels.
    const auto src_plane = std::mdspan(src, static_cast<size_t>(src_h), static_cast<size_t>(src_w));
    auto dst_plane = std::mdspan(dst, static_cast<size_t>(dst_h), static_cast<size_t>(dst_w));

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        float sy = (dy + 0.5f) * scale_y - 0.5f;
        int sy0 = static_cast<int>(std::floor(sy));

        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            float sx = (dx + 0.5f) * scale_x - 0.5f;
            int sx0 = static_cast<int>(std::floor(sx));

            float sum = 0.0f;
            float weight_sum = 0.0f;

            for (int m = -1; m <= 2; ++m) {
                int cy = std::clamp(sy0 + m, 0, static_cast<int>(src_h - 1));
                float wy = bicubic_kernel(sy - (sy0 + m));

                for (int n = -1; n <= 2; ++n) {
                    int cx = std::clamp(sx0 + n, 0, static_cast<int>(src_w - 1));
                    float wx = bicubic_kernel(sx - (sx0 + n));

                    float w = wx * wy;
                    sum += src_plane[static_cast<size_t>(cy), static_cast<size_t>(cx)] * w;
                    weight_sum += w;
                }
            }

            dst_plane[static_cast<size_t>(dy), static_cast<size_t>(dx)] = static_cast<uint8_t>(
                std::clamp(sum / weight_sum, 0.0f, 255.0f));
        }
    }
}

} // namespace crossscale
