// GPU tile-processing pipeline (3 tiers: u8-queue / float / u8), the SPIRV
// pipeline init helpers, the Vulkan dispatch paths, and the runtime snapshot
// formatter. Split out of ncnn_engine.cpp for readability; the function bodies
// are byte-for-byte identical to the pre-split implementation.

#include "engine.hpp"
#include "ncnn_engine_impl.hpp"
#if NCNN_VULKAN
#include "vulkan_shaders.hpp"
#include "gpu_precision.hpp"
#include "spirv_pack.hpp"
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
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <cstdlib>
#include <future>
#include <limits>
#include <mdspan>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <thread>

namespace upix {

#if NCNN_VULKAN
namespace {

// Raw upload, bypassing VkCompute::record_upload entirely. record_upload's
// delivered element type is device-dependent: discrete GPUs CPU-cast fp32 to
// fp16 (and pass u8 through), while integrated GPUs request an on-the-fly
// device cast that ncnn cannot perform for int8 (convert_packing rejects
// int8 -> fp16/fp32) and may silently skip for fp32 — so a pipeline bound to
// the "wrong" type reads mismatched bits on one device class or the other
// (the Adreno all-zero / garbage hazards). Staging the bytes ourselves with
// the same HOST_WRITE bookkeeping record_upload uses, then record_clone()
// to device memory (a plain vkCmdCopyBuffer with proper barriers) makes the
// bound buffer's contents exactly what we memcpy'd, on every device.
bool stage_raw_input(ncnn::VkCompute& cmd,
                     ncnn::VkAllocator* staging_vkallocator,
                     const void* bytes,
                     size_t total_bytes,
                     size_t elemsize,
                     ncnn::Option& opt,
                     ncnn::VkMat& dst,
                     std::vector<ncnn::VkMat>& keepalive)
{
    // Round the staged payload up to a whole number of uint32 words: the
    // word-addressed custom shaders read src_data[e >> 1] / [e >> 2], and a
    // trailing partial word must stay inside both the allocation and the
    // descriptor range. The pad bytes themselves are never consumed.
    const size_t padded_bytes = (total_bytes + 3u) & ~size_t(3u);
    ncnn::VkMat staged;
    staged.create(static_cast<int>(padded_bytes / elemsize), elemsize, 1, staging_vkallocator);
    if (staged.empty()) {
        return false;
    }
    std::memcpy(staged.mapped_ptr(), bytes, total_bytes);
    staged.allocator->flush(staged.data);
    staged.data->access_flags = VK_ACCESS_HOST_WRITE_BIT;
    staged.data->stage_flags = VK_PIPELINE_STAGE_HOST_BIT;
    cmd.record_clone(staged, dst, opt);
    keepalive.push_back(std::move(staged));
    return !dst.empty();
}

bool stage_u8_input(ncnn::VkCompute& cmd,
                    ncnn::VkAllocator* staging_vkallocator,
                    const uint8_t* bytes,
                    size_t total,
                    ncnn::Option& opt,
                    ncnn::VkMat& dst,
                    std::vector<ncnn::VkMat>& keepalive)
{
    return stage_raw_input(cmd, staging_vkallocator, bytes, total, 1u, opt, dst, keepalive);
}

// Download plumbing for word-backed u8 outputs. record_clone only reuses a
// wrapped CPU Mat when dims match bit-exactly, so stage through a scratch and
// copy the logical bytes out — the copy MUST happen only after
// submit_and_wait (downloads execute inside the submit).
ncnn::Mat record_u8_download(ncnn::VkCompute& cmd, const ncnn::VkMat& gpu_u8,
                             ncnn::Option& opt) {
    const int words = static_cast<int>(gpu_u8.total());
    ncnn::Mat tmp;
    tmp.create(words, static_cast<size_t>(4), 1);
    cmd.record_clone(gpu_u8, tmp, opt);
    return tmp;
}

// Full-output GPU buffers shared by all tile pipelines: a float HWC
// accumulator, a float weight map, optionally the u8 output staging. They
// scale with the OUTPUT image, not the tile size — this term is what makes
// x4 on large inputs unrunnable on small heaps.
size_t full_output_gpu_bytes(size_t out_width, size_t out_height, int channels,
                             bool with_u8_staging) {
    const size_t px = out_width * out_height;
    return px * channels * 4 + px * 4 + (with_u8_staging ? px * channels : 0);
}

// Per-tile inference working set: input tile + fp16 feature maps (~64ch,
// ~16 live blobs in light mode), times the in-flight batch. Rough, on
// purpose — the runtime fallback ladder catches underestimates. For
// pre-padding models the net consumes (tile + 2*prepad)², not tile².
size_t tile_working_set_bytes(const TileConfig& config, int channels, int prepadding) {
    const double t = static_cast<double>(config.tile_size) + 2 * std::max(0, config.overlap) +
                     2 * std::max(0, prepadding);
    const double batch = std::max(1, config.batch_size);
    return static_cast<size_t>(t * t * 64 * 2 * 16 * batch + t * t * channels * 2 * batch);
}

// Preflight device allocations before anything big is created: refuse with a
// clean OOM error (nothing allocated yet) instead of letting VkMat::create
// silently return empty mats that later dispatch garbage.
auto check_gpu_memory_budget(const NCNNEngineImpl& engine, size_t need_bytes,
                             std::string_view what) -> std::optional<EngineError> {
    const size_t budget_mb = engine.get_device_memory_budget_mb();
    if (budget_mb == 0) return std::nullopt;  // unknown budget → let the device decide
    if (need_bytes <= budget_mb * 1024 * 1024) return std::nullopt;
    return EngineError{kErrOutOfMemory,
        std::format("out of GPU memory for {}: need ~{}MB, heap budget {}MB",
                    what, need_bytes / (1024 * 1024), budget_mb)};
}

// VkMat::create fails silently (empty mat) when the allocator refuses — turn
// that into the same OOM error so callers can fall back. Tests `data`, not
// buffer(): VkMat::buffer() dereferences data unconditionally.
auto check_vkmat_allocated(const ncnn::VkMat& mat, std::string_view what)
    -> std::optional<EngineError> {
    if (mat.data != nullptr) return std::nullopt;
    return EngineError{kErrOutOfMemory, "out of GPU memory allocating " + std::string(what)};
}

} // namespace
#endif

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
            // Net input rect (blend-space tile grown by model_prepadding_ on
            // every side): what actually goes into the model. Its internal
            // border crops shrink the output back to in_w x in_h, so every
            // blend / shape check below keeps using the fields above.
            int net_in_w = 0;
            int net_in_h = 0;
            int net_tile_x = 0;
            int net_tile_y = 0;
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
            ncnn::Mat u8_download_scratch;
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
            // Pre-padding models: the net needs a replicate-border margin.
            // Growing the crop rect into real neighboring pixels provides it
            // in the interior; the shader clamps (replicates) at the image
            // border. The 2*prepad the internal crops remove brings the
            // output back to in_w*scale, so blend math is unaffected.
            const int prepad = model_prepadding_;
            info.net_in_w = tile_in_w + 2 * prepad;
            info.net_in_h = tile_in_h + 2 * prepad;
            info.net_tile_x = info.tile_x - prepad;
            info.net_tile_y = info.tile_y - prepad;
            return info;
        };

        std::vector<QueueTaskState> states;
        states.reserve(tasks.size());
        std::vector<std::vector<TilePadInfo>> task_pad_infos;
        task_pad_infos.reserve(tasks.size());
        size_t total_tiles = 0;
        size_t total_need_bytes = 0;
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

        // OOM preflight: full-output accumulators + weight maps + u8 staging +
        // the u8 input upload + the per-tile working set, summed over tasks.
        // Nothing has been allocated yet — a refusal here is free.
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto& task = tasks[i];
            total_need_bytes +=
                full_output_gpu_bytes(states[i].out_width, states[i].out_height, task.channels, true) +
                static_cast<size_t>(task.input_width) * task.input_height * task.channels +
                tile_working_set_bytes(task.config, task.channels, model_prepadding_);
        }
        if (auto oom = check_gpu_memory_budget(*this, total_need_bytes, "u8 queue pipeline")) {
            return std::unexpected(*oom);
        }
        for (auto& state : states) {
            state.out_gpu_hwc.create(static_cast<int>(state.out_total), 4u, blob_vkallocator);
            state.out_gpu_weight.create(static_cast<int>(state.out_total_pixels), 4u, blob_vkallocator);
            // u8 output as uint32 words: whole-word stores in the quantize
            // kernel never cross the allocation or the descriptor range.
            state.out_gpu_u8.create((static_cast<int>(state.out_total) + 3) / 4, 4u, blob_vkallocator);
            if (auto oom = check_vkmat_allocated(state.out_gpu_hwc, "u8 queue output accumulator")) {
                return std::unexpected(*oom);
            }
            if (auto oom = check_vkmat_allocated(state.out_gpu_weight, "u8 queue weight map")) {
                return std::unexpected(*oom);
            }
            if (auto oom = check_vkmat_allocated(state.out_gpu_u8, "u8 queue output staging")) {
                return std::unexpected(*oom);
            }
        }
        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        // Multi-Task Concurrent Tile Scheduling:
        // Each task operates on its own dedicated output buffer (state.out_gpu_hwc).
        // By scheduling up to 1 tile per task per submit round (local_budget = 1),
        // multiple distinct image tasks can execute in parallel within the same VkCompute
        // command buffer without intra-image RAW/WAR hazard.
        const size_t submit_budget = std::max<size_t>(1, std::min<size_t>(states.size(), 16));

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
                return std::unexpected(EngineError{207, std::format("Failed to set Vulkan input (error {})", ret)});
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return std::unexpected(EngineError{208, std::format("Failed to extract Vulkan output (error {})", ret)});
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                if (auto oom = empty_gpu_output_error(out_gpu.w, out_gpu.h, out_gpu.c)) {
                    return std::unexpected(*oom);
                }
                return std::unexpected(EngineError{209, std::format("Vulkan output shape mismatch (got {}x{}x{}, want {}x{}x{})", out_gpu.w, out_gpu.h, out_gpu.c, tile_out_full_w, tile_out_full_h, channels)});
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
        std::vector<ncnn::vk_constant_type> pre_constants(9);
        std::vector<ncnn::VkMat> batch_output_keepalive;
        batch_output_keepalive.reserve(submit_budget);
        std::vector<std::pair<TileInputKey, ncnn::VkMat>> inflight_tile_inputs;
        inflight_tile_inputs.reserve(submit_budget);
        std::vector<ncnn::VkMat> normalize_quantize_bindings(3);
        std::vector<ncnn::vk_constant_type> normalize_quantize_constants(2);
        std::vector<ncnn::VkMat> clear_bindings(1);
        std::vector<ncnn::vk_constant_type> clear_constants(1);
        // ---- TEMP PERF INSTRUMENTATION (remove after measuring) ----
        const bool cs_perf = std::getenv("UPIX_PIPELINE_TIMER") != nullptr;
        double perf_record_ms = 0.0, perf_submit_ms = 0.0;
        long perf_submits = 0;
        long perf_tiles_total = static_cast<long>(total_tiles);
        auto perf_t0 = std::chrono::steady_clock::now();
        // ---- END TEMP ----
        while (true) {
            if (cancel_flag) {
                return std::unexpected(EngineError{3, "Processing cancelled"});
            }

            auto perf_t_rec_start = std::chrono::steady_clock::now();
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::VkMat> u8_upload_staging;
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
                    if (!stage_u8_input(cmd, staging_vkallocator,
                                        state.task->input_data, in_total,
                                        opt, state.in_gpu_full, u8_upload_staging)) {
                        return std::unexpected(EngineError{212, "Vulkan u8 input upload failed (staging)"});
                    }
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

                    // Net input: blend-space tile grown by model_prepadding_.
                    auto pooled_input = acquire_tile_input(pad_info.net_in_w, pad_info.net_in_h, state.task->channels);
                    ncnn::VkMat in_gpu_chw = std::move(pooled_input.second);

                    pre_bindings[0] = state.in_gpu_full;
                    pre_bindings[1] = in_gpu_chw;

                    pre_constants[0].i = state.task->input_width;
                    pre_constants[1].i = state.task->input_height;
                    pre_constants[2].i = state.task->channels;
                    pre_constants[3].i = pad_info.net_tile_x;
                    pre_constants[4].i = pad_info.net_tile_y;
                    pre_constants[5].i = pad_info.net_in_w;
                    pre_constants[6].i = pad_info.net_in_h;
                    pre_constants[7].i = static_cast<int>(in_gpu_chw.cstep);
                    pre_constants[8].i = (model_prepadding_ > 0) ? 1 : 0;  // border replicate

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = (state.task->channels * static_cast<int>(in_gpu_chw.cstep) + 1) / 2;
                    pre_dispatcher.h = 1;
                    pre_dispatcher.c = 1;
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
                    normalize_quantize_dispatcher.w =
                        (static_cast<int>(state.out_total_pixels) * state.task->channels + 3) / 4;
                    normalize_quantize_dispatcher.h = 1;
                    normalize_quantize_dispatcher.c = 1;
                    cmd.record_pipeline(normalize_quantize_output_pipeline_.get(),
                                        normalize_quantize_bindings,
                                        normalize_quantize_constants,
                                        normalize_quantize_dispatcher);

                    state.u8_download_scratch = record_u8_download(cmd, state.out_gpu_u8, opt);
                }
            }

            auto perf_t_rec_end = std::chrono::steady_clock::now();
            if (cs_perf) perf_record_ms += std::chrono::duration<double, std::milli>(perf_t_rec_end - perf_t_rec_start).count();
            auto perf_t_sub_start = std::chrono::steady_clock::now();
            int submit_ret = cmd.submit_and_wait();
            if (cs_perf) {
                perf_submit_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perf_t_sub_start).count();
                perf_submits++;
            }
            if (submit_ret != 0) {
                if (all_tiles_scheduled) {
                    return std::unexpected(EngineError{214, std::format("Vulkan queue normalize+quantize/download failed (error {})", submit_ret)});
                }
                if (has_init_work) {
                    return std::unexpected(EngineError{206, std::format("Vulkan queue init failed (error {})", submit_ret)});
                }
                return std::unexpected(EngineError{210, std::format("Vulkan submit failed (error {})", submit_ret)});
            }
            if (all_tiles_scheduled) {
                for (auto& state : states) {
                    std::memcpy(state.task->output_data,
                                static_cast<const uint8_t*>(state.u8_download_scratch.data),
                                state.out_total);
                }
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
            std::vector<ncnn::Mat> scratches;
            for (auto& state : states) {
                normalize_quantize_bindings[0] = state.out_gpu_hwc;
                normalize_quantize_bindings[1] = state.out_gpu_weight;
                normalize_quantize_bindings[2] = state.out_gpu_u8;

                normalize_quantize_constants[0].i = static_cast<int>(state.out_total_pixels);
                normalize_quantize_constants[1].i = state.task->channels;

                ncnn::VkMat normalize_quantize_dispatcher;
                normalize_quantize_dispatcher.w =
                    (static_cast<int>(state.out_total_pixels) * state.task->channels + 3) / 4;
                normalize_quantize_dispatcher.h = 1;
                normalize_quantize_dispatcher.c = 1;
                cmd.record_pipeline(normalize_quantize_output_pipeline_.get(),
                                    normalize_quantize_bindings,
                                    normalize_quantize_constants,
                                    normalize_quantize_dispatcher);

                scratches.push_back(record_u8_download(cmd, state.out_gpu_u8, opt));
            }

            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{214, std::format("Vulkan queue normalize+quantize/download failed (error {})", submit_ret)});
            }
            size_t si = 0;
            for (auto& state : states) {
                std::memcpy(state.task->output_data,
                            static_cast<const uint8_t*>(scratches[si].data),
                            state.out_total);
                ++si;
            }
        }

        // ---- TEMP PERF INSTRUMENTATION print ----
        if (cs_perf) {
            double perf_total_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - perf_t0).count();
            LOG_INFO("Engine",
                "[PERF] u8_queue pipeline: tiles={} submits={} total={:.1f}ms record={:.1f}ms submit={:.1f}ms "
                "(submit={:.0f}% of total, {:.3f}ms/submit)",
                perf_tiles_total, perf_submits, perf_total_ms, perf_record_ms, perf_submit_ms,
                perf_total_ms > 0 ? 100.0 * perf_submit_ms / perf_total_ms : 0.0,
                perf_submits > 0 ? perf_submit_ms / perf_submits : 0.0);
        }
        // ---- END TEMP ----

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

        // OOM preflight (full-output accumulator + weight map + float input
        // upload + per-tile working set) — before any big allocation.
        if (auto oom = check_gpu_memory_budget(
                *this,
                full_output_gpu_bytes(out_width, out_height, channels, false) +
                    static_cast<size_t>(input_width) * input_height * channels * 4 +
                    tile_working_set_bytes(config, channels, model_prepadding_),
                "float tile pipeline")) {
            return std::unexpected(*oom);
        }

        ncnn::VkMat out_gpu_hwc;
        out_gpu_hwc.create(static_cast<int>(out_total), 4u, blob_vkallocator);
        ncnn::VkMat out_gpu_weight;
        out_gpu_weight.create(static_cast<int>(out_total_pixels), 4u, blob_vkallocator);
        if (auto oom = check_vkmat_allocated(out_gpu_hwc, "float tile output accumulator")) {
            return std::unexpected(*oom);
        }
        if (auto oom = check_vkmat_allocated(out_gpu_weight, "float tile weight map")) {
            return std::unexpected(*oom);
        }

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
                return std::unexpected(EngineError{204, std::format("Vulkan clear failed (error {})", submit_ret)});
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        // 4-Pass Disjoint Tile Batching:
        // Tiles with grid coordinates of the same parity (gx % 2, gy % 2) have strictly non-overlapping
        // inner and output blending areas. All tiles within the same pass can be safely recorded and
        // executed in the SAME VkCompute command buffer without memory hazards.
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
            // Net input rect (blend-space tile grown by model_prepadding_ on
            // every side): what actually goes into the model. Its internal
            // border crops shrink the output back to in_w x in_h, so every
            // blend / shape check below keeps using the fields above.
            int net_in_w = 0;
            int net_in_h = 0;
            int net_tile_x = 0;
            int net_tile_y = 0;
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
            // Pre-padding models: the net needs a replicate-border margin.
            // Growing the crop rect into real neighboring pixels provides it
            // in the interior; the shader clamps (replicates) at the image
            // border. The 2*prepad the internal crops remove brings the
            // output back to in_w*scale, so blend math is unaffected.
            const int prepad = model_prepadding_;
            info.net_in_w = tile_in_w + 2 * prepad;
            info.net_in_h = tile_in_h + 2 * prepad;
            info.net_tile_x = info.tile_x - prepad;
            info.net_tile_y = info.tile_y - prepad;
            return info;
        };

        bool use_gpu_crop = gpu_crop_pipeline_ready_;
        ncnn::VkMat in_gpu_full;
        if (use_gpu_crop) {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::VkMat> float_upload_staging;
            if (!stage_raw_input(cmd, staging_vkallocator,
                                 input_data, in_total_full * sizeof(float), 4u,
                                 opt, in_gpu_full, float_upload_staging)) {
                return std::unexpected(EngineError{210, "Vulkan input upload failed (staging)"});
            }
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{210, std::format("Vulkan input upload failed (error {})", submit_ret)});
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
                return std::unexpected(EngineError{207, std::format("Failed to set Vulkan input (error {})", ret)});
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return std::unexpected(EngineError{208, std::format("Failed to extract Vulkan output (error {})", ret)});
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                if (auto oom = empty_gpu_output_error(out_gpu.w, out_gpu.h, out_gpu.c)) {
                    return std::unexpected(*oom);
                }
                return std::unexpected(EngineError{209, std::format("Vulkan output shape mismatch (got {}x{}x{}, want {}x{}x{})", out_gpu.w, out_gpu.h, out_gpu.c, tile_out_full_w, tile_out_full_h, channels)});
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
            const int tile_step = std::max(1, config.tile_size - config.overlap);
            std::array<std::vector<size_t>, 4> pass_indices;
            if (config.overlap > 0) {
                // views::enumerate is unavailable on NDK29 libc++; index+range
                // pattern keeps the same pairing portably.
                for (auto i : std::views::iota(size_t{0}, regions.size())) {
                    const auto& reg = regions[i];
                    int gx = static_cast<int>(reg.x) / tile_step;
                    int gy = static_cast<int>(reg.y) / tile_step;
                    int pass_id = ((gy & 1) << 1) | (gx & 1);
                    pass_indices[pass_id].push_back(i);
                }
            } else {
                pass_indices[0].reserve(regions.size());
                for (auto i : std::views::iota(size_t{0}, regions.size())) {
                    pass_indices[0].push_back(i);
                }
            }

            const size_t heap_budget_mb = static_cast<size_t>(vkdev_->get_heap_budget());
            const size_t max_tiles_per_submit = (heap_budget_mb > 2000) ? 64 : 16;

            for (int pass_id = 0; pass_id < 4; ++pass_id) {
                const auto& p_indices = pass_indices[pass_id];
                if (p_indices.empty()) continue;

                size_t pass_cursor = 0;
                while (pass_cursor < p_indices.size()) {
                    if (cancel_flag) {
                        return std::unexpected(EngineError{3, "Processing cancelled"});
                    }

                    size_t chunk_end = std::min(p_indices.size(), pass_cursor + max_tiles_per_submit);
                    size_t chunk_size = chunk_end - pass_cursor;

                    ncnn::VkCompute cmd(vkdev_);
                    std::vector<ncnn::VkMat> batch_keepalive;
                    batch_keepalive.reserve(chunk_size * 2);

                    for (size_t i = pass_cursor; i < chunk_end; ++i) {
                        size_t r_idx = p_indices[i];
                        const auto& region = regions[r_idx];
                        const TilePadInfo pad_info = compute_pad(region);
                        const int tile_in_w = pad_info.in_w;
                        const int tile_in_h = pad_info.in_h;
                        const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                        if (tile_in_total == 0) {
                            return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                        }

                        // Net input: blend-space tile grown by model_prepadding_.
                        ncnn::VkMat in_gpu_chw;
                        in_gpu_chw.create(pad_info.net_in_w, pad_info.net_in_h, channels, in_elemsize, 1, blob_vkallocator);

                        std::vector<ncnn::VkMat> pre_bindings(2);
                        pre_bindings[0] = in_gpu_full;
                        pre_bindings[1] = in_gpu_chw;

                        std::vector<ncnn::vk_constant_type> pre_constants(9);
                        pre_constants[0].i = input_width;
                        pre_constants[1].i = input_height;
                        pre_constants[2].i = channels;
                        pre_constants[3].i = pad_info.net_tile_x;
                        pre_constants[4].i = pad_info.net_tile_y;
                        pre_constants[5].i = pad_info.net_in_w;
                        pre_constants[6].i = pad_info.net_in_h;
                        pre_constants[7].i = static_cast<int>(in_gpu_chw.cstep);
                        pre_constants[8].i = (model_prepadding_ > 0) ? 1 : 0;  // border replicate

                        ncnn::VkMat pre_dispatcher;
                        pre_dispatcher.w = (channels * static_cast<int>(in_gpu_chw.cstep) + 1) / 2;
                        pre_dispatcher.h = 1;
                        pre_dispatcher.c = 1;
                        cmd.record_pipeline(preproc_hwc_crop_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

                        auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                                 tile_in_w, tile_in_h);
                        if (!record_result.has_value()) {
                            return std::unexpected(record_result.error());
                        }
                    }

                    int submit_ret = cmd.submit_and_wait();
                    if (submit_ret != 0) {
                        return std::unexpected(EngineError{210, std::format("Vulkan submit failed (error {})", submit_ret)});
                    }

                    for (size_t i = pass_cursor; i < chunk_end; ++i) {
                        size_t r_idx = p_indices[i];
                        const auto& region = regions[r_idx];
                        progress.completed_tiles++;
                        progress.current_tile_x = region.x;
                        progress.current_tile_y = region.y;
                        if (progress_callback) {
                            progress_callback(progress);
                        }
                    }

                    pass_cursor = chunk_end;
                }
            }
        } else {
            const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
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

                    // Net input: blend-space tile grown by model_prepadding_,
                    // replicate (clamp) at the image border like the GPU
                    // crop shader; mirror everywhere else, as before.
                    const bool replicate = model_prepadding_ > 0;
                    std::vector<float> tile_input(static_cast<size_t>(pad_info.net_in_w) * pad_info.net_in_h * channels);
                    for (int y = 0; y < pad_info.net_in_h; ++y) {
                        int src_y = pad_info.net_tile_y + y;
                        src_y = replicate ? std::clamp(src_y, 0, input_height - 1)
                                          : (input_height - 1) - std::abs(std::abs(src_y) - (input_height - 1));
                        for (int x = 0; x < pad_info.net_in_w; ++x) {
                            int src_x = pad_info.net_tile_x + x;
                            src_x = replicate ? std::clamp(src_x, 0, input_width - 1)
                                              : (input_width - 1) - std::abs(std::abs(src_x) - (input_width - 1));
                            const size_t src_idx = (static_cast<size_t>(src_y) * input_width + src_x) * channels;
                            const size_t dst_idx = (static_cast<size_t>(y) * pad_info.net_in_w + x) * channels;
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

                    // Net input: blend-space tile grown by model_prepadding_.
                    const size_t net_in_total =
                        static_cast<size_t>(pad_info.net_in_w) * pad_info.net_in_h * channels;
                    ncnn::VkMat in_gpu_hwc;
                    if (!stage_raw_input(cmd, staging_vkallocator,
                                         current_batch->input_buffers[idx].data(),
                                         net_in_total * sizeof(float), 4u,
                                         opt, in_gpu_hwc, batch_keepalive)) {
                        return std::unexpected(EngineError{210, "Vulkan input upload failed (staging)"});
                    }

                    ncnn::VkMat in_gpu_chw;
                    in_gpu_chw.create(pad_info.net_in_w, pad_info.net_in_h, channels, in_elemsize, 1, blob_vkallocator);

                    std::vector<ncnn::VkMat> pre_bindings(2);
                    pre_bindings[0] = in_gpu_hwc;
                    pre_bindings[1] = in_gpu_chw;

                    std::vector<ncnn::vk_constant_type> pre_constants(4);
                    pre_constants[0].i = pad_info.net_in_w;
                    pre_constants[1].i = pad_info.net_in_h;
                    pre_constants[2].i = channels;
                    pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = (channels * static_cast<int>(in_gpu_chw.cstep) + 1) / 2;
                    pre_dispatcher.h = 1;
                    pre_dispatcher.c = 1;
                    cmd.record_pipeline(preproc_hwc_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);
                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                             tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                    batch_keepalive.emplace_back(in_gpu_hwc);
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    return std::unexpected(EngineError{210, std::format("Vulkan submit failed (error {})", submit_ret)});
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
                return std::unexpected(EngineError{214, std::format("Vulkan normalize/download failed (error {})", submit_ret)});
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

        // OOM preflight (full-output accumulator + weight map + u8 download
        // staging + per-tile working set) — before any big allocation. The
        // u8 staging is created late (last batch) but sized like the output,
        // so it is budgeted up front.
        if (auto oom = check_gpu_memory_budget(
                *this,
                full_output_gpu_bytes(out_width, out_height, channels, true) +
                    tile_working_set_bytes(config, channels, model_prepadding_),
                "u8 tile pipeline")) {
            return std::unexpected(*oom);
        }

        ncnn::VkMat out_gpu_hwc;
        out_gpu_hwc.create(static_cast<int>(out_total), 4u, blob_vkallocator);
        ncnn::VkMat out_gpu_weight;
        out_gpu_weight.create(static_cast<int>(out_total_pixels), 4u, blob_vkallocator);
        if (auto oom = check_vkmat_allocated(out_gpu_hwc, "u8 tile output accumulator")) {
            return std::unexpected(*oom);
        }
        if (auto oom = check_vkmat_allocated(out_gpu_weight, "u8 tile weight map")) {
            return std::unexpected(*oom);
        }

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
                return std::unexpected(EngineError{204, std::format("Vulkan clear failed (error {})", submit_ret)});
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        // 4-Pass Disjoint Tile Batching:
        // Tiles with grid coordinates of the same parity (gx % 2, gy % 2) have strictly non-overlapping
        // inner and output blending areas. All tiles within the same pass can be safely recorded and
        // executed in the SAME VkCompute command buffer without memory hazards.
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
            // Net input rect (blend-space tile grown by model_prepadding_ on
            // every side): what actually goes into the model. Its internal
            // border crops shrink the output back to in_w x in_h, so every
            // blend / shape check below keeps using the fields above.
            int net_in_w = 0;
            int net_in_h = 0;
            int net_tile_x = 0;
            int net_tile_y = 0;
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
            // Pre-padding models: the net needs a replicate-border margin.
            // Growing the crop rect into real neighboring pixels provides it
            // in the interior; the shader clamps (replicates) at the image
            // border. The 2*prepad the internal crops remove brings the
            // output back to in_w*scale, so blend math is unaffected.
            const int prepad = model_prepadding_;
            info.net_in_w = tile_in_w + 2 * prepad;
            info.net_in_h = tile_in_h + 2 * prepad;
            info.net_tile_x = info.tile_x - prepad;
            info.net_tile_y = info.tile_y - prepad;
            return info;
        };

        bool use_gpu_crop = gpu_crop_u8_pipeline_ready_;
        ncnn::VkMat in_gpu_full;
        if (use_gpu_crop) {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::VkMat> u8_upload_staging;
            if (!stage_u8_input(cmd, staging_vkallocator,
                                input_data, in_total_full,
                                opt, in_gpu_full, u8_upload_staging)) {
                return std::unexpected(EngineError{212, "Vulkan u8 input upload failed (staging)"});
            }
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{212, std::format("Vulkan u8 input upload failed (error {})", submit_ret)});
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
                return std::unexpected(EngineError{207, std::format("Failed to set Vulkan input (error {})", ret)});
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return std::unexpected(EngineError{208, std::format("Failed to extract Vulkan output (error {})", ret)});
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                if (auto oom = empty_gpu_output_error(out_gpu.w, out_gpu.h, out_gpu.c)) {
                    return std::unexpected(*oom);
                }
                return std::unexpected(EngineError{209, std::format("Vulkan output shape mismatch (got {}x{}x{}, want {}x{}x{})", out_gpu.w, out_gpu.h, out_gpu.c, tile_out_full_w, tile_out_full_h, channels)});
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

        // Records quantize + download into a caller-held scratch; the host
        // copy happens after the caller's submit_and_wait.
        auto record_normalize_quantize_download = [&](ncnn::VkCompute& cmd,
                                                      ncnn::VkMat& out_gpu_u8,
                                                      ncnn::Mat& scratch) {
            std::vector<ncnn::VkMat> normalize_quantize_bindings(3);
            normalize_quantize_bindings[0] = out_gpu_hwc;
            normalize_quantize_bindings[1] = out_gpu_weight;
            normalize_quantize_bindings[2] = out_gpu_u8;

            std::vector<ncnn::vk_constant_type> normalize_quantize_constants(2);
            normalize_quantize_constants[0].i = static_cast<int>(out_total_pixels);
            normalize_quantize_constants[1].i = channels;

            ncnn::VkMat normalize_quantize_dispatcher;
            normalize_quantize_dispatcher.w =
                (static_cast<int>(out_total_pixels) * channels + 3) / 4;
            normalize_quantize_dispatcher.h = 1;
            normalize_quantize_dispatcher.c = 1;
            cmd.record_pipeline(normalize_quantize_output_pipeline_.get(), normalize_quantize_bindings,
                                normalize_quantize_constants, normalize_quantize_dispatcher);

            scratch = record_u8_download(cmd, out_gpu_u8, opt);
        };

        bool output_downloaded = false;

        if (use_gpu_crop && preproc_hwc_crop_u8_to_chw_ == nullptr) {
            return std::unexpected(EngineError{213, "GPU u8 crop pipeline unavailable"});
        }

        // ---- TEMP PERF INSTRUMENTATION (crop-u8 hot path) ----
        const bool cs_perf2 = std::getenv("UPIX_PIPELINE_TIMER") != nullptr;
        double p2_record_ms = 0.0, p2_submit_ms = 0.0;
        long p2_submits = 0, p2_tiles = static_cast<long>(regions.size());
        auto p2_t0 = std::chrono::steady_clock::now();
        // ---- END TEMP ----

        if (use_gpu_crop) {
            LOG_INFO("Engine", "Using GPU tile pipeline with 4-pass disjoint tile batching");
            
            const int tile_step = std::max(1, config.tile_size - config.overlap);
            std::array<std::vector<size_t>, 4> pass_indices;
            if (config.overlap > 0) {
                // views::enumerate is unavailable on NDK29 libc++; index+range
                // pattern keeps the same pairing portably.
                for (auto i : std::views::iota(size_t{0}, regions.size())) {
                    const auto& reg = regions[i];
                    int gx = static_cast<int>(reg.x) / tile_step;
                    int gy = static_cast<int>(reg.y) / tile_step;
                    int pass_id = ((gy & 1) << 1) | (gx & 1);
                    pass_indices[pass_id].push_back(i);
                }
            } else {
                pass_indices[0].reserve(regions.size());
                for (auto i : std::views::iota(size_t{0}, regions.size())) {
                    pass_indices[0].push_back(i);
                }
            }

            const size_t heap_budget_mb = static_cast<size_t>(vkdev_->get_heap_budget());
            const size_t max_tiles_per_submit = (heap_budget_mb > 2000) ? 64 : 16;

            // ---- Chunk submit/execute overlap --------------------------------
            // submit_and_wait() blocks the recording thread until the GPU has
            // finished the whole chunk, so the GPU idles whenever the CPU
            // records the next chunk (4-5% of the pipeline wall time, see the
            // UPIX_PIPELINE_TIMER p2_record_ms/p2_submit_ms split). ncnn
            // 20260113's VkCompute offers no fence-based submit (public API:
            // submit_and_wait()/reset() only; the command buffer and fence
            // are private), so instead the blocking half of submit_and_wait
            // moves to a dedicated submit thread and two alternating
            // VkCompute objects double-buffer the chunks: the CPU records
            // chunk N+1 while chunk N executes on the GPU.
            //
            // Safety rules this structure must preserve:
            //  * ncnn does not reference-track VkBufferMemory — every VkMat a
            //    recorded command buffer touches (tile inputs, tile outputs,
            //    download staging) stays alive in the slot's keepalive until
            //    the chunk's fence has signalled, and is only recycled /
            //    reset afterwards (never while in flight).
            //  * Only the main thread mutates the blob/staging allocators
            //    (record, reset, keepalive recycle); the submit thread runs
            //    submit_and_wait() only, which touches neither. Vulkan-wise
            //    each VkCompute owns its private command pool/buffer/fence,
            //    and the queue is handed out by ncnn's mutex-guarded
            //    acquire_queue/reclaim_queue.
            //  * Submits stay strictly ordered (single submit thread, FIFO
            //    queue), so cross-chunk WAR hazards on the shared full-image
            //    buffers keep the submission-order dependency they rely on.
            //  * Double buffering holds one extra chunk of tile in/out
            //    buffers (~77MB at 64 x 200px tiles, fp16) on top of the
            //    existing budget; intermediates keep being recycled within
            //    a chunk's recording as before.
            struct ChunkSlot {
                std::unique_ptr<ncnn::VkCompute> cmd;
                std::vector<ncnn::VkMat> keepalive;
                const std::vector<size_t>* indices = nullptr;  // pass_indices[p], stable
                size_t chunk_begin = 0;
                size_t chunk_end = 0;
                bool pending = false;
                bool pending_is_last = false;
                ncnn::Mat pending_scratch;
                std::future<int> pending_future;
            };
            ChunkSlot slots[2];

            struct SubmitJob {
                ncnn::VkCompute* cmd;
                std::promise<int> result;
            };
            std::mutex submit_mutex;
            std::condition_variable submit_cv;
            std::deque<SubmitJob> submit_jobs;
            bool submit_stop = false;
            std::thread submit_thread([&] {
                while (true) {
                    SubmitJob job{nullptr, {}};
                    {
                        std::unique_lock<std::mutex> lock(submit_mutex);
                        submit_cv.wait(lock, [&] { return submit_stop || !submit_jobs.empty(); });
                        if (submit_jobs.empty()) {
                            return;  // stop requested and queue drained
                        }
                        job = std::move(submit_jobs.front());
                        submit_jobs.pop_front();
                    }
                    job.result.set_value(job.cmd->submit_and_wait());
                }
            });
            struct SubmitThreadGuard {
                std::thread& thread;
                std::mutex& mutex;
                std::condition_variable& cv;
                bool& stop;
                ~SubmitThreadGuard() {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        stop = true;
                    }
                    cv.notify_all();
                    thread.join();
                }
            } submit_thread_guard{submit_thread, submit_mutex, submit_cv, submit_stop};

            // Completion-ordered tile count (progress bookkeeping); the
            // record-side count below drives the is_last_batch decision.
            size_t total_completed_tiles = 0;
            size_t tiles_recorded = 0;
            size_t chunk_counter = 0;

            // Wait for the chunk recorded into slot `s` two iterations ago,
            // then do its post-completion work (output copy, progress,
            // keepalive recycle, command reset) on the main thread.
            auto flush_slot = [&](size_t s) -> expected<void, EngineError> {
                ChunkSlot& slot = slots[s];
                if (!slot.pending) {
                    return {};
                }
                const auto p2_wait_start = std::chrono::steady_clock::now();
                const int submit_ret = slot.pending_future.get();
                if (cs_perf2) {
                    p2_submit_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - p2_wait_start).count();
                    p2_submits++;
                }
                slot.pending = false;
                if (submit_ret != 0) {
                    if (slot.pending_is_last) {
                        return std::unexpected(EngineError{214, std::format("Vulkan normalize+quantize/download failed (error {})", submit_ret)});
                    }
                    return std::unexpected(EngineError{210, std::format("Vulkan submit failed (error {})", submit_ret)});
                }
                if (slot.pending_is_last) {
                    output_downloaded = true;
                    std::memcpy(output_data, static_cast<const uint8_t*>(slot.pending_scratch.data),
                                out_total);
                }

                for (size_t i = slot.chunk_begin; i < slot.chunk_end; ++i) {
                    size_t r_idx = (*slot.indices)[i];
                    const auto& region = regions[r_idx];
                    progress.completed_tiles++;
                    progress.current_tile_x = region.x;
                    progress.current_tile_y = region.y;
                    if (progress_callback) {
                        progress_callback(progress);
                    }
                }
                total_completed_tiles += slot.chunk_end - slot.chunk_begin;

                // Fence signalled — the chunk's buffers may be recycled now.
                slot.keepalive.clear();
                slot.pending_scratch = ncnn::Mat();
                if (slot.cmd) {
                    int reset_ret = slot.cmd->reset();
                    if (reset_ret != 0) {
                        return std::unexpected(EngineError{210, std::format("Vulkan command buffer reset failed (error {})", reset_ret)});
                    }
                }
                return {};
            };

            for (int pass_id = 0; pass_id < 4; ++pass_id) {
                const auto& p_indices = pass_indices[pass_id];
                if (p_indices.empty()) continue;

                size_t pass_cursor = 0;
                while (pass_cursor < p_indices.size()) {
                    if (cancel_flag) {
                        return std::unexpected(EngineError{3, "Processing cancelled"});
                    }

                    // Reusing this slot means its previous chunk (recorded
                    // two chunks ago) must be complete first.
                    const size_t slot_idx = chunk_counter & 1;
                    chunk_counter++;
                    if (auto flushed = flush_slot(slot_idx); !flushed.has_value()) {
                        return std::unexpected(flushed.error());
                    }
                    ChunkSlot& slot = slots[slot_idx];
                    if (!slot.cmd) {
                        slot.cmd = std::make_unique<ncnn::VkCompute>(vkdev_);
                    }
                    ncnn::VkCompute& cmd = *slot.cmd;

                    auto perf_t_rec_start2 = std::chrono::steady_clock::now();
                    size_t chunk_end = std::min(p_indices.size(), pass_cursor + max_tiles_per_submit);
                    size_t chunk_size = chunk_end - pass_cursor;

                    slot.indices = &p_indices;
                    slot.chunk_begin = pass_cursor;
                    slot.chunk_end = chunk_end;
                    slot.keepalive.clear();
                    slot.keepalive.reserve(chunk_size * 2);

                    for (size_t i = pass_cursor; i < chunk_end; ++i) {
                        size_t r_idx = p_indices[i];
                        const auto& region = regions[r_idx];
                        const TilePadInfo pad_info = compute_pad(region);
                        const int tile_in_w = pad_info.in_w;
                        const int tile_in_h = pad_info.in_h;
                        const size_t tile_in_total = static_cast<size_t>(tile_in_w) * tile_in_h * channels;
                        if (tile_in_total == 0) {
                            return std::unexpected(EngineError{206, "Invalid tile dimensions"});
                        }

                        // Net input: blend-space tile grown by model_prepadding_.
                        ncnn::VkMat in_gpu_chw;
                        in_gpu_chw.create(pad_info.net_in_w, pad_info.net_in_h, channels, in_elemsize, 1, blob_vkallocator);

                        std::vector<ncnn::VkMat> pre_bindings(2);
                        pre_bindings[0] = in_gpu_full;
                        pre_bindings[1] = in_gpu_chw;

                        std::vector<ncnn::vk_constant_type> pre_constants(9);
                        pre_constants[0].i = input_width;
                        pre_constants[1].i = input_height;
                        pre_constants[2].i = channels;
                        pre_constants[3].i = pad_info.net_tile_x;
                        pre_constants[4].i = pad_info.net_tile_y;
                        pre_constants[5].i = pad_info.net_in_w;
                        pre_constants[6].i = pad_info.net_in_h;
                        pre_constants[7].i = static_cast<int>(in_gpu_chw.cstep);
                        pre_constants[8].i = (model_prepadding_ > 0) ? 1 : 0;  // border replicate

                        ncnn::VkMat pre_dispatcher;
                        pre_dispatcher.w = (channels * static_cast<int>(in_gpu_chw.cstep) + 1) / 2;
                        pre_dispatcher.h = 1;
                        pre_dispatcher.c = 1;
                        cmd.record_pipeline(preproc_hwc_crop_u8_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);

                        auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, slot.keepalive,
                                                                 tile_in_w, tile_in_h);
                        if (!record_result.has_value()) {
                            return std::unexpected(record_result.error());
                        }
                    }

                    const bool is_last_batch = (tiles_recorded + chunk_size == regions.size());
                    if (is_last_batch) {
                        ncnn::VkMat out_gpu_u8;
                        out_gpu_u8.create((static_cast<int>(out_total) + 3) / 4, 4u, blob_vkallocator);
                        if (auto oom = check_vkmat_allocated(out_gpu_u8, "u8 tile download staging")) {
                            return std::unexpected(*oom);
                        }
                        record_normalize_quantize_download(cmd, out_gpu_u8, slot.pending_scratch);
                        // The download staging buffer is referenced by the
                        // in-flight command buffer — keep it alive with the
                        // rest of the chunk's mats until the fence signals.
                        slot.keepalive.emplace_back(std::move(out_gpu_u8));
                    }
                    tiles_recorded += chunk_size;

                    if (cs_perf2) p2_record_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perf_t_rec_start2).count();
                    // Hand the finished recording to the submit thread; the
                    // GPU starts it while the CPU records the next chunk.
                    {
                        std::promise<int> result;
                        slot.pending_future = result.get_future();
                        SubmitJob job{&cmd, std::move(result)};
                        {
                            std::lock_guard<std::mutex> lock(submit_mutex);
                            submit_jobs.push_back(std::move(job));
                        }
                        submit_cv.notify_one();
                        slot.pending = true;
                        slot.pending_is_last = is_last_batch;
                    }

                    pass_cursor = chunk_end;
                }
            }

            // Drain both slots, older chunk first.
            if (auto flushed = flush_slot(chunk_counter & 1); !flushed.has_value()) {
                return std::unexpected(flushed.error());
            }
            if (auto flushed = flush_slot((chunk_counter + 1) & 1); !flushed.has_value()) {
                return std::unexpected(flushed.error());
            }
        } else {
            const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
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

                    // Net input: blend-space tile grown by model_prepadding_,
                    // replicate (clamp) at the image border like the GPU
                    // crop shader; mirror everywhere else, as before.
                    const bool replicate = model_prepadding_ > 0;
                    std::vector<uint8_t> tile_input(static_cast<size_t>(pad_info.net_in_w) * pad_info.net_in_h * channels);
                    for (int y = 0; y < pad_info.net_in_h; ++y) {
                        int src_y = pad_info.net_tile_y + y;
                        src_y = replicate ? std::clamp(src_y, 0, input_height - 1)
                                          : (input_height - 1) - std::abs(std::abs(src_y) - (input_height - 1));
                        for (int x = 0; x < pad_info.net_in_w; ++x) {
                            int src_x = pad_info.net_tile_x + x;
                            src_x = replicate ? std::clamp(src_x, 0, input_width - 1)
                                              : (input_width - 1) - std::abs(std::abs(src_x) - (input_width - 1));
                            const size_t src_idx = (static_cast<size_t>(src_y) * input_width + src_x) * channels;
                            const size_t dst_idx = (static_cast<size_t>(y) * pad_info.net_in_w + x) * channels;
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

                    // Net input: blend-space tile grown by model_prepadding_.
                    const size_t net_in_total =
                        static_cast<size_t>(pad_info.net_in_w) * pad_info.net_in_h * channels;
                    ncnn::VkMat in_gpu_hwc;
                    if (!stage_u8_input(cmd, staging_vkallocator,
                                        current_batch->input_buffers[idx].data(), net_in_total,
                                        opt, in_gpu_hwc, batch_keepalive)) {
                        return std::unexpected(EngineError{212, "Vulkan u8 input upload failed (staging)"});
                    }

                    ncnn::VkMat in_gpu_chw;
                    in_gpu_chw.create(pad_info.net_in_w, pad_info.net_in_h, channels, in_elemsize, 1, blob_vkallocator);

                    std::vector<ncnn::VkMat> pre_bindings(2);
                    pre_bindings[0] = in_gpu_hwc;
                    pre_bindings[1] = in_gpu_chw;

                    std::vector<ncnn::vk_constant_type> pre_constants(4);
                    pre_constants[0].i = pad_info.net_in_w;
                    pre_constants[1].i = pad_info.net_in_h;
                    pre_constants[2].i = channels;
                    pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

                    ncnn::VkMat pre_dispatcher;
                    pre_dispatcher.w = (channels * static_cast<int>(in_gpu_chw.cstep) + 1) / 2;
                    pre_dispatcher.h = 1;
                    pre_dispatcher.c = 1;
                    cmd.record_pipeline(preproc_hwc_u8_to_chw_.get(), pre_bindings, pre_constants, pre_dispatcher);
                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, batch_keepalive,
                                                             tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return std::unexpected(record_result.error());                    }
                    batch_keepalive.emplace_back(in_gpu_hwc);
                }

                ncnn::VkMat out_gpu_u8;
                ncnn::Mat u8_scratch;
                const bool is_last_batch = (current_batch->end == regions.size());
                if (is_last_batch) {
                    out_gpu_u8.create((static_cast<int>(out_total) + 3) / 4, 4u, blob_vkallocator);
                    if (auto oom = check_vkmat_allocated(out_gpu_u8, "u8 tile download staging")) {
                        return std::unexpected(*oom);
                    }
                    record_normalize_quantize_download(cmd, out_gpu_u8, u8_scratch);
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    if (is_last_batch) {
                        return std::unexpected(EngineError{214, std::format("Vulkan normalize+quantize/download failed (error {})", submit_ret)});
                    }
                    return std::unexpected(EngineError{210, std::format("Vulkan submit failed (error {})", submit_ret)});
                }
                if (is_last_batch) {
                    output_downloaded = true;
                    std::memcpy(output_data, static_cast<const uint8_t*>(u8_scratch.data),
                                out_total);
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
            out_gpu_u8.create((static_cast<int>(out_total) + 3) / 4, 4u, blob_vkallocator);
            if (auto oom = check_vkmat_allocated(out_gpu_u8, "u8 tile download staging")) {
                return std::unexpected(*oom);
            }
            ncnn::Mat u8_scratch;
            record_normalize_quantize_download(cmd, out_gpu_u8, u8_scratch);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{214, std::format("Vulkan normalize+quantize/download failed (error {})", submit_ret)});
            }
            std::memcpy(output_data, static_cast<const uint8_t*>(u8_scratch.data),
                        out_total);
        }

        // ---- TEMP PERF INSTRUMENTATION print (crop-u8 hot path) ----
        if (cs_perf2) {
            double p2_total_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - p2_t0).count();
            LOG_INFO("Engine",
                "[PERF] u8 crop pipeline: tiles={} submits={} total={:.1f}ms record={:.1f}ms submit={:.1f}ms "
                "(submit={:.0f}% of total, {:.3f}ms/submit, record={:.0f}%)",
                p2_tiles, p2_submits, p2_total_ms, p2_record_ms, p2_submit_ms,
                p2_total_ms > 0 ? 100.0 * p2_submit_ms / p2_total_ms : 0.0,
                p2_submits > 0 ? p2_submit_ms / p2_submits : 0.0,
                p2_total_ms > 0 ? 100.0 * p2_record_ms / p2_total_ms : 0.0);
        }
        // ---- END TEMP ----

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
        std::string stream = std::format(
            "stage={}, initialized={}, model_loaded={}, gpu_layout_ready={}, gpu_u8_ready={},"
            " gpu_crop_ready={}, gpu_crop_u8_ready={}, gpu_tile_ready={}, pre_u8={}, post_u8={},"
            " quant_u8={}, fp16_packed={}, fp16_storage={}, fp16_arith={}, int8_storage={}",
            stage, initialized_ ? 1 : 0, model_loaded_ ? 1 : 0,
            gpu_layout_pipeline_ready_ ? 1 : 0, gpu_layout_u8_pipeline_ready_ ? 1 : 0,
            gpu_crop_pipeline_ready_ ? 1 : 0, gpu_crop_u8_pipeline_ready_ ? 1 : 0,
            gpu_tile_pipeline_ready_ ? 1 : 0,
            preproc_hwc_u8_to_chw_ != nullptr ? 1 : 0,
            postproc_chw_to_hwc_u8_ != nullptr ? 1 : 0,
            quantize_hwc_to_u8_ != nullptr ? 1 : 0,
            net_->opt.use_fp16_packed ? 1 : 0, net_->opt.use_fp16_storage ? 1 : 0,
            net_->opt.use_fp16_arithmetic ? 1 : 0, net_->opt.use_int8_storage ? 1 : 0);

        if (vkdev_ != nullptr) {
            stream += std::format(", heap_budget_mb={}", vkdev_->get_heap_budget());
        } else {
            stream += ", heap_budget_mb=-1";
        }

        if (width > 0 && height > 0 && channels > 0) {
            stream += std::format(", input={}x{}x{}, scale={}, output={}x{}",
                                  width, height, channels, scale_factor_,
                                  width * scale_factor_, height * scale_factor_);
        }
        if (submit_budget > 0) {
            stream += std::format(", submit_budget={}", submit_budget);
        }
        if (submit_budget_cap > 0) {
            stream += std::format(", submit_budget_cap={}", submit_budget_cap);
        }
        if (submit_memory_budget_mb > 0) {
            stream += std::format(", submit_mem_budget_mb={}", submit_memory_budget_mb);
        }
        if (avg_tile_pixels >= 0.0) {
            stream += std::format(", avg_tile_pixels={:.6g}", avg_tile_pixels);
        }

        return stream;
    }

    // UPIX_KERNEL_SELFTEST=1: round-trip HWC fp32 -> CHW f16-words -> HWC
    // through the two rewritten layout kernels on deliberately odd dims.
    void NCNNEngineImpl::kernel_selftest() {
        if (vkdev_ == nullptr || preproc_hwc_to_chw_ == nullptr ||
            postproc_chw_to_hwc_ == nullptr) {
            LOG_WARNING("Engine", "SELFTEST pipelines unavailable");
            return;
        }
        constexpr int W = 5, H = 3, C = 3;
        ncnn::VkAllocator* blob = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging = vkdev_->acquire_staging_allocator();
        detail::VkAllocGuard guard{vkdev_, blob, staging};
        ncnn::Option opt = net_->opt;
        opt.blob_vkallocator = blob;
        opt.workspace_vkallocator = blob;
        opt.staging_vkallocator = staging;

        std::vector<float> src(static_cast<size_t>(W) * H * C);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<float>(i % 251) / 251.0f;
        }

        ncnn::VkCompute cmd(vkdev_);
        std::vector<ncnn::VkMat> keep;
        ncnn::VkMat in_hwc;
        stage_raw_input(cmd, staging, src.data(), src.size() * sizeof(float), 4u,
                        opt, in_hwc, keep);

        ncnn::VkMat chw;
        chw.create(W, H, C, 2u, 1, blob);
        std::vector<ncnn::VkMat> pb(2);
        pb[0] = in_hwc;
        pb[1] = chw;
        std::vector<ncnn::vk_constant_type> pc(4);
        pc[0].i = W; pc[1].i = H; pc[2].i = C; pc[3].i = static_cast<int>(chw.cstep);
        ncnn::VkMat pd;
        pd.w = (C * static_cast<int>(chw.cstep) + 1) / 2; pd.h = 1; pd.c = 1;
        cmd.record_pipeline(preproc_hwc_to_chw_.get(), pb, pc, pd);

        ncnn::VkMat out_hwc;
        out_hwc.create(W, H, C, 4u, 1, blob);
        std::vector<ncnn::VkMat> qb(2);
        qb[0] = chw;
        qb[1] = out_hwc;
        ncnn::VkMat qd;
        qd.w = W; qd.h = H; qd.c = C;
        cmd.record_pipeline(postproc_chw_to_hwc_.get(), qb, pc, qd);

        ncnn::Mat hwc_dl;
        cmd.record_clone(out_hwc, hwc_dl, opt);
        ncnn::Mat chw_dl;
        cmd.record_clone(chw, chw_dl, opt);
        const int rc = cmd.submit_and_wait();

        int bad = 0;
        double mx = 0.0;
        if (rc == 0 && hwc_dl.data != nullptr && chw_dl.data != nullptr) {
            const auto* got = static_cast<const float*>(hwc_dl.data);
            for (int i = 0; i < W * H * C; ++i) {
                const double d = std::fabs(static_cast<double>(got[i]) - src[i]);
                if (d > 1.0 / 512.0) {
                    if (bad < 6) {
                        LOG_INFO("Engine", "SELFTEST mismatch i={} got={:.5f} want={:.5f}", i, got[i], src[i]);
                    }
                    ++bad;
                }
                mx = std::max(mx, d);
            }
        } else {
            LOG_WARNING("Engine", "SELFTEST submit rc={} data null", rc);
            bad = -1;
        }
        LOG_INFO("Engine",
                 "SELFTEST roundtrip w={}x{}x{} cstep={} bad={} maxabs={:.5f} => {}",
                 W, H, C, chw.cstep, bad, mx, bad == 0 ? "PASS" : "FAIL");

        // Part 2: crop-u8 kernel with an identity window (mirror border on an
        // exact-size window is the identity map).
        if (preproc_hwc_crop_u8_to_chw_ == nullptr) {
            LOG_WARNING("Engine", "SELFTEST crop-u8 pipeline unavailable");
            return;
        }
        std::vector<uint8_t> src8(static_cast<size_t>(W) * H * C);
        for (size_t i = 0; i < src8.size(); ++i) {
            src8[i] = static_cast<uint8_t>((i * 37) % 256);
        }
        ncnn::VkCompute cmd2(vkdev_);
        std::vector<ncnn::VkMat> keep2;
        ncnn::VkMat u8_full;
        stage_u8_input(cmd2, staging, src8.data(), src8.size(), opt, u8_full, keep2);
        ncnn::VkMat chw2;
        chw2.create(W, H, C, 2u, 1, blob);
        std::vector<ncnn::VkMat> rb(2);
        rb[0] = u8_full;
        rb[1] = chw2;
        std::vector<ncnn::vk_constant_type> rcst(9);
        rcst[0].i = W; rcst[1].i = H; rcst[2].i = C;
        rcst[3].i = 0; rcst[4].i = 0;             // tile_x/y
        rcst[5].i = W; rcst[6].i = H;             // tile_w/h
        rcst[7].i = static_cast<int>(chw2.cstep);
        rcst[8].i = 0;                            // border_replicate → mirror
        ncnn::VkMat rd;
        rd.w = (C * static_cast<int>(chw2.cstep) + 1) / 2; rd.h = 1; rd.c = 1;
        cmd2.record_pipeline(preproc_hwc_crop_u8_to_chw_.get(), rb, rcst, rd);
        ncnn::Mat chw2_dl;
        cmd2.record_clone(chw2, chw2_dl, opt);
        const int rc2 = cmd2.submit_and_wait();
        if (rc2 == 0 && chw2_dl.data != nullptr) {
            const auto* w16dbg = static_cast<const uint16_t*>(chw2_dl.data);
            std::string hex;
            for (int i = 0; i < 8; ++i) {
                hex += std::format("{:04x} ", w16dbg[i]);
            }
            LOG_INFO("Engine", "SELFTEST crop-u8 first halves: {}", hex);
        }
        int bad2 = -2;
        if (rc2 == 0 && chw2_dl.data != nullptr) {
            // create_like may resize to f16 dims — byte layout is identical.
            bad2 = 0;
            const auto* w16 = static_cast<const uint16_t*>(chw2_dl.data);
            for (int c = 0; c < C && bad2 < 6; ++c) {
                for (int off = 0; off < W * H && bad2 < 6; ++off) {
                    const int e = c * static_cast<int>(chw2.cstep) + off;
                    // chw2_dl is an f16-typed Mat: u16 index e IS element e.
                    const uint32_t bits = w16[e];
                    const uint32_t sgn = (bits >> 15) & 1u;
                    const uint32_t exp = (bits >> 10) & 0x1Fu;
                    const uint32_t man = bits & 0x3FFu;
                    float v;
                    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
                    else v = std::ldexp(1.0f + static_cast<float>(man) / 1024.0f,
                                        static_cast<int>(exp) - 15);
                    if (sgn) v = -v;
                    const float want =
                        static_cast<float>(src8[(static_cast<size_t>(off) * C) + c]) / 255.0f;
                    if (std::fabs(v - want) > 2.0f / 255.0f) {
                        LOG_INFO("Engine", "SELFTEST crop-u8 mismatch e={} got={:.5f} want={:.5f} bits={:04x}",
                                 e, v, want, bits);
                        ++bad2;
                    }
                }
            }
        }
        LOG_INFO("Engine", "SELFTEST crop-u8 cstep={} => {}", chw2.cstep,
                 bad2 == 0 ? "PASS" : "FAIL");

        // Part 3: BlendTile lane-read over a synthetic f16 net-output plane.
        if (blend_tile_pipeline_ != nullptr && normalize_quantize_output_pipeline_ != nullptr) {
            constexpr int TW = 5, TH = 3;
            const int cs = static_cast<int>(chw2.cstep);
            std::vector<float> tile_vals(static_cast<size_t>(C) * cs);
            for (int c = 0; c < C; ++c) {
                for (int off = 0; off < TW * TH; ++off) {
                    tile_vals[static_cast<size_t>(c) * cs + off] =
                        static_cast<float>((c * 31 + off * 7) % 200) / 200.0f;
                }
            }
            ncnn::VkCompute cmd3(vkdev_);
            std::vector<ncnn::VkMat> keep3;
            // Stage the f16 tile as raw words (bytes identical to an f16 Mat).
            const size_t half_bytes = tile_vals.size() * 2;
            std::vector<uint8_t> f16bytes(half_bytes);
            for (size_t e = 0; e < tile_vals.size(); ++e) {
                // float -> fp16 nearest (values are small rationals; use float bits trick)
                const float v = tile_vals[e];
                uint16_t h;
                // simple conversion via rounding float bits
                const uint32_t fb = [&] {
                    uint32_t u;
                    std::memcpy(&u, &v, 4);
                    return u;
                }();
                const uint32_t sgn = (fb >> 16) & 0x8000u;
                int32_t ex = static_cast<int32_t>((fb >> 23) & 0xFFu) - 127 + 15;
                uint32_t ma = (fb >> 13) & 0x3FFu;
                if (ex <= 0) { h = 0; } else if (ex >= 31) { h = 0x7C00; }
                else { h = static_cast<uint16_t>(sgn | (static_cast<uint32_t>(ex) << 10) | ma); }
                f16bytes[e * 2] = static_cast<uint8_t>(h & 0xFF);
                f16bytes[e * 2 + 1] = static_cast<uint8_t>(h >> 8);
            }
            ncnn::VkMat tile_f16;
            stage_raw_input(cmd3, staging, f16bytes.data(), half_bytes, 2u,
                            opt, tile_f16, keep3);
            ncnn::VkMat acc, wgt;
            acc.create(TW, TH, C, 4u, 1, blob);
            wgt.create(static_cast<int>(TW * TH), 4u, 1, blob);
            // Zero the accumulators: blend accumulates with +=.
            {
                std::vector<ncnn::VkMat> cb(1);
                cb[0] = acc;
                std::vector<ncnn::vk_constant_type> cc(1);
                cc[0].i = TW * TH * C;
                ncnn::VkMat cd;
                cd.w = TW * TH * C; cd.h = 1; cd.c = 1;
                cmd3.record_pipeline(clear_output_pipeline_.get(), cb, cc, cd);
                cb[0] = wgt;
                cc[0].i = TW * TH;
                cd.w = TW * TH;
                cmd3.record_pipeline(clear_output_pipeline_.get(), cb, cc, cd);
            }
            std::vector<ncnn::VkMat> bb(3);
            bb[0] = tile_f16;
            bb[1] = acc;
            bb[2] = wgt;
            std::vector<ncnn::vk_constant_type> bc(13);
            bc[0].i = TW; bc[1].i = TH; bc[2].i = C;
            bc[3].i = TW; bc[4].i = TH;             // out_w/h
            bc[5].i = 0;  bc[6].i = 0;              // out_x/y
            bc[7].i = 0;                            // overlap
            bc[8].i = TW; bc[9].i = TH;             // src full dims
            bc[10].i = 0; bc[11].i = 0;             // src offsets
            bc[12].i = cs;                          // src_cstep
            ncnn::VkMat bd;
            bd.w = TW; bd.h = TH; bd.c = C;
            cmd3.record_pipeline(blend_tile_pipeline_.get(), bb, bc, bd);

            // Part 4: normalize+quantize over the blended result.
            ncnn::VkMat u8out;
            u8out.create((TW * TH * C + 3) / 4, 4u, 1, blob);
            std::vector<ncnn::VkMat> nb(3);
            nb[0] = acc;
            nb[1] = wgt;
            nb[2] = u8out;
            std::vector<ncnn::vk_constant_type> nc2(2);
            nc2[0].i = TW * TH; nc2[1].i = C;
            ncnn::VkMat nd;
            nd.w = (TW * TH * C + 3) / 4; nd.h = 1; nd.c = 1;
            cmd3.record_pipeline(normalize_quantize_output_pipeline_.get(), nb, nc2, nd);

            ncnn::Mat acc_dl;
            cmd3.record_clone(acc, acc_dl, opt);
            ncnn::Mat u8_dl;
            {
                const int words = (TW * TH * C + 3) / 4;
                u8_dl.create(words, static_cast<size_t>(4), 1);
                cmd3.record_clone(u8out, u8_dl, opt);
            }
            const int rc3 = cmd3.submit_and_wait();

            int badb = -3, badq = -3;
            if (rc3 == 0 && acc_dl.data != nullptr && u8_dl.data != nullptr) {
                badb = 0; badq = 0;
                const auto* gota = static_cast<const float*>(acc_dl.data);
                const auto* got8 = static_cast<const uint8_t*>(u8_dl.data);
                for (int c = 0; c < C && (badb < 6 || badq < 6); ++c) {
                    for (int off = 0; off < TW * TH && (badb < 6 || badq < 6); ++off) {
                        const int di = off * C + c;
                        const float want = tile_vals[static_cast<size_t>(c) * cs + off];
                        if (std::fabs(gota[di] - want) > 1.0f / 512.0f) {
                            LOG_INFO("Engine", "SELFTEST blend mismatch px={} c={} got={:.5f} want={:.5f}",
                                     off, c, gota[di], want);
                            ++badb;
                        }
                        const int eb = c * cs + off; // element index in the source plane
                        (void)eb;
                        const uint8_t wantb = static_cast<uint8_t>(
                            std::lround(std::clamp(want * 255.0f, 0.0f, 255.0f)));
                        if (got8[di] != wantb && badq < 6) {
                            LOG_INFO("Engine", "SELFTEST quant mismatch e={} got={} want={}",
                                     di, got8[di], wantb);
                            ++badq;
                        }
                    }
                }
            }
            LOG_INFO("Engine", "SELFTEST blend => {} quant => {}",
                     badb == 0 ? "PASS" : "FAIL", badq == 0 ? "PASS" : "FAIL");
        }
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
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::HwcToChw,
                                                        detail::kHwcToChwComp, net_->opt,
                                                        spirv_hwc_to_chw)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC->CHW shader");
                    return false;
                }
            }
            if (spirv_chw_to_hwc.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::ChwToHwc,
                                                        detail::kChwToHwcComp, net_->opt,
                                                        spirv_chw_to_hwc)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan CHW->HWC shader");
                    return false;
                }
            }
            if (spirv_hwc_u8_to_chw.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::HwcU8ToChw,
                                                        detail::kHwcU8ToChwComp, net_->opt,
                                                        spirv_hwc_u8_to_chw)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC-u8->CHW shader");
                    u8_shader_ready = false;
                }
            }
            if (spirv_chw_to_hwc_u8.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::ChwToHwcU8,
                                                        detail::kChwToHwcU8Comp, net_->opt,
                                                        spirv_chw_to_hwc_u8)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan CHW->HWC-u8 shader");
                    u8_shader_ready = false;
                }
            }
            if (spirv_hwc_to_u8.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::HwcFloatToU8,
                                                        detail::kHwcFloatToU8Comp, net_->opt,
                                                        spirv_hwc_to_u8)) {
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
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::HwcCropToChw,
                                                        detail::kHwcCropToChwComp, net_->opt,
                                                        spirv_crop)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC crop shader");
                    return false;
                }
            }
            if (spirv_crop_u8.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::HwcCropU8ToChw,
                                                        detail::kHwcCropU8ToChwComp, net_->opt,
                                                        spirv_crop_u8)) {
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
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::Clear, detail::kClearComp,
                                                        net_->opt, spirv_clear)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan clear shader");
                    return false;
                }
            }
            if (spirv_blend.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::BlendTile,
                                                        detail::kBlendTileComp, net_->opt,
                                                        spirv_blend)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan blend shader");
                    return false;
                }
            }
            if (spirv_normalize.empty()) {
                if (!spirv_pack::load_or_compile_custom(detail::CustomShader::NormalizeByWeight,
                                                        detail::kNormalizeByWeightComp, net_->opt,
                                                        spirv_normalize)) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan normalize shader");
                    return false;
                }
            }
            if (spirv_normalize_quantize.empty()) {
                if (!spirv_pack::load_or_compile_custom(
                        detail::CustomShader::NormalizeQuantizeByWeight,
                        detail::kNormalizeQuantizeByWeightComp, net_->opt,
                        spirv_normalize_quantize)) {
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

    namespace {

// Replicate-border pad an HWC interleaved float buffer by `pad` pixels.
void replicate_pad_hwc(const float* src, int w, int h, int c, int pad,
                       std::vector<float>& dst) {
    const int ow = w + 2 * pad;
    const int oh = h + 2 * pad;
    dst.resize(static_cast<size_t>(ow) * oh * c);
    // Gather view over the source in HWC order; the per-pixel channel run
    // stays a memcpy on raw bytes (hot path).
    const auto& src_px = std::mdspan(src, static_cast<size_t>(h),
                                     static_cast<size_t>(w), static_cast<size_t>(c));
    for (int y = 0; y < oh; ++y) {
        const int sy = std::clamp(y - pad, 0, h - 1);
        for (int x = 0; x < ow; ++x) {
            const int sx = std::clamp(x - pad, 0, w - 1);
            std::memcpy(dst.data() + (static_cast<size_t>(y) * ow + x) * c,
                        &src_px[static_cast<size_t>(sy), static_cast<size_t>(sx), 0],
                        static_cast<size_t>(c) * sizeof(float));
        }
    }
}

// Crop a fixed replicate border from an HWC interleaved float buffer.
// (kept for future pre-padding variants that emit oversized frames)
[[maybe_unused]] void crop_hwc_border(const float* src, int w, int h, int c, int pad, float* dst) {
    const int ow = w - 2 * pad;
    const int oh = h - 2 * pad;
    for (int y = 0; y < oh; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * ow * c,
                    src + (static_cast<size_t>(y + pad) * w + pad) * c,
                    static_cast<size_t>(ow) * c * sizeof(float));
    }
}

}  // namespace

auto NCNNEngineImpl::process_vulkan_with_gpu_layout(
    const float* input, float* output, int width, int height,
    int channels, const std::atomic<bool>& cancel_flag)
    -> expected<void, EngineError> {
    // Whole-image device preflight: the widest live blobs are the body
    // features (~64ch fp16 at input resolution, a few of them) plus the
    // fp16 in/out images. On mobile GPUs the driver heap budget reports
    // the whole unified RAM pool, not what is actually free — clamp it to
    // host availability so oversized frames fail here with a clean OOM
    // error (callers fall back) instead of dying mid-graph inside ncnn.
    {
        size_t budget_mb = get_device_memory_budget_mb();
        if (budget_mb > 0) {
            const char* device_name = vkdev_ ? vkdev_->info.device_name() : nullptr;
            const std::string_view name_sv = device_name ? device_name : "";
            const bool mobile = name_sv.find("Adreno") != std::string_view::npos ||
                                name_sv.find("Mali") != std::string_view::npos ||
                                name_sv.find("PowerVR") != std::string_view::npos ||
                                name_sv.find("Imagination") != std::string_view::npos;
            bool clamped = false;
            if (mobile) {
                const size_t host_avail_mb = get_memory_info().second;
                if (host_avail_mb > 0 && host_avail_mb < budget_mb) {
                    budget_mb = host_avail_mb;
                    clamped = true;
                }
            }
            const size_t in_total = static_cast<size_t>(width) * height * channels;
            const size_t out_total = in_total * scale_factor_ * scale_factor_;
            const size_t device_need =
                static_cast<size_t>(width) * height * 64 * 2 * 4 +
                (in_total + out_total) * 2;
            if (device_need > budget_mb * 1024 * 1024) {
                return std::unexpected(EngineError{kErrOutOfMemory,
                    std::format("out of GPU memory for whole-image Vulkan path: need ~{}MB, budget {}MB{}",
                                device_need / (1024 * 1024), budget_mb,
                                clamped ? " (clamped to host availability)" : "")});
            }
        }
    }

    // Pre-padding models: replicate-pad the frame so the model's internal
    // border crops cancel out. The model then emits exactly width*scale for
    // the ORIGINAL frame, straight into the caller's buffer.
    const int pad = model_prepadding_;
    if (pad > 0) {
        std::vector<float> padded;
        replicate_pad_hwc(input, width, height, channels, pad, padded);
        return process_vulkan_with_gpu_layout_run(
            padded.data(), output, width + 2 * pad, height + 2 * pad, channels, cancel_flag);
    }
    return process_vulkan_with_gpu_layout_run(input, output, width, height, channels, cancel_flag);
}

auto NCNNEngineImpl::process_vulkan_with_gpu_layout_run(
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

        // On pre-padded frames (model_prepadding_ > 0) `width`/`height` include
        // the 2*pad margin the model will crop internally, so the emitted frame
        // corresponds to the caller's original (width - 2*pad) * scale.
        const int out_width = (width - 2 * model_prepadding_) * scale_factor_;
        const int out_height = (height - 2 * model_prepadding_) * scale_factor_;
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

        std::vector<ncnn::VkMat> float_upload_staging;
        ncnn::VkMat in_gpu_hwc;
        if (!stage_raw_input(cmd, staging_vkallocator,
                             input, in_total * sizeof(float), 4u,
                             opt, in_gpu_hwc, float_upload_staging)) {
            return std::unexpected(EngineError{14, "Vulkan input upload failed (staging)"});
        }

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
        pre_dispatcher.w = (channels * static_cast<int>(in_gpu_chw.cstep) + 1) / 2;
        pre_dispatcher.h = 1;
        pre_dispatcher.c = 1;
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
            return std::unexpected(EngineError{14, std::format("Failed to set Vulkan input (error {})", ret)});
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
                    return std::unexpected(EngineError{15, std::format("Failed to extract Vulkan output (error {})", ret)});
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
            if (auto oom = empty_gpu_output_error(out_gpu.w, out_gpu.h, out_gpu.c)) {
                return std::unexpected(*oom);
            }
            return std::unexpected(EngineError{16, std::format("Vulkan output shape mismatch (got {}x{}x{}, want {}x{}x{})", out_gpu.w, out_gpu.h, out_gpu.c, out_width, out_height, channels)});
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
            return std::unexpected(EngineError{17, std::format("Vulkan submit failed (error {})", ret)});
        }

        return {};
    }

} // namespace upix
