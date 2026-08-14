// GPU tile-processing pipeline (3 tiers: u8-queue / float / u8), the SPIRV
// pipeline init helpers, the Vulkan dispatch paths, and the runtime snapshot
// formatter. Split out of ncnn_engine.cpp for readability; the function bodies
// are byte-for-byte identical to the pre-split implementation.

#include "engine.hpp"
#include "ncnn_engine_impl.hpp"
#if NCNN_VULKAN
#include "vulkan_shaders.hpp"
#endif
#include "tile_processor.hpp"
#include "utils/logger.hpp"
#include "utils/benchmark.hpp"
#include <net.h>
#include <cpu.h>
#if NCNN_VULKAN
#include <gpu.h>
#include <command.h>
#include <pipeline.h>
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

namespace crossscale {

    auto NCNNEngineImpl::process_tiles_gpu_u8_queue(
        const std::vector<GPUTileTaskU8>& tasks,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
#if NCNN_VULKAN
        if (tasks.empty()) {
            return {};
        }
        if (!supports_gpu_tile_pipeline_u8_queue()) {
            return Engine::process_tiles_gpu_u8_queue(tasks, cancel_flag);
        }
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        detail::VkAllocGuard alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

        ncnn::Option opt = net_->opt;
        opt.blob_vkallocator = blob_vkallocator;
        opt.workspace_vkallocator = blob_vkallocator;
        opt.staging_vkallocator = staging_vkallocator;

        struct TilePadInfo {
            int in_w = 0;
            int in_h = 0;
            int pad_left = 0;
            int pad_top = 0;
            int pad_right = 0;
            int pad_bottom = 0;
            int tile_x = 0;
            int tile_y = 0;
        };

        struct QueueTaskState {
            const GPUTileTaskU8* task = nullptr;
            size_t task_index = 0;
            int out_width = 0;
            int out_height = 0;
            size_t out_total = 0;
            size_t out_total_pixels = 0;
            ncnn::VkMat out_gpu_hwc;
            ncnn::VkMat out_gpu_weight;
            ncnn::VkMat out_gpu_u8;
            ncnn::VkMat in_gpu_full;
            size_t region_cursor = 0;
            ProcessingProgress progress;
        };

        auto compute_pad = [&](const GPUTileTaskU8& task, const TileRegion& region) -> TilePadInfo {
            TilePadInfo info;
            const int rx = static_cast<int>(region.x);
            const int ry = static_cast<int>(region.y);
            const int rw = static_cast<int>(region.width);
            const int rh = static_cast<int>(region.height);
            const int pad = std::max(0, task.config.overlap);
            const int align = (scale_factor_ == 2 || scale_factor_ == 4) ? 2 : 4;

            info.pad_left = (rx == 0) ? pad : 0;
            info.pad_top = (ry == 0) ? pad : 0;
            info.pad_right = (rx + rw >= task.input_width) ? pad : 0;
            info.pad_bottom = (ry + rh >= task.input_height) ? pad : 0;

            int tile_in_w = rw + info.pad_left + info.pad_right;
            int tile_in_h = rh + info.pad_top + info.pad_bottom;
            if (align > 1) {
                const int aligned_w = (tile_in_w + align - 1) / align * align;
                const int aligned_h = (tile_in_h + align - 1) / align * align;
                info.pad_right += aligned_w - tile_in_w;
                info.pad_bottom += aligned_h - tile_in_h;
                tile_in_w = aligned_w;
                tile_in_h = aligned_h;
            }

            info.in_w = tile_in_w;
            info.in_h = tile_in_h;
            info.tile_x = rx - info.pad_left;
            info.tile_y = ry - info.pad_top;
            return info;
        };

        std::vector<QueueTaskState> states;
        states.reserve(tasks.size());
        std::vector<std::vector<TilePadInfo>> task_pad_infos;
        task_pad_infos.reserve(tasks.size());
        size_t total_tiles = 0;
        for (const auto& task : tasks) {
            if (task.input_data == nullptr || task.output_data == nullptr || task.regions == nullptr) {
                return std::unexpected(EngineError{205, "GPU queue task has null buffers"});
            }
            if (task.channels <= 0 || task.input_width <= 0 || task.input_height <= 0) {
                return std::unexpected(EngineError{202, "Invalid input dimensions"});
            }

            QueueTaskState state;
            state.task = &task;
            state.task_index = states.size();
            state.out_width = task.input_width * scale_factor_;
            state.out_height = task.input_height * scale_factor_;
            state.out_total = static_cast<size_t>(state.out_width) * state.out_height * task.channels;
            state.out_total_pixels = static_cast<size_t>(state.out_width) * state.out_height;
            if (state.out_total > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                state.out_total_pixels > static_cast<size_t>(std::numeric_limits<int>::max())) {
                return std::unexpected(EngineError{203, "Output image too large for Vulkan tile pipeline"});
            }

            state.out_gpu_hwc.create(static_cast<int>(state.out_total), 4u, blob_vkallocator);
            state.out_gpu_weight.create(static_cast<int>(state.out_total_pixels), 4u, blob_vkallocator);
            state.out_gpu_u8.create(static_cast<int>(state.out_total), 1u, blob_vkallocator);
            state.progress.total_tiles = static_cast<int>(task.regions->size());
            state.progress.completed_tiles = 0;
            states.emplace_back(std::move(state));

            total_tiles += task.regions->size();
            std::vector<TilePadInfo> pad_infos;
            pad_infos.reserve(task.regions->size());
            for (const auto& region : *task.regions) {
                pad_infos.emplace_back(compute_pad(task, region));
            }
            task_pad_infos.emplace_back(std::move(pad_infos));
        }
        if (total_tiles == 0) {
            return {};
        }
        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        // submit_budget is pinned to 1: each tile's blend dispatch does read-modify-write
        // accumulation (dst += w*src, weight += w) into SHARED out_gpu_hwc/out_gpu_weight.
        // Packing more than one blend into a single VkCompute command buffer is a RAW/WAR
        // hazard — ncnn's record_pipeline inserts no memory barrier between dispatches that
        // read and write the same buffer, so later tiles lose their writes (dark/black
        // regions). ncnn's VkCompute::barrier_readwrite is protected and cannot be called
        // from here, so one tile per command buffer relies on the implicit queue-submit
        // barrier to serialize the accumulations. See process_tiles_gpu_u8 for the same
        // constraint and the commit history for the empirical confirmation
        // (one-tile-per-cmd -> mean 242 vs corrupted 139).
        constexpr size_t submit_budget = 1;

        int input_idx = -1;
        int output_idx = -1;
        if (!net_->input_indexes().empty()) {
            input_idx = net_->input_indexes()[0];
        }
        if (!net_->output_indexes().empty()) {
            output_idx = net_->output_indexes()[0];
        }

        using TileInputKey = std::tuple<int, int, int>;
        struct TileInputKeyHash {
            auto operator()(const TileInputKey& key) const noexcept -> size_t {
                size_t seed = 0;
                auto mix = [&](int v) {
                    seed ^= std::hash<int>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                };
                mix(std::get<0>(key));
                mix(std::get<1>(key));
                mix(std::get<2>(key));
                return seed;
            }
        };
        std::unordered_map<TileInputKey, std::vector<ncnn::VkMat>, TileInputKeyHash> tile_input_pool;
        tile_input_pool.reserve(32);
        auto acquire_tile_input = [&](int w, int h, int channels)
            -> std::pair<TileInputKey, ncnn::VkMat> {
            TileInputKey key{w, h, channels};
            auto& bucket = tile_input_pool[key];
            if (!bucket.empty()) {
                ncnn::VkMat mat = std::move(bucket.back());
                bucket.pop_back();
                return {key, std::move(mat)};
            }

            ncnn::VkMat mat;
            mat.create(w, h, channels, in_elemsize, 1, blob_vkallocator);
            return {key, std::move(mat)};
        };

        auto recycle_tile_inputs = [&](std::vector<std::pair<TileInputKey, ncnn::VkMat>>& inflight_inputs) {
            for (auto& inflight : inflight_inputs) {
                tile_input_pool[inflight.first].emplace_back(std::move(inflight.second));
            }
            inflight_inputs.clear();
        };

        std::vector<ncnn::VkMat> blend_bindings(3);
        std::vector<ncnn::vk_constant_type> blend_constants(13);
        auto record_tile_outputs = [&](ncnn::VkCompute& cmd,
                                       QueueTaskState& state,
                                       const TileRegion& region,
                                       const TilePadInfo& pad_info,
                                       ncnn::VkMat& in_gpu_chw,
                                       std::vector<ncnn::VkMat>& batch_output_keepalive,
                                       int tile_in_w,
                                       int tile_in_h) -> expected<void, EngineError> {
            const int channels = state.task->channels;
            const int tile_out_full_w = tile_in_w * scale_factor_;
            const int tile_out_full_h = tile_in_h * scale_factor_;
            const int tile_out_inner_w = static_cast<int>(region.width) * scale_factor_;
            const int tile_out_inner_h = static_cast<int>(region.height) * scale_factor_;
            const int src_offset_x = pad_info.pad_left * scale_factor_;
            const int src_offset_y = pad_info.pad_top * scale_factor_;
            if (tile_out_full_w <= 0 || tile_out_full_h <= 0) {
                return std::unexpected(EngineError{206, "Invalid tile dimensions"});
            }

            ncnn::Extractor ex = net_->create_extractor();
            ex.set_light_mode(true);
            ex.set_blob_vkallocator(blob_vkallocator);
            ex.set_workspace_vkallocator(blob_vkallocator);
            ex.set_staging_vkallocator(staging_vkallocator);

            int ret = 0;
            if (input_idx >= 0) {
                ret = ex.input(input_idx, in_gpu_chw);
            } else {
                ret = ex.input("data", in_gpu_chw);
            }
            if (ret != 0) {
                return std::unexpected(EngineError{207, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"});
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return std::unexpected(EngineError{208, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"});
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                return std::unexpected(EngineError{209, "Vulkan output shape mismatch"});
            }

            blend_bindings[0] = out_gpu;
            blend_bindings[1] = state.out_gpu_hwc;
            blend_bindings[2] = state.out_gpu_weight;

            blend_constants[0].i = tile_out_inner_w;
            blend_constants[1].i = tile_out_inner_h;
            blend_constants[2].i = channels;
            blend_constants[3].i = state.out_width;
            blend_constants[4].i = state.out_height;
            blend_constants[5].i = static_cast<int>(region.x) * scale_factor_;
            blend_constants[6].i = static_cast<int>(region.y) * scale_factor_;
            blend_constants[7].i = state.task->config.overlap * scale_factor_;
            blend_constants[8].i = tile_out_full_w;
            blend_constants[9].i = tile_out_full_h;
            blend_constants[10].i = src_offset_x;
            blend_constants[11].i = src_offset_y;
            blend_constants[12].i = static_cast<int>(out_gpu.cstep);

            ncnn::VkMat blend_dispatcher;
            blend_dispatcher.w = tile_out_inner_w;
            blend_dispatcher.h = tile_out_inner_h;
            blend_dispatcher.c = channels;
            cmd.record_pipeline(blend_tile_pipeline_.get(), blend_bindings, blend_constants, blend_dispatcher);

            batch_output_keepalive.emplace_back(std::move(out_gpu));
            return {};
        };

        size_t rr_cursor = 0;
        bool outputs_downloaded = false;
        bool need_init_submit = true;
        std::vector<int> submitted_tiles(states.size(), 0);
        std::vector<ncnn::VkMat> pre_bindings(2);
        std::vector<ncnn::vk_constant_type> pre_constants(8);
        std::vector<ncnn::VkMat> batch_output_keepalive;
        batch_output_keepalive.reserve(submit_budget);
        std::vector<std::pair<TileInputKey, ncnn::VkMat>> inflight_tile_inputs;
        inflight_tile_inputs.reserve(submit_budget);
        std::vector<ncnn::VkMat> normalize_quantize_bindings(3);
        std::vector<ncnn::vk_constant_type> normalize_quantize_constants(2);
        std::vector<ncnn::VkMat> clear_bindings(1);
        std::vector<ncnn::vk_constant_type> clear_constants(1);
        while (true) {
            if (cancel_flag) {
                return std::unexpected(EngineError{3, "Processing cancelled"});
            }

            ncnn::VkCompute cmd(vkdev_);
            bool has_init_work = false;
            if (need_init_submit) {
                has_init_work = true;
                for (auto& state : states) {
                    clear_constants[0].i = static_cast<int>(state.out_total);

                    ncnn::VkMat clear_dispatcher;
                    clear_dispatcher.w = static_cast<int>(state.out_total);
                    clear_dispatcher.h = 1;
                    clear_dispatcher.c = 1;

                    clear_bindings[0] = state.out_gpu_hwc;
                    cmd.record_pipeline(clear_output_pipeline_.get(), clear_bindings,
                                        clear_constants, clear_dispatcher);

                    clear_constants[0].i = static_cast<int>(state.out_total_pixels);
                    clear_dispatcher.w = static_cast<int>(state.out_total_pixels);
                    clear_bindings[0] = state.out_gpu_weight;
                    cmd.record_pipeline(clear_output_pipeline_.get(), clear_bindings,
                                        clear_constants, clear_dispatcher);

                    const size_t in_total = static_cast<size_t>(state.task->input_width) *
                                            state.task->input_height * state.task->channels;
                    if (in_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
                        return std::unexpected(EngineError{205, "Input image too large for Vulkan tile pipeline"});
                    }
                    ncnn::Mat in_cpu_full(static_cast<int>(in_total),
                                          const_cast<uint8_t*>(state.task->input_data), 1u);
                    cmd.record_clone(in_cpu_full, state.in_gpu_full, opt);
                }
            }
            batch_output_keepalive.clear();
            inflight_tile_inputs.clear();
            std::fill(submitted_tiles.begin(), submitted_tiles.end(), 0);
            size_t scheduled_tiles = 0;
            size_t idle_rounds = 0;

            while (scheduled_tiles < submit_budget && idle_rounds < states.size()) {
                QueueTaskState& state = states[rr_cursor];
                const auto& regions = *state.task->regions;
                bool did_work = false;
                int local_budget = std::max(1, state.task->config.batch_size);
                while (local_budget > 0 &&
                       state.region_cursor < regions.size() &&
                       scheduled_tiles < submit_budget) {
                    const auto& region = regions[state.region_cursor];
                    const auto& pad_info = task_pad_infos[state.task_index][state.region_cursor];
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * state.task->channels;
                    if (tile_in_total == 0) {
                        return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                    }

                    auto pooled_input = acquire_tile_input(tile_in_w, tile_in_h, state.task->channels);
                    ncnn::VkMat in_gpu_chw = std::move(pooled_input.second);

                    pre_bindings[0] = state.in_gpu_full;
                    pre_bindings[1] = in_gpu_chw;

                    pre_constants[0].i = state.task->input_width;
                    pre_constants[1].i = state.task->input_height;
                    pre_constants[2].i = state.task->channels;
                    pre_constants[3].i = pad_info.tile_x;
                    pre_constants[4].i = pad_info.tile_y;
                    pre_constants[5].i = tile_in_w;
                    pre_constants[6].i = tile_in_h;
                    pre_constants[7].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = tile_in_w;
                    pre_dispatcher.h = tile_in_h;
                    pre_dispatcher.c = state.task->channels;
                    cmd.record_pipeline(preproc_hwc_crop_u8_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

                    auto record_result = record_tile_outputs(cmd, state, region, pad_info, in_gpu_chw,
                                                             batch_output_keepalive, tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                    inflight_tile_inputs.emplace_back(std::move(pooled_input.first), std::move(in_gpu_chw));

                    state.region_cursor++;
                    submitted_tiles[rr_cursor]++;
                    scheduled_tiles++;
                    local_budget--;
                    did_work = true;
                    state.progress.current_tile_x = region.x;
                    state.progress.current_tile_y = region.y;
                }

                rr_cursor = (rr_cursor + 1) % states.size();
                idle_rounds = did_work ? 0 : (idle_rounds + 1);
            }

            if (scheduled_tiles == 0 && !has_init_work) {
                break;
            }

            bool all_tiles_scheduled = true;
            for (const auto& state : states) {
                if (state.region_cursor < state.task->regions->size()) {
                    all_tiles_scheduled = false;
                    break;
                }
            }

            if (all_tiles_scheduled) {
                for (auto& state : states) {
                    normalize_quantize_bindings[0] = state.out_gpu_hwc;
                    normalize_quantize_bindings[1] = state.out_gpu_weight;
                    normalize_quantize_bindings[2] = state.out_gpu_u8;

                    normalize_quantize_constants[0].i = static_cast<int>(state.out_total_pixels);
                    normalize_quantize_constants[1].i = state.task->channels;

                    ncnn::VkMat normalize_quantize_dispatcher;
                    normalize_quantize_dispatcher.w = static_cast<int>(state.out_total_pixels);
                    normalize_quantize_dispatcher.h = 1;
                    normalize_quantize_dispatcher.c = state.task->channels;
                    cmd.record_pipeline(normalize_quantize_output_pipeline_.get(),
                                        normalize_quantize_bindings,
                                        normalize_quantize_constants,
                                        normalize_quantize_dispatcher);

                    ncnn::Mat out_cpu(static_cast<int>(state.out_total), state.task->output_data, 1u);
                    cmd.record_clone(state.out_gpu_u8, out_cpu, opt);
                }
            }

            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                if (all_tiles_scheduled) {
                    return std::unexpected(EngineError{214, "Vulkan queue normalize+quantize/download failed (error " +
                                            std::to_string(submit_ret) + ")"});
                }
                if (has_init_work) {
                    return std::unexpected(EngineError{206, "Vulkan queue init failed (error " + std::to_string(submit_ret) + ")"});
                }
                return std::unexpected(EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"});
            }
            recycle_tile_inputs(inflight_tile_inputs);
            need_init_submit = false;

            for (size_t i = 0; i < states.size(); ++i) {
                const int completed = submitted_tiles[i];
                if (completed <= 0) {
                    continue;
                }

                auto& state = states[i];
                state.progress.completed_tiles += completed;
                if (state.progress.completed_tiles > state.progress.total_tiles) {
                    state.progress.completed_tiles = state.progress.total_tiles;
                }
                if (state.task->progress_callback) {
                    state.task->progress_callback(state.progress);
                }
            }

            if (all_tiles_scheduled) {
                outputs_downloaded = true;
                break;
            }
        }

        if (!outputs_downloaded) {
            ncnn::VkCompute cmd(vkdev_);
            for (auto& state : states) {
                normalize_quantize_bindings[0] = state.out_gpu_hwc;
                normalize_quantize_bindings[1] = state.out_gpu_weight;
                normalize_quantize_bindings[2] = state.out_gpu_u8;

                normalize_quantize_constants[0].i = static_cast<int>(state.out_total_pixels);
                normalize_quantize_constants[1].i = state.task->channels;

                ncnn::VkMat normalize_quantize_dispatcher;
                normalize_quantize_dispatcher.w = static_cast<int>(state.out_total_pixels);
                normalize_quantize_dispatcher.h = 1;
                normalize_quantize_dispatcher.c = state.task->channels;
                cmd.record_pipeline(normalize_quantize_output_pipeline_.get(),
                                    normalize_quantize_bindings,
                                    normalize_quantize_constants,
                                    normalize_quantize_dispatcher);

                ncnn::Mat out_cpu(static_cast<int>(state.out_total), state.task->output_data, 1u);
                cmd.record_clone(state.out_gpu_u8, out_cpu, opt);
            }

            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{214, "Vulkan queue normalize+quantize/download failed (error " +
                                        std::to_string(submit_ret) + ")"});
            }
        }

        return {};
#else
        (void)tasks;
        (void)cancel_flag;
        return std::unexpected(EngineError{200, "Vulkan not available"});
#endif
    }

    auto NCNNEngineImpl::process_tiles_gpu(
        const float* input_data,
        float* output_data,
        int input_width,
        int input_height,
        int channels,
        const std::vector<TileRegion>& regions,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
#if NCNN_VULKAN
        if (!supports_gpu_tile_pipeline()) {
            return std::unexpected(EngineError{201, "GPU tile pipeline not available"});
        }
        if (channels <= 0 || input_width <= 0 || input_height <= 0) {
            return std::unexpected(EngineError{202, "Invalid input dimensions"});
        }
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        const int out_width = input_width * scale_factor_;
        const int out_height = input_height * scale_factor_;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        const size_t out_total_pixels = static_cast<size_t>(out_width) * out_height;
        if (out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(EngineError{203, "Output image too large for Vulkan tile pipeline"});
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        detail::VkAllocGuard alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

        ncnn::Option opt = net_->opt;
        opt.blob_vkallocator = blob_vkallocator;
        opt.workspace_vkallocator = blob_vkallocator;
        opt.staging_vkallocator = staging_vkallocator;

        ncnn::VkMat out_gpu_hwc;
        out_gpu_hwc.create(static_cast<int>(out_total), 4u, blob_vkallocator);
        ncnn::VkMat out_gpu_weight;
        out_gpu_weight.create(static_cast<int>(out_total_pixels), 4u, blob_vkallocator);

        {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::vk_constant_type> constants(1);
            ncnn::VkMat dispatcher{};
            constants[0].i = static_cast<int>(out_total);
            dispatcher.w = static_cast<int>(out_total);
            dispatcher.h = 1;
            dispatcher.c = 1;
            std::vector<ncnn::VkMat> output_bindings(1);
            output_bindings[0] = out_gpu_hwc;
            cmd.record_pipeline(clear_output_pipeline_.get(), output_bindings, constants, dispatcher);

            constants[0].i = static_cast<int>(out_total_pixels);
            dispatcher.w = static_cast<int>(out_total_pixels);
            std::vector<ncnn::VkMat> weight_bindings(1);
            weight_bindings[0] = out_gpu_weight;
            cmd.record_pipeline(clear_output_pipeline_.get(), weight_bindings, constants, dispatcher);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{204, "Vulkan clear failed (error " + std::to_string(submit_ret) + ")"});
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
        // See the long comment in process_tiles_gpu_u8: blend accumulation is a read-modify-write
        // on shared buffers, so multiple blend dispatches per VkCompute are a RAW/WAR hazard.
        // Pin per-cmd tiles to 1 to serialize via the queue-submit barrier.
        const size_t tiles_per_submit = 1;
        (void)tiles_per_cmd;
        const size_t in_total_full = static_cast<size_t>(input_width) * input_height * channels;
        if (in_total_full > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(EngineError{205, "Input image too large for Vulkan tile pipeline"});
        }
        if (normalize_quantize_output_pipeline_ == nullptr) {
            return std::unexpected(EngineError{212, "Vulkan normalize+quantize pipeline not available"});
        }

        struct TilePadInfo {
            int in_w = 0;
            int in_h = 0;
            int pad_left = 0;
            int pad_top = 0;
            int pad_right = 0;
            int pad_bottom = 0;
            int tile_x = 0;
            int tile_y = 0;
        };

        const int pad = std::max(0, config.overlap);
        const int align = (scale_factor_ == 2 || scale_factor_ == 4) ? 2 : 4;
        auto compute_pad = [&](const TileRegion& region) -> TilePadInfo {
            TilePadInfo info;
            const int rx = static_cast<int>(region.x);
            const int ry = static_cast<int>(region.y);
            const int rw = static_cast<int>(region.width);
            const int rh = static_cast<int>(region.height);

            info.pad_left = (rx == 0) ? pad : 0;
            info.pad_top = (ry == 0) ? pad : 0;
            info.pad_right = (rx + rw >= input_width) ? pad : 0;
            info.pad_bottom = (ry + rh >= input_height) ? pad : 0;

            int tile_in_w = rw + info.pad_left + info.pad_right;
            int tile_in_h = rh + info.pad_top + info.pad_bottom;

            if (align > 1) {
                int aligned_w = (tile_in_w + align - 1) / align * align;
                int aligned_h = (tile_in_h + align - 1) / align * align;
                info.pad_right += aligned_w - tile_in_w;
                info.pad_bottom += aligned_h - tile_in_h;
                tile_in_w = aligned_w;
                tile_in_h = aligned_h;
            }

            info.in_w = tile_in_w;
            info.in_h = tile_in_h;
            info.tile_x = rx - info.pad_left;
            info.tile_y = ry - info.pad_top;
            return info;
        };

        bool use_gpu_crop = gpu_crop_pipeline_ready_;
        ncnn::VkMat in_gpu_full;
        if (use_gpu_crop) {
            ncnn::VkCompute cmd(vkdev_);
            ncnn::Mat in_cpu_full(static_cast<int>(in_total_full), const_cast<float*>(input_data), 4u);
            cmd.record_clone(in_cpu_full, in_gpu_full, opt);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{210, "Vulkan input upload failed (error " + std::to_string(submit_ret) + ")"});
            }
        }

        int input_idx = -1;
        int output_idx = -1;
        if (!net_->input_indexes().empty()) {
            input_idx = net_->input_indexes()[0];
        }
        if (!net_->output_indexes().empty()) {
            output_idx = net_->output_indexes()[0];
        }

        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        auto record_tile_outputs = [&](ncnn::VkCompute& cmd,
                                       const TileRegion& region,
                                       const TilePadInfo& pad_info,
                                       ncnn::VkMat& in_gpu_chw,
                                       std::vector<ncnn::VkMat>& batch_keepalive,
                                       int tile_in_w,
                                       int tile_in_h) -> expected<void, EngineError> {
            const int tile_out_full_w = tile_in_w * scale_factor_;
            const int tile_out_full_h = tile_in_h * scale_factor_;
            const int tile_out_inner_w = static_cast<int>(region.width) * scale_factor_;
            const int tile_out_inner_h = static_cast<int>(region.height) * scale_factor_;
            const int src_offset_x = pad_info.pad_left * scale_factor_;
            const int src_offset_y = pad_info.pad_top * scale_factor_;
            if (tile_out_full_w <= 0 || tile_out_full_h <= 0) {
                return std::unexpected(EngineError{206, "Invalid tile dimensions"});
            }

            ncnn::Extractor ex = net_->create_extractor();
            ex.set_light_mode(true);
            ex.set_blob_vkallocator(blob_vkallocator);
            ex.set_workspace_vkallocator(blob_vkallocator);
            ex.set_staging_vkallocator(staging_vkallocator);

            int ret = 0;
            if (input_idx >= 0) {
                ret = ex.input(input_idx, in_gpu_chw);
            } else {
                ret = ex.input("data", in_gpu_chw);
            }
            if (ret != 0) {
                return std::unexpected(EngineError{207, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"});
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return std::unexpected(EngineError{208, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"});
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                return std::unexpected(EngineError{209, "Vulkan output shape mismatch"});
            }

            std::vector<ncnn::VkMat> blend_bindings(3);
            blend_bindings[0] = out_gpu;
            blend_bindings[1] = out_gpu_hwc;
            blend_bindings[2] = out_gpu_weight;

            std::vector<ncnn::vk_constant_type> blend_constants(13);
            blend_constants[0].i = tile_out_inner_w;
            blend_constants[1].i = tile_out_inner_h;
            blend_constants[2].i = channels;
            blend_constants[3].i = out_width;
            blend_constants[4].i = out_height;
            blend_constants[5].i = static_cast<int>(region.x) * scale_factor_;
            blend_constants[6].i = static_cast<int>(region.y) * scale_factor_;
            blend_constants[7].i = config.overlap * scale_factor_;
            blend_constants[8].i = tile_out_full_w;
            blend_constants[9].i = tile_out_full_h;
            blend_constants[10].i = src_offset_x;
            blend_constants[11].i = src_offset_y;
            blend_constants[12].i = static_cast<int>(out_gpu.cstep);

            ncnn::VkMat blend_dispatcher;
            blend_dispatcher.w = tile_out_inner_w;
            blend_dispatcher.h = tile_out_inner_h;
            blend_dispatcher.c = channels;
            cmd.record_pipeline(blend_tile_pipeline_.get(), blend_bindings, blend_constants, blend_dispatcher);
            batch_keepalive.emplace_back(in_gpu_chw);
            batch_keepalive.emplace_back(std::move(out_gpu));

            return {};
        };
        if (use_gpu_crop && preproc_hwc_crop_to_chw_ == nullptr) {
            return std::unexpected(EngineError{211, "GPU crop pipeline unavailable"});
        }

        if (use_gpu_crop) {
            size_t region_idx = 0;
            while (region_idx < regions.size()) {
                if (cancel_flag) {
                    return std::unexpected(EngineError{3, "Processing cancelled"});
                }

                size_t batch_end = std::min(regions.size(), region_idx + tiles_per_submit);
                ncnn::VkCompute cmd(vkdev_);
                std::vector<ncnn::VkMat> batch_keepalive;
                batch_keepalive.reserve((batch_end - region_idx) * 2);
                for (size_t i = region_idx; i < batch_end; ++i) {
                    const auto& region = regions[i];
                    const TilePadInfo pad_info = compute_pad(region);
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                    if (tile_in_total == 0) {
                        return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                    }

                    ncnn::VkMat in_gpu_chw;
                    in_gpu_chw.create(tile_in_w, tile_in_h, channels, in_elemsize, 1, blob_vkallocator);

                    std::vector<ncnn::VkMat> pre_bindings(2);
                    pre_bindings[0] = in_gpu_full;
                    pre_bindings[1] = in_gpu_chw;

                    std::vector<ncnn::vk_constant_type> pre_constants(8);
                    pre_constants[0].i = input_width;
                    pre_constants[1].i = input_height;
                    pre_constants[2].i = channels;
                    pre_constants[3].i = pad_info.tile_x;
                    pre_constants[4].i = pad_info.tile_y;
                    pre_constants[5].i = tile_in_w;
                    pre_constants[6].i = tile_in_h;
                    pre_constants[7].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = tile_in_w;
                    pre_dispatcher.h = tile_in_h;
                    pre_dispatcher.c = channels;
                    cmd.record_pipeline(preproc_hwc_crop_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                             tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    return std::unexpected(EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"});
                }

                for (size_t i = region_idx; i < batch_end; ++i) {
                    const auto& region = regions[i];
                    progress.completed_tiles++;
                    progress.current_tile_x = region.x;
                    progress.current_tile_y = region.y;
                    if (progress_callback) {
                        progress_callback(progress);
                    }
                }

                region_idx = batch_end;
            }
        } else {
            struct BatchWork {
                size_t begin = 0;
                size_t end = 0;
                std::vector<std::vector<float>> input_buffers;
            };

            auto build_batch = [&](size_t start_idx) -> std::optional<BatchWork> {
                if (start_idx >= regions.size() || cancel_flag) {
                    return std::nullopt;
                }

                BatchWork batch;
                batch.begin = start_idx;
                batch.end = std::min(regions.size(), start_idx + tiles_per_cmd);
                batch.input_buffers.reserve(batch.end - batch.begin);

                for (size_t i = batch.begin; i < batch.end; ++i) {
                    const auto& region = regions[i];
                    const TilePadInfo pad_info = compute_pad(region);
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                    if (tile_in_total == 0) {
                        return std::nullopt;
                    }

                    std::vector<float> tile_input(tile_in_total);
                    for (int y = 0; y < tile_in_h; ++y) {
                        int src_y = pad_info.tile_y + y;
                        src_y = std::abs(src_y);
                        src_y = (input_height - 1) - std::abs(src_y - (input_height - 1));
                        for (int x = 0; x < tile_in_w; ++x) {
                            int src_x = pad_info.tile_x + x;
                            src_x = std::abs(src_x);
                            src_x = (input_width - 1) - std::abs(src_x - (input_width - 1));
                            const size_t src_idx = (static_cast<size_t>(src_y) * input_width + src_x) * channels;
                            const size_t dst_idx = (static_cast<size_t>(y) * tile_in_w + x) * channels;
                            std::memcpy(
                                tile_input.data() + dst_idx,
                                input_data + src_idx,
                                static_cast<size_t>(channels) * sizeof(float)
                            );
                        }
                    }
                    batch.input_buffers.emplace_back(std::move(tile_input));
                }

                return batch;
            };

            auto current_batch = build_batch(0);
            while (current_batch.has_value()) {
                std::future<std::optional<BatchWork>> next_batch_future;
                if (current_batch->end < regions.size()) {
                    size_t next_start = current_batch->end;
                    next_batch_future = std::async(std::launch::async, [&, next_start]() {
                        return build_batch(next_start);
                    });
                }

                ncnn::VkCompute cmd(vkdev_);
                std::vector<ncnn::VkMat> batch_keepalive;
                batch_keepalive.reserve(current_batch->input_buffers.size() * 3);
                for (size_t idx = 0; idx < current_batch->input_buffers.size(); ++idx) {
                    const size_t region_idx = current_batch->begin + idx;
                    const auto& region = regions[region_idx];
                    const TilePadInfo pad_info = compute_pad(region);
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                    if (tile_in_total == 0) {
                        return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                    }

                    ncnn::Mat in_cpu(static_cast<int>(tile_in_total),
                                     current_batch->input_buffers[idx].data(),
                                     4u);
                    ncnn::VkMat in_gpu_hwc;
                    cmd.record_clone(in_cpu, in_gpu_hwc, opt);

                    ncnn::VkMat in_gpu_chw;
                    in_gpu_chw.create(tile_in_w, tile_in_h, channels, in_elemsize, 1, blob_vkallocator);

                    std::vector<ncnn::VkMat> pre_bindings(2);
                    pre_bindings[0] = in_gpu_hwc;
                    pre_bindings[1] = in_gpu_chw;

                    std::vector<ncnn::vk_constant_type> pre_constants(4);
                    pre_constants[0].i = tile_in_w;
                    pre_constants[1].i = tile_in_h;
                    pre_constants[2].i = channels;
                    pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = tile_in_w;
                    pre_dispatcher.h = tile_in_h;
                    pre_dispatcher.c = channels;
                    cmd.record_pipeline(preproc_hwc_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);
                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                             tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                    batch_keepalive.emplace_back(in_gpu_hwc);
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    return std::unexpected(EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"});
                }

                for (size_t i = current_batch->begin; i < current_batch->end; ++i) {
                    const auto& region = regions[i];
                    progress.completed_tiles++;
                    progress.current_tile_x = region.x;
                    progress.current_tile_y = region.y;
                    if (progress_callback) {
                        progress_callback(progress);
                    }
                }

                if (next_batch_future.valid()) {
                    current_batch = next_batch_future.get();
                } else {
                    current_batch = std::nullopt;
                }
            }
        }

        {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::VkMat> normalize_bindings(2);
            normalize_bindings[0] = out_gpu_hwc;
            normalize_bindings[1] = out_gpu_weight;

            std::vector<ncnn::vk_constant_type> normalize_constants(2);
            normalize_constants[0].i = static_cast<int>(out_total_pixels);
            normalize_constants[1].i = channels;

            ncnn::VkMat normalize_dispatcher;
            normalize_dispatcher.w = static_cast<int>(out_total_pixels);
            normalize_dispatcher.h = 1;
            normalize_dispatcher.c = channels;
            cmd.record_pipeline(normalize_output_pipeline_.get(), normalize_bindings, normalize_constants,
                                normalize_dispatcher);

            ncnn::Mat out_cpu(static_cast<int>(out_total), output_data, 4u);
            cmd.record_clone(out_gpu_hwc, out_cpu, opt);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{214, "Vulkan normalize/download failed (error " + std::to_string(submit_ret) + ")"});
            }
        }

        return {};
#else
        (void)input_data;
        (void)output_data;
        (void)input_width;
        (void)input_height;
        (void)channels;
        (void)regions;
        (void)config;
        (void)progress_callback;
        (void)cancel_flag;
        return std::unexpected(EngineError{200, "Vulkan not available"});
#endif
    }

    auto NCNNEngineImpl::process_tiles_gpu_u8(
        const uint8_t* input_data,
        uint8_t* output_data,
        int input_width,
        int input_height,
        int channels,
        const std::vector<TileRegion>& regions,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
#if NCNN_VULKAN
        if (!supports_gpu_tile_pipeline_u8()) {
            return std::unexpected(EngineError{201, "GPU u8 tile pipeline not available"});
        }
        if (channels <= 0 || input_width <= 0 || input_height <= 0) {
            return std::unexpected(EngineError{202, "Invalid input dimensions"});
        }
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        const int out_width = input_width * scale_factor_;
        const int out_height = input_height * scale_factor_;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        const size_t out_total_pixels = static_cast<size_t>(out_width) * out_height;
        if (out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(EngineError{203, "Output image too large for Vulkan tile pipeline"});
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        detail::VkAllocGuard alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

        ncnn::Option opt = net_->opt;
        opt.blob_vkallocator = blob_vkallocator;
        opt.workspace_vkallocator = blob_vkallocator;
        opt.staging_vkallocator = staging_vkallocator;

        ncnn::VkMat out_gpu_hwc;
        out_gpu_hwc.create(static_cast<int>(out_total), 4u, blob_vkallocator);
        ncnn::VkMat out_gpu_weight;
        out_gpu_weight.create(static_cast<int>(out_total_pixels), 4u, blob_vkallocator);

        {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::vk_constant_type> constants(1);
            ncnn::VkMat dispatcher{};
            constants[0].i = static_cast<int>(out_total);
            dispatcher.w = static_cast<int>(out_total);
            dispatcher.h = 1;
            dispatcher.c = 1;

            std::vector<ncnn::VkMat> output_bindings(1);
            output_bindings[0] = out_gpu_hwc;
            cmd.record_pipeline(clear_output_pipeline_.get(), output_bindings, constants, dispatcher);

            constants[0].i = static_cast<int>(out_total_pixels);
            dispatcher.w = static_cast<int>(out_total_pixels);
            std::vector<ncnn::VkMat> weight_bindings(1);
            weight_bindings[0] = out_gpu_weight;
            cmd.record_pipeline(clear_output_pipeline_.get(), weight_bindings, constants, dispatcher);

            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{204, "Vulkan clear failed (error " + std::to_string(submit_ret) + ")"});
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
        // Each tile's blend dispatch performs read-modify-write accumulation (dst += src*w,
        // weight += w) into the SHARED out_gpu_hwc / out_gpu_weight buffers. Recording more
        // than one such dispatch in a single VkCompute command buffer is a RAW/WAR hazard:
        // ncnn's record_pipeline inserts no memory barrier between dispatches that read and
        // write the same buffer, so later tiles read stale/partial data and their writes are
        // lost — producing dark/black regions (confirmed for scale>=3 overlapping tiles).
        // ncnn's VkCompute::barrier_readwrite is protected, so we cannot insert barriers from
        // here. The correct, robust fix is to submit each tile in its OWN VkCompute so the
        // implicit queue-submit barrier serializes the accumulations. batch_size therefore
        // controls inference batching intent only; the per-cmd tile count is pinned to 1.
        const size_t tiles_per_submit = 1;
        (void)tiles_per_cmd;
        const size_t in_total_full = static_cast<size_t>(input_width) * input_height * channels;
        if (in_total_full > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(EngineError{205, "Input image too large for Vulkan tile pipeline"});
        }

        struct TilePadInfo {
            int in_w = 0;
            int in_h = 0;
            int pad_left = 0;
            int pad_top = 0;
            int pad_right = 0;
            int pad_bottom = 0;
            int tile_x = 0;
            int tile_y = 0;
        };

        const int pad = std::max(0, config.overlap);
        const int align = (scale_factor_ == 2 || scale_factor_ == 4) ? 2 : 4;
        auto compute_pad = [&](const TileRegion& region) -> TilePadInfo {
            TilePadInfo info;
            const int rx = static_cast<int>(region.x);
            const int ry = static_cast<int>(region.y);
            const int rw = static_cast<int>(region.width);
            const int rh = static_cast<int>(region.height);

            info.pad_left = (rx == 0) ? pad : 0;
            info.pad_top = (ry == 0) ? pad : 0;
            info.pad_right = (rx + rw >= input_width) ? pad : 0;
            info.pad_bottom = (ry + rh >= input_height) ? pad : 0;

            int tile_in_w = rw + info.pad_left + info.pad_right;
            int tile_in_h = rh + info.pad_top + info.pad_bottom;

            if (align > 1) {
                int aligned_w = (tile_in_w + align - 1) / align * align;
                int aligned_h = (tile_in_h + align - 1) / align * align;
                info.pad_right += aligned_w - tile_in_w;
                info.pad_bottom += aligned_h - tile_in_h;
                tile_in_w = aligned_w;
                tile_in_h = aligned_h;
            }

            info.in_w = tile_in_w;
            info.in_h = tile_in_h;
            info.tile_x = rx - info.pad_left;
            info.tile_y = ry - info.pad_top;
            return info;
        };

        bool use_gpu_crop = gpu_crop_u8_pipeline_ready_;
        ncnn::VkMat in_gpu_full;
        if (use_gpu_crop) {
            ncnn::VkCompute cmd(vkdev_);
            ncnn::Mat in_cpu_full(static_cast<int>(in_total_full), const_cast<uint8_t*>(input_data), 1u);
            cmd.record_clone(in_cpu_full, in_gpu_full, opt);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{212, "Vulkan u8 input upload failed (error " + std::to_string(submit_ret) + ")"});
            }
        }

        int input_idx = -1;
        int output_idx = -1;
        if (!net_->input_indexes().empty()) {
            input_idx = net_->input_indexes()[0];
        }
        if (!net_->output_indexes().empty()) {
            output_idx = net_->output_indexes()[0];
        }

        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        auto record_tile_outputs = [&](ncnn::VkCompute& cmd,
                                       const TileRegion& region,
                                       const TilePadInfo& pad_info,
                                       ncnn::VkMat& in_gpu_chw,
                                       std::vector<ncnn::VkMat>& batch_keepalive,
                                       int tile_in_w,
                                       int tile_in_h) -> expected<void, EngineError> {
            const int tile_out_full_w = tile_in_w * scale_factor_;
            const int tile_out_full_h = tile_in_h * scale_factor_;
            const int tile_out_inner_w = static_cast<int>(region.width) * scale_factor_;
            const int tile_out_inner_h = static_cast<int>(region.height) * scale_factor_;
            const int src_offset_x = pad_info.pad_left * scale_factor_;
            const int src_offset_y = pad_info.pad_top * scale_factor_;
            if (tile_out_full_w <= 0 || tile_out_full_h <= 0) {
                return std::unexpected(EngineError{206, "Invalid tile dimensions"});
            }

            ncnn::Extractor ex = net_->create_extractor();
            ex.set_light_mode(true);
            ex.set_blob_vkallocator(blob_vkallocator);
            ex.set_workspace_vkallocator(blob_vkallocator);
            ex.set_staging_vkallocator(staging_vkallocator);

            int ret = 0;
            if (input_idx >= 0) {
                ret = ex.input(input_idx, in_gpu_chw);
            } else {
                ret = ex.input("data", in_gpu_chw);
            }
            if (ret != 0) {
                return std::unexpected(EngineError{207, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"});
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return std::unexpected(EngineError{208, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"});
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                return std::unexpected(EngineError{209, "Vulkan output shape mismatch"});
            }

            std::vector<ncnn::VkMat> blend_bindings(3);
            blend_bindings[0] = out_gpu;
            blend_bindings[1] = out_gpu_hwc;
            blend_bindings[2] = out_gpu_weight;

            std::vector<ncnn::vk_constant_type> blend_constants(13);
            blend_constants[0].i = tile_out_inner_w;
            blend_constants[1].i = tile_out_inner_h;
            blend_constants[2].i = channels;
            blend_constants[3].i = out_width;
            blend_constants[4].i = out_height;
            blend_constants[5].i = static_cast<int>(region.x) * scale_factor_;
            blend_constants[6].i = static_cast<int>(region.y) * scale_factor_;
            blend_constants[7].i = config.overlap * scale_factor_;
            blend_constants[8].i = tile_out_full_w;
            blend_constants[9].i = tile_out_full_h;
            blend_constants[10].i = src_offset_x;
            blend_constants[11].i = src_offset_y;
            blend_constants[12].i = static_cast<int>(out_gpu.cstep);

            ncnn::VkMat blend_dispatcher;
            blend_dispatcher.w = tile_out_inner_w;
            blend_dispatcher.h = tile_out_inner_h;
            blend_dispatcher.c = channels;
            cmd.record_pipeline(blend_tile_pipeline_.get(), blend_bindings, blend_constants, blend_dispatcher);
            batch_keepalive.emplace_back(in_gpu_chw);
            batch_keepalive.emplace_back(std::move(out_gpu));

            return {};
        };

        auto record_normalize_quantize_download = [&](ncnn::VkCompute& cmd, ncnn::VkMat& out_gpu_u8) {
            std::vector<ncnn::VkMat> normalize_quantize_bindings(3);
            normalize_quantize_bindings[0] = out_gpu_hwc;
            normalize_quantize_bindings[1] = out_gpu_weight;
            normalize_quantize_bindings[2] = out_gpu_u8;

            std::vector<ncnn::vk_constant_type> normalize_quantize_constants(2);
            normalize_quantize_constants[0].i = static_cast<int>(out_total_pixels);
            normalize_quantize_constants[1].i = channels;

            ncnn::VkMat normalize_quantize_dispatcher;
            normalize_quantize_dispatcher.w = static_cast<int>(out_total_pixels);
            normalize_quantize_dispatcher.h = 1;
            normalize_quantize_dispatcher.c = channels;
            cmd.record_pipeline(normalize_quantize_output_pipeline_.get(), normalize_quantize_bindings,
                                normalize_quantize_constants, normalize_quantize_dispatcher);

            ncnn::Mat out_cpu(static_cast<int>(out_total), output_data, 1u);
            cmd.record_clone(out_gpu_u8, out_cpu, opt);
        };

        bool output_downloaded = false;

        if (use_gpu_crop && preproc_hwc_crop_u8_to_chw_ == nullptr) {
            return std::unexpected(EngineError{213, "GPU u8 crop pipeline unavailable"});
        }

        if (use_gpu_crop) {
            LOG_INFO("Engine", "Using GPU tile pipeline with crop u8 preprocessing");
            
            size_t region_idx = 0;
            while (region_idx < regions.size()) {
                if (cancel_flag) {
                    return std::unexpected(EngineError{3, "Processing cancelled"});
                }

                size_t batch_end = std::min(regions.size(), region_idx + tiles_per_submit);
                ncnn::VkCompute cmd(vkdev_);
                std::vector<ncnn::VkMat> batch_keepalive;
                batch_keepalive.reserve((batch_end - region_idx) * 2);
                for (size_t i = region_idx; i < batch_end; ++i) {
                    const auto& region = regions[i];
                    const TilePadInfo pad_info = compute_pad(region);
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                    if (tile_in_total == 0) {
                        return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                    }

                    ncnn::VkMat in_gpu_chw;
                    in_gpu_chw.create(tile_in_w, tile_in_h, channels, in_elemsize, 1, blob_vkallocator);

                    std::vector<ncnn::VkMat> pre_bindings(2);
                    pre_bindings[0] = in_gpu_full;
                    pre_bindings[1] = in_gpu_chw;

                    std::vector<ncnn::vk_constant_type> pre_constants(8);
                    pre_constants[0].i = input_width;
                    pre_constants[1].i = input_height;
                    pre_constants[2].i = channels;
                    pre_constants[3].i = pad_info.tile_x;
                    pre_constants[4].i = pad_info.tile_y;
                    pre_constants[5].i = tile_in_w;
                    pre_constants[6].i = tile_in_h;
                    pre_constants[7].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = tile_in_w;
                    pre_dispatcher.h = tile_in_h;
                    pre_dispatcher.c = channels;
                    cmd.record_pipeline(preproc_hwc_crop_u8_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                             tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                }

                ncnn::VkMat out_gpu_u8;
                const bool is_last_batch = (batch_end == regions.size());
                if (is_last_batch) {
                    out_gpu_u8.create(static_cast<int>(out_total), 1u, blob_vkallocator);
                    record_normalize_quantize_download(cmd, out_gpu_u8);
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    if (is_last_batch) {
                        return std::unexpected(EngineError{214, "Vulkan normalize+quantize/download failed (error " +
                                                std::to_string(submit_ret) + ")"});
                    }
                    return std::unexpected(EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"});
                }
                if (is_last_batch) {
                    output_downloaded = true;
                }

                for (size_t i = region_idx; i < batch_end; ++i) {
                    const auto& region = regions[i];
                    progress.completed_tiles++;
                    progress.current_tile_x = region.x;
                    progress.current_tile_y = region.y;
                    if (progress_callback) {
                        progress_callback(progress);
                    }
                }

                region_idx = batch_end;
            }
        } else {
            struct BatchWork {
                size_t begin = 0;
                size_t end = 0;
                std::vector<std::vector<uint8_t>> input_buffers;
            };

            auto build_batch = [&](size_t start_idx) -> std::optional<BatchWork> {
                if (start_idx >= regions.size() || cancel_flag) {
                    return std::nullopt;
                }

                BatchWork batch;
                batch.begin = start_idx;
                batch.end = std::min(regions.size(), start_idx + tiles_per_cmd);
                batch.input_buffers.reserve(batch.end - batch.begin);

                for (size_t i = batch.begin; i < batch.end; ++i) {
                    const auto& region = regions[i];
                    const TilePadInfo pad_info = compute_pad(region);
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                    if (tile_in_total == 0) {
                        return std::nullopt;
                    }

                    std::vector<uint8_t> tile_input(tile_in_total);
                    for (int y = 0; y < tile_in_h; ++y) {
                        int src_y = pad_info.tile_y + y;
                        src_y = std::abs(src_y);
                        src_y = (input_height - 1) - std::abs(src_y - (input_height - 1));
                        for (int x = 0; x < tile_in_w; ++x) {
                            int src_x = pad_info.tile_x + x;
                            src_x = std::abs(src_x);
                            src_x = (input_width - 1) - std::abs(src_x - (input_width - 1));
                            const size_t src_idx = (static_cast<size_t>(src_y) * input_width + src_x) * channels;
                            const size_t dst_idx = (static_cast<size_t>(y) * tile_in_w + x) * channels;
                            std::memcpy(
                                tile_input.data() + dst_idx,
                                input_data + src_idx,
                                static_cast<size_t>(channels) * sizeof(uint8_t)
                            );
                        }
                    }
                    batch.input_buffers.emplace_back(std::move(tile_input));
                }

                return batch;
            };

            auto current_batch = build_batch(0);
            while (current_batch.has_value()) {
                std::future<std::optional<BatchWork>> next_batch_future;
                if (current_batch->end < regions.size()) {
                    size_t next_start = current_batch->end;
                    next_batch_future = std::async(std::launch::async, [&, next_start]() {
                        return build_batch(next_start);
                    });
                }

                ncnn::VkCompute cmd(vkdev_);
                std::vector<ncnn::VkMat> batch_keepalive;
                batch_keepalive.reserve(current_batch->input_buffers.size() * 3);
                for (size_t idx = 0; idx < current_batch->input_buffers.size(); ++idx) {
                    const size_t region_idx = current_batch->begin + idx;
                    const auto& region = regions[region_idx];
                    const TilePadInfo pad_info = compute_pad(region);
                    const int tile_in_w = pad_info.in_w;
                    const int tile_in_h = pad_info.in_h;
                    const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                    if (tile_in_total == 0) {
                        return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                    }

                    ncnn::Mat in_cpu(static_cast<int>(tile_in_total),
                                     current_batch->input_buffers[idx].data(),
                                     1u);
                    ncnn::VkMat in_gpu_hwc;
                    cmd.record_clone(in_cpu, in_gpu_hwc, opt);

                    ncnn::VkMat in_gpu_chw;
                    in_gpu_chw.create(tile_in_w, tile_in_h, channels, in_elemsize, 1, blob_vkallocator);

                    std::vector<ncnn::VkMat> pre_bindings(2);
                    pre_bindings[0] = in_gpu_hwc;
                    pre_bindings[1] = in_gpu_chw;

                    std::vector<ncnn::vk_constant_type> pre_constants(4);
                    pre_constants[0].i = tile_in_w;
                    pre_constants[1].i = tile_in_h;
                    pre_constants[2].i = channels;
                    pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = tile_in_w;
                    pre_dispatcher.h = tile_in_h;
                    pre_dispatcher.c = channels;
                    cmd.record_pipeline(preproc_hwc_u8_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);
                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                             tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                    batch_keepalive.emplace_back(in_gpu_hwc);
                }

                ncnn::VkMat out_gpu_u8;
                const bool is_last_batch = (current_batch->end == regions.size());
                if (is_last_batch) {
                    out_gpu_u8.create(static_cast<int>(out_total), 1u, blob_vkallocator);
                    record_normalize_quantize_download(cmd, out_gpu_u8);
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    if (is_last_batch) {
                        return std::unexpected(EngineError{214, "Vulkan normalize+quantize/download failed (error " +
                                                std::to_string(submit_ret) + ")"});
                    }
                    return std::unexpected(EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"});
                }
                if (is_last_batch) {
                    output_downloaded = true;
                }

                for (size_t i = current_batch->begin; i < current_batch->end; ++i) {
                    const auto& region = regions[i];
                    progress.completed_tiles++;
                    progress.current_tile_x = region.x;
                    progress.current_tile_y = region.y;
                    if (progress_callback) {
                        progress_callback(progress);
                    }
                }

                if (next_batch_future.valid()) {
                    current_batch = next_batch_future.get();
                } else {
                    current_batch = std::nullopt;
                }
            }
        }

        if (!output_downloaded) {
            ncnn::VkCompute cmd(vkdev_);
            ncnn::VkMat out_gpu_u8;
            out_gpu_u8.create(static_cast<int>(out_total), 1u, blob_vkallocator);
            record_normalize_quantize_download(cmd, out_gpu_u8);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{214, "Vulkan normalize+quantize/download failed (error " + std::to_string(submit_ret) + ")"});
            }
        }

        return {};
#else
        (void)input_data;
        (void)output_data;
        (void)input_width;
        (void)input_height;
        (void)channels;
        (void)regions;
        (void)config;
        (void)progress_callback;
        (void)cancel_flag;
        return std::unexpected(EngineError{200, "Vulkan not available"});
#endif
    }

    auto NCNNEngineImpl::format_vulkan_runtime_snapshot(
        std::string_view stage,
        int width,
        int height,
        int channels,
        size_t submit_budget,
        size_t submit_budget_cap,
        size_t submit_memory_budget_mb,
        double avg_tile_pixels
    ) const -> std::string {
        std::ostringstream stream;
        stream << "stage=" << stage
               << ", initialized=" << (initialized_ ? 1 : 0)
               << ", model_loaded=" << (model_loaded_ ? 1 : 0)
               << ", gpu_layout_ready=" << (gpu_layout_pipeline_ready_ ? 1 : 0)
               << ", gpu_u8_ready=" << (gpu_layout_u8_pipeline_ready_ ? 1 : 0)
               << ", gpu_crop_ready=" << (gpu_crop_pipeline_ready_ ? 1 : 0)
               << ", gpu_crop_u8_ready=" << (gpu_crop_u8_pipeline_ready_ ? 1 : 0)
               << ", gpu_tile_ready=" << (gpu_tile_pipeline_ready_ ? 1 : 0)
               << ", pre_u8=" << (preproc_hwc_u8_to_chw_ != nullptr ? 1 : 0)
               << ", post_u8=" << (postproc_chw_to_hwc_u8_ != nullptr ? 1 : 0)
               << ", quant_u8=" << (quantize_hwc_to_u8_ != nullptr ? 1 : 0)
               << ", fp16_packed=" << (net_->opt.use_fp16_packed ? 1 : 0)
               << ", fp16_storage=" << (net_->opt.use_fp16_storage ? 1 : 0)
               << ", fp16_arith=" << (net_->opt.use_fp16_arithmetic ? 1 : 0)
               << ", int8_storage=" << (net_->opt.use_int8_storage ? 1 : 0);

        if (vkdev_ != nullptr) {
            stream << ", heap_budget_mb=" << vkdev_->get_heap_budget();
        } else {
            stream << ", heap_budget_mb=-1";
        }

        if (width > 0 && height > 0 && channels > 0) {
            stream << ", input=" << width << "x" << height << "x" << channels
                   << ", scale=" << scale_factor_
                   << ", output=" << (width * scale_factor_) << "x" << (height * scale_factor_);
        }
        if (submit_budget > 0) {
            stream << ", submit_budget=" << submit_budget;
        }
        if (submit_budget_cap > 0) {
            stream << ", submit_budget_cap=" << submit_budget_cap;
        }
        if (submit_memory_budget_mb > 0) {
            stream << ", submit_mem_budget_mb=" << submit_memory_budget_mb;
        }
        if (avg_tile_pixels >= 0.0) {
            stream << ", avg_tile_pixels=" << avg_tile_pixels;
        }

        return stream.str();
    }

    bool NCNNEngineImpl::init_gpu_layout_pipelines() {
        if (vkdev_ == nullptr) {
            return false;
        }

        static std::vector<uint32_t> spirv_hwc_to_chw;
        static std::vector<uint32_t> spirv_hwc_u8_to_chw;
        static std::vector<uint32_t> spirv_chw_to_hwc;
        static std::vector<uint32_t> spirv_chw_to_hwc_u8;
        static std::vector<uint32_t> spirv_hwc_to_u8;
        static std::mutex spirv_mutex;

        bool u8_shader_ready = true;
        {
            std::lock_guard<std::mutex> lock(spirv_mutex);
            if (spirv_hwc_to_chw.empty()) {
                if (ncnn::compile_spirv_module(detail::kHwcToChwComp, net_->opt, spirv_hwc_to_chw) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC->CHW shader");
                    return false;
                }
            }
            if (spirv_chw_to_hwc.empty()) {
                if (ncnn::compile_spirv_module(detail::kChwToHwcComp, net_->opt, spirv_chw_to_hwc) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan CHW->HWC shader");
                    return false;
                }
            }
            if (spirv_hwc_u8_to_chw.empty()) {
                if (ncnn::compile_spirv_module(detail::kHwcU8ToChwComp, net_->opt, spirv_hwc_u8_to_chw) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC-u8->CHW shader");
                    u8_shader_ready = false;
                }
            }
            if (spirv_chw_to_hwc_u8.empty()) {
                if (ncnn::compile_spirv_module(detail::kChwToHwcU8Comp, net_->opt, spirv_chw_to_hwc_u8) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan CHW->HWC-u8 shader");
                    u8_shader_ready = false;
                }
            }
            if (spirv_hwc_to_u8.empty()) {
                if (ncnn::compile_spirv_module(detail::kHwcFloatToU8Comp, net_->opt, spirv_hwc_to_u8) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC-float->u8 shader");
                    u8_shader_ready = false;
                }
            }
        }

        preproc_hwc_to_chw_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        preproc_hwc_to_chw_->set_optimal_local_size_xyz(8, 8, 1);
        if (preproc_hwc_to_chw_->create(spirv_hwc_to_chw.data(), spirv_hwc_to_chw.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan HWC->CHW pipeline");
            preproc_hwc_to_chw_.reset();
            return false;
        }

        postproc_chw_to_hwc_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        postproc_chw_to_hwc_->set_optimal_local_size_xyz(8, 8, 1);
        if (postproc_chw_to_hwc_->create(spirv_chw_to_hwc.data(), spirv_chw_to_hwc.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan CHW->HWC pipeline");
            postproc_chw_to_hwc_.reset();
            preproc_hwc_to_chw_.reset();
            return false;
        }

        preproc_hwc_u8_to_chw_.reset();
        postproc_chw_to_hwc_u8_.reset();
        quantize_hwc_to_u8_.reset();
        if (u8_shader_ready) {
            preproc_hwc_u8_to_chw_ = std::make_unique<ncnn::Pipeline>(vkdev_);
            preproc_hwc_u8_to_chw_->set_optimal_local_size_xyz(8, 8, 1);
            if (preproc_hwc_u8_to_chw_->create(spirv_hwc_u8_to_chw.data(), spirv_hwc_u8_to_chw.size() * 4, {}) != 0) {
                LOG_WARNING("Engine", "Failed to create Vulkan HWC-u8->CHW pipeline");
                preproc_hwc_u8_to_chw_.reset();
                u8_shader_ready = false;
            }

            if (u8_shader_ready) {
                postproc_chw_to_hwc_u8_ = std::make_unique<ncnn::Pipeline>(vkdev_);
                postproc_chw_to_hwc_u8_->set_optimal_local_size_xyz(8, 8, 1);
                if (postproc_chw_to_hwc_u8_->create(spirv_chw_to_hwc_u8.data(), spirv_chw_to_hwc_u8.size() * 4, {}) != 0) {
                    LOG_WARNING("Engine", "Failed to create Vulkan CHW->HWC-u8 pipeline");
                    postproc_chw_to_hwc_u8_.reset();
                    u8_shader_ready = false;
                }
            }

            if (u8_shader_ready) {
                quantize_hwc_to_u8_ = std::make_unique<ncnn::Pipeline>(vkdev_);
                quantize_hwc_to_u8_->set_optimal_local_size_xyz(256, 1, 1);
                if (quantize_hwc_to_u8_->create(spirv_hwc_to_u8.data(), spirv_hwc_to_u8.size() * 4, {}) != 0) {
                    LOG_WARNING("Engine", "Failed to create Vulkan HWC-float->u8 pipeline");
                    quantize_hwc_to_u8_.reset();
                    u8_shader_ready = false;
                }
            }
        }

        if (!u8_shader_ready) {
            preproc_hwc_u8_to_chw_.reset();
            postproc_chw_to_hwc_u8_.reset();
            quantize_hwc_to_u8_.reset();
        }

        return true;
    }

    bool NCNNEngineImpl::init_gpu_crop_pipeline() {
        if (vkdev_ == nullptr) {
            return false;
        }

        static std::vector<uint32_t> spirv_crop;
        static std::vector<uint32_t> spirv_crop_u8;
        static std::mutex spirv_mutex;

        bool crop_u8_ready = true;
        {
            std::lock_guard<std::mutex> lock(spirv_mutex);
            if (spirv_crop.empty()) {
                if (ncnn::compile_spirv_module(detail::kHwcCropToChwComp, net_->opt, spirv_crop) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC crop shader");
                    return false;
                }
            }
            if (spirv_crop_u8.empty()) {
                if (ncnn::compile_spirv_module(detail::kHwcCropU8ToChwComp, net_->opt, spirv_crop_u8) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC-u8 crop shader");
                    crop_u8_ready = false;
                }
            }
        }

        preproc_hwc_crop_to_chw_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        preproc_hwc_crop_to_chw_->set_optimal_local_size_xyz(8, 8, 1);
        if (preproc_hwc_crop_to_chw_->create(spirv_crop.data(), spirv_crop.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan HWC crop pipeline");
            preproc_hwc_crop_to_chw_.reset();
            return false;
        }

        preproc_hwc_crop_u8_to_chw_.reset();
        if (crop_u8_ready) {
            preproc_hwc_crop_u8_to_chw_ = std::make_unique<ncnn::Pipeline>(vkdev_);
            preproc_hwc_crop_u8_to_chw_->set_optimal_local_size_xyz(8, 8, 1);
            if (preproc_hwc_crop_u8_to_chw_->create(spirv_crop_u8.data(), spirv_crop_u8.size() * 4, {}) != 0) {
                LOG_WARNING("Engine", "Failed to create Vulkan HWC-u8 crop pipeline");
                preproc_hwc_crop_u8_to_chw_.reset();
            }
        }

        return true;
    }

    bool NCNNEngineImpl::init_gpu_tile_pipelines() {
        if (vkdev_ == nullptr) {
            return false;
        }

        static std::vector<uint32_t> spirv_clear;
        static std::vector<uint32_t> spirv_blend;
        static std::vector<uint32_t> spirv_normalize;
        static std::vector<uint32_t> spirv_normalize_quantize;
        static std::mutex spirv_mutex;

        {
            std::lock_guard<std::mutex> lock(spirv_mutex);
            if (spirv_clear.empty()) {
                if (ncnn::compile_spirv_module(detail::kClearComp, net_->opt, spirv_clear) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan clear shader");
                    return false;
                }
            }
            if (spirv_blend.empty()) {
                if (ncnn::compile_spirv_module(detail::kBlendTileComp, net_->opt, spirv_blend) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan blend shader");
                    return false;
                }
            }
            if (spirv_normalize.empty()) {
                if (ncnn::compile_spirv_module(detail::kNormalizeByWeightComp, net_->opt, spirv_normalize) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan normalize shader");
                    return false;
                }
            }
            if (spirv_normalize_quantize.empty()) {
                if (ncnn::compile_spirv_module(detail::kNormalizeQuantizeByWeightComp, net_->opt, spirv_normalize_quantize) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan normalize+quantize shader");
                    return false;
                }
            }
        }

        clear_output_pipeline_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        clear_output_pipeline_->set_optimal_local_size_xyz(256, 1, 1);
        if (clear_output_pipeline_->create(spirv_clear.data(), spirv_clear.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan clear pipeline");
            clear_output_pipeline_.reset();
            return false;
        }

        blend_tile_pipeline_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        blend_tile_pipeline_->set_optimal_local_size_xyz(8, 8, 1);
        if (blend_tile_pipeline_->create(spirv_blend.data(), spirv_blend.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan tile blend pipeline");
            blend_tile_pipeline_.reset();
            clear_output_pipeline_.reset();
            return false;
        }

        normalize_output_pipeline_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        normalize_output_pipeline_->set_optimal_local_size_xyz(256, 1, 1);
        if (normalize_output_pipeline_->create(spirv_normalize.data(), spirv_normalize.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan normalize pipeline");
            normalize_output_pipeline_.reset();
            blend_tile_pipeline_.reset();
            clear_output_pipeline_.reset();
            return false;
        }

        normalize_quantize_output_pipeline_ = std::make_unique<ncnn::Pipeline>(vkdev_);
        normalize_quantize_output_pipeline_->set_optimal_local_size_xyz(256, 1, 1);
        if (normalize_quantize_output_pipeline_->create(spirv_normalize_quantize.data(), spirv_normalize_quantize.size() * 4, {}) != 0) {
            LOG_WARNING("Engine", "Failed to create Vulkan normalize+quantize pipeline");
            normalize_quantize_output_pipeline_.reset();
            normalize_output_pipeline_.reset();
            blend_tile_pipeline_.reset();
            clear_output_pipeline_.reset();
            return false;
        }

        return true;
    }

    auto NCNNEngineImpl::process_vulkan_with_gpu_layout(
        const float* input,
        float* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
        if (vkdev_ == nullptr || preproc_hwc_to_chw_ == nullptr || postproc_chw_to_hwc_ == nullptr) {
            return std::unexpected(EngineError{10, "Vulkan layout pipeline not initialized"});
        }
        if (channels <= 0 || width <= 0 || height <= 0) {
            return std::unexpected(EngineError{11, "Invalid input dimensions"});
        }
        if (channels != 1 && channels != 3 && channels != 4) {
            return std::unexpected(EngineError{12, "Unsupported channel count for Vulkan layout path"});
        }
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;
        const size_t in_total = static_cast<size_t>(width) * height * channels;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        if (in_total > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(EngineError{13, "Image too large for Vulkan layout path"});
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        detail::VkAllocGuard alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

        ncnn::Option opt = net_->opt;
        opt.blob_vkallocator = blob_vkallocator;
        opt.workspace_vkallocator = blob_vkallocator;
        opt.staging_vkallocator = staging_vkallocator;

        ncnn::VkCompute cmd(vkdev_);

        ncnn::Mat in_cpu(static_cast<int>(in_total), const_cast<float*>(input), 4u);
        ncnn::VkMat in_gpu_hwc;
        cmd.record_clone(in_cpu, in_gpu_hwc, opt);

        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        ncnn::VkMat in_gpu_chw;
        in_gpu_chw.create(width, height, channels, in_elemsize, 1, blob_vkallocator);

        std::vector<ncnn::VkMat> pre_bindings(2);
        pre_bindings[0] = in_gpu_hwc;
        pre_bindings[1] = in_gpu_chw;

        std::vector<ncnn::vk_constant_type> pre_constants(4);
        pre_constants[0].i = width;
        pre_constants[1].i = height;
        pre_constants[2].i = channels;
        pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

        ncnn::VkMat pre_dispatcher;
        pre_dispatcher.w = width;
        pre_dispatcher.h = height;
        pre_dispatcher.c = channels;
        cmd.record_pipeline(preproc_hwc_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::Extractor ex = net_->create_extractor();
        ex.set_light_mode(true);
        ex.set_blob_vkallocator(blob_vkallocator);
        ex.set_workspace_vkallocator(blob_vkallocator);
        ex.set_staging_vkallocator(staging_vkallocator);

        int input_idx = -1;
        int output_idx = -1;
        if (!net_->input_indexes().empty()) {
            input_idx = net_->input_indexes()[0];
        }
        if (!net_->output_indexes().empty()) {
            output_idx = net_->output_indexes()[0];
        }

        int ret = 0;
        if (input_idx >= 0) {
            ret = ex.input(input_idx, in_gpu_chw);
        } else {
            ret = ex.input("data", in_gpu_chw);
        }
        if (ret != 0) {
            return std::unexpected(EngineError{14, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"});
        }

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::VkMat out_gpu;
        if (output_idx >= 0) {
            ret = ex.extract(output_idx, out_gpu, cmd);
        } else {
            ret = ex.extract("output", out_gpu, cmd);
            if (ret != 0) {
                ret = ex.extract("out", out_gpu, cmd);
                if (ret != 0) {
                    return std::unexpected(EngineError{15, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"});
                }
            }
        }

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        if (out_gpu.elempack != 1) {
            ncnn::VkMat out_unpacked;
            vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
            out_gpu = out_unpacked;
        }

        if (out_gpu.w != out_width || out_gpu.h != out_height || out_gpu.c != channels) {
            return std::unexpected(EngineError{16, "Vulkan output shape mismatch"});
        }

        ncnn::VkMat out_gpu_hwc;
        out_gpu_hwc.create(static_cast<int>(out_total), 4u, blob_vkallocator);

        std::vector<ncnn::VkMat> post_bindings(2);
        post_bindings[0] = out_gpu;
        post_bindings[1] = out_gpu_hwc;

        std::vector<ncnn::vk_constant_type> post_constants(4);
        post_constants[0].i = out_width;
        post_constants[1].i = out_height;
        post_constants[2].i = channels;
        post_constants[3].i = static_cast<int>(out_gpu.cstep);

        ncnn::VkMat post_dispatcher;
        post_dispatcher.w = out_width;
        post_dispatcher.h = out_height;
        post_dispatcher.c = channels;
        cmd.record_pipeline(postproc_chw_to_hwc_.get(), post_bindings, post_constants, post_dispatcher);

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::Mat out_cpu(static_cast<int>(out_total), output, 4u);
        cmd.record_clone(out_gpu_hwc, out_cpu, opt);

        ret = cmd.submit_and_wait();
        if (ret != 0) {
            return std::unexpected(EngineError{17, "Vulkan submit failed (error " + std::to_string(ret) + ")"});
        }

        return {};
    }

    auto NCNNEngineImpl::process_vulkan_with_gpu_layout_u8(
        const uint8_t* input,
        uint8_t* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
        if (vkdev_ == nullptr || preproc_hwc_u8_to_chw_ == nullptr || postproc_chw_to_hwc_u8_ == nullptr) {
            const std::string detail = format_vulkan_runtime_snapshot("u8_layout_not_initialized", width, height, channels);
            LOG_ERROR("Engine", "Vulkan u8 layout pipeline not initialized: " + detail);
            return std::unexpected(EngineError{19, "Vulkan u8 layout pipeline not initialized; " + detail});        }
        if (channels <= 0 || width <= 0 || height <= 0) {
            return std::unexpected(EngineError{11, "Invalid input dimensions"});
        }
        if (channels != 1 && channels != 3 && channels != 4) {
            return std::unexpected(EngineError{12, "Unsupported channel count for Vulkan layout path"});
        }
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;
        const size_t in_total = static_cast<size_t>(width) * height * channels;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        if (in_total > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(EngineError{13, "Image too large for Vulkan layout path"});
        }
        LOG_INFO("Engine", "Starting Vulkan u8 layout path: " + format_vulkan_runtime_snapshot("u8_layout_begin", width, height, channels));

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        detail::VkAllocGuard alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

        ncnn::Option opt = net_->opt;
        opt.blob_vkallocator = blob_vkallocator;
        opt.workspace_vkallocator = blob_vkallocator;
        opt.staging_vkallocator = staging_vkallocator;

        ncnn::VkCompute cmd(vkdev_);

        ncnn::Mat in_cpu(static_cast<int>(in_total), const_cast<uint8_t*>(input), 1u);
        ncnn::VkMat in_gpu_hwc;
        cmd.record_clone(in_cpu, in_gpu_hwc, opt);

        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        ncnn::VkMat in_gpu_chw;
        in_gpu_chw.create(width, height, channels, in_elemsize, 1, blob_vkallocator);
        if (in_gpu_chw.empty()) {
            const std::string detail = format_vulkan_runtime_snapshot("u8_in_gpu_chw_alloc_failed", width, height, channels);
            LOG_ERROR("Engine", "Failed to allocate Vulkan input CHW buffer: " + detail);
            return std::unexpected(EngineError{210, "Failed to allocate Vulkan input CHW buffer; " + detail});        }

        std::vector<ncnn::VkMat> pre_bindings(2);
        pre_bindings[0] = in_gpu_hwc;
        pre_bindings[1] = in_gpu_chw;

        std::vector<ncnn::vk_constant_type> pre_constants(4);
        pre_constants[0].i = width;
        pre_constants[1].i = height;
        pre_constants[2].i = channels;
        pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

        ncnn::VkMat pre_dispatcher;
        pre_dispatcher.w = width;
        pre_dispatcher.h = height;
        pre_dispatcher.c = channels;
        cmd.record_pipeline(preproc_hwc_u8_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::Extractor ex = net_->create_extractor();
        ex.set_light_mode(true);
        ex.set_blob_vkallocator(blob_vkallocator);
        ex.set_workspace_vkallocator(blob_vkallocator);
        ex.set_staging_vkallocator(staging_vkallocator);

        int input_idx = -1;
        int output_idx = -1;
        if (!net_->input_indexes().empty()) {
            input_idx = net_->input_indexes()[0];
        }
        if (!net_->output_indexes().empty()) {
            output_idx = net_->output_indexes()[0];
        }

        int ret = 0;
        if (input_idx >= 0) {
            ret = ex.input(input_idx, in_gpu_chw);
        } else {
            ret = ex.input("data", in_gpu_chw);
        }
        if (ret != 0) {
            return std::unexpected(EngineError{14, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"});
        }

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::VkMat out_gpu;
        if (output_idx >= 0) {
            ret = ex.extract(output_idx, out_gpu, cmd);
        } else {
            ret = ex.extract("output", out_gpu, cmd);
            if (ret != 0) {
                ret = ex.extract("out", out_gpu, cmd);
                if (ret != 0) {
                    return std::unexpected(EngineError{15, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"});
                }
            }
        }

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        if (out_gpu.elempack != 1) {
            ncnn::VkMat out_unpacked;
            vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
            out_gpu = out_unpacked;
        }

        if (out_gpu.w != out_width || out_gpu.h != out_height || out_gpu.c != channels) {
            return std::unexpected(EngineError{16, "Vulkan output shape mismatch"});
        }

        ncnn::VkMat out_gpu_hwc_u8;
        out_gpu_hwc_u8.create(static_cast<int>(out_total), 1u, blob_vkallocator);
        if (out_gpu_hwc_u8.empty()) {
            const std::string detail = format_vulkan_runtime_snapshot("u8_out_gpu_hwc_alloc_failed", width, height, channels);
            LOG_ERROR("Engine", "Failed to allocate Vulkan output u8 buffer: " + detail);
            return std::unexpected(EngineError{211, "Failed to allocate Vulkan output u8 buffer; " + detail});        }

        std::vector<ncnn::VkMat> post_bindings(2);
        post_bindings[0] = out_gpu;
        post_bindings[1] = out_gpu_hwc_u8;

        std::vector<ncnn::vk_constant_type> post_constants(4);
        post_constants[0].i = out_width;
        post_constants[1].i = out_height;
        post_constants[2].i = channels;
        post_constants[3].i = static_cast<int>(out_gpu.cstep);

        ncnn::VkMat post_dispatcher;
        post_dispatcher.w = out_width;
        post_dispatcher.h = out_height;
        post_dispatcher.c = channels;
        cmd.record_pipeline(postproc_chw_to_hwc_u8_.get(), post_bindings, post_constants, post_dispatcher);

        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

        ncnn::Mat out_cpu(static_cast<int>(out_total), output, 1u);
        cmd.record_clone(out_gpu_hwc_u8, out_cpu, opt);

        ret = cmd.submit_and_wait();
        if (ret != 0) {
            const std::string detail = format_vulkan_runtime_snapshot("u8_submit_failed", width, height, channels);
            LOG_ERROR("Engine", "Vulkan u8 submit failed error=" + std::to_string(ret) + ", " + detail);
            return std::unexpected(EngineError{17, "Vulkan submit failed (error " + std::to_string(ret) + "); " + detail});        }

        LOG_INFO("Engine", "Finished Vulkan u8 layout path: " + format_vulkan_runtime_snapshot("u8_layout_done", width, height, channels));

        return {};
    }

} // namespace crossscale
