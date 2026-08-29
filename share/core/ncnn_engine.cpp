#include "engine.hpp"
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
#include "ncnn_engine_impl.hpp"
#if NCNN_VULKAN
#include "vulkan_shaders.hpp"
#include "gpu_precision.hpp"
#include "spirv_pack.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>

// Platform-specific memory detection
#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace upix {

#if NCNN_VULKAN
namespace {

// g_vulkan_init_mutex moved to upix::detail (defined in
// ncnn_gpu_init.cpp); declared extern in ncnn_engine_impl.hpp.
std::mutex g_vulkan_model_load_mutex;

} // namespace
#endif

// Class declaration lives in ncnn_engine_impl.hpp; only out-of-line member
// function definitions below.

NCNNEngineImpl::NCNNEngineImpl() : net_(std::make_unique<ncnn::Net>()) {}
NCNNEngineImpl::~NCNNEngineImpl() { unload_model(); }

    auto NCNNEngineImpl::initialize(DeviceType preferred)
        -> expected<void, EngineError> {
        
        if (preferred == DeviceType::GPU_VULKAN) {
            if (try_init_vulkan()) {
                current_device_ = DeviceType::GPU_VULKAN;
                initialized_ = true;
                LOG_INFO("Engine", "Initialized with Vulkan GPU support");
                return {};
            }
            LOG_WARNING("Engine", "Vulkan not available, falling back to CPU");
        }
        
        current_device_ = DeviceType::CPU;
        initialized_ = true;
        LOG_INFO("Engine", "Initialized with CPU");
        return {};
    }
    
    auto NCNNEngineImpl::list_devices() -> std::vector<DeviceCapability> {
        std::vector<DeviceCapability> devices;
        
        DeviceCapability cpu;
        cpu.type = DeviceType::CPU;
        cpu.name = "CPU";
        cpu.compute_units = std::thread::hardware_concurrency();
        cpu.memory_mb = 0;
        cpu.benchmark_score = 1.0f;
        devices.push_back(cpu);
        
#if NCNN_VULKAN
        int gpu_count = ncnn::get_gpu_count();
        for (int i = 0; i < gpu_count; ++i) {
            DeviceCapability gpu;
            gpu.type = DeviceType::GPU_VULKAN;
            gpu.name = std::format("Vulkan GPU-{}", i);
            gpu.compute_units = 1;
            gpu.memory_mb = 0;
            gpu.benchmark_score = 10.0f;
            devices.push_back(gpu);
        }
#endif
        
        return devices;
    }
    
    auto NCNNEngineImpl::load_model(const std::string& param_path, const std::string& bin_path)
        -> expected<void, EngineError> {
        
        if (!initialized_) {
            return std::unexpected(EngineError{1, "Engine not initialized"});
        }
        
        LOG_INFO("Engine", "Loading model: {}", param_path);
        
        model_loaded_ = false;
        
        // Set Vulkan device if using GPU
#if NCNN_VULKAN
        // fp16 is a ~2x throughput win on most mobile/integrated GPUs and the
        // quality impact is negligible for SR. Opt-out via UPIX_GPU_DISABLE_FP16=1.
        static const bool disable_gpu_fp16 = []() {
            const char* env_value = std::getenv("UPIX_GPU_DISABLE_FP16");
            return env_value != nullptr && std::string(env_value) == "1";
        }();
        // int8 storage enables the zero-copy u8 tile queue pipeline (no CPU float
        // conversion, deeper Vulkan submit batching). Disabled previously for some
        // mobile drivers; keep an escape hatch so a misbehaving device can opt out.
        // The neural network still runs in fp32; int8 only affects the image
        // storage/transfer layer. Output differs from the fp32 path only by u8
        // quantization rounding (measured PSNR ~49 dB, mean abs diff 0.55/255 —
        // visually lossless, but not bit-identical).
        static const bool disable_gpu_int8 = []() {
            const char* env_value = std::getenv("UPIX_GPU_DISABLE_INT8");
            return env_value != nullptr && std::string(env_value) == "1";
        }();
        std::unique_lock<std::mutex> gpu_model_load_lock;
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr) {
            gpu_model_load_lock = std::unique_lock<std::mutex>(g_vulkan_model_load_mutex);
            LOG_INFO("Engine", "Serializing Vulkan model load to avoid concurrent queue submits");
            spirv_pack::register_ncnn_lookup();
            net_->set_vulkan_device(vkdev_);
            net_->opt.use_vulkan_compute = true;
            const auto& gpu_info = vkdev_->info;

            static const bool force_fp16_arithmetic = []() {
                const char* v = std::getenv("UPIX_GPU_ENABLE_FP16_ARITH");
                return v != nullptr && std::string(v) == "1";
            }();

            auto fallback_cpu = [&](const char* why) {
                LOG_WARNING("Engine", "{}, falling back to CPU", why);
                net_->opt.use_vulkan_compute = false;
                net_->opt.use_fp16_packed = false;
                net_->opt.use_fp16_storage = false;
                net_->opt.use_fp16_arithmetic = false;
                net_->opt.use_int8_storage = false;
                current_device_ = DeviceType::CPU;
                if (inference_threads_ > 0) {
                    net_->opt.num_threads = inference_threads_;
                }
            };

            if (disable_gpu_fp16) {
                fallback_cpu("UPIX_GPU_DISABLE_FP16=1: frozen SPIR-V pack requires fp16 storage");
            } else {
                if (disable_gpu_int8) {
                    LOG_WARNING("Engine",
                                "UPIX_GPU_DISABLE_INT8=1 ignored: frozen SPIR-V pack includes int8 storage");
                }
                const auto caps = gpu_feature_caps(gpu_info);
                const auto profile = select_profile(caps, force_fp16_arithmetic);
                if (!profile) {
                    fallback_cpu("GPU lacks frozen SPIR-V pack features");
                } else {
                    apply_profile(net_->opt, *profile);
                    unsigned int hw_threads = std::thread::hardware_concurrency();
                    int tuned_threads = 2;
                    if (hw_threads > 0) {
                        tuned_threads = std::clamp(static_cast<int>(hw_threads / 4), 1, 4);
                    }
                    net_->opt.num_threads = tuned_threads;
                    LOG_INFO(
                        "Engine",
                        "Vulkan precision profile={} key=0x{:x} fp16_packed={:d} "
                        "fp16_storage={:d} fp16_arithmetic={:d} int8_storage={:d} "
                        "local_memory={:d}",
                        profile_name(*profile), option_key(net_->opt),
                        net_->opt.use_fp16_packed,
                        net_->opt.use_fp16_storage,
                        net_->opt.use_fp16_arithmetic,
                        net_->opt.use_int8_storage,
                        net_->opt.use_shader_local_memory);
                }
            }
        } else {
            net_->opt.use_vulkan_compute = false;
            net_->opt.use_fp16_packed = false;
            net_->opt.use_fp16_storage = false;
            net_->opt.use_fp16_arithmetic = false;
            net_->opt.use_int8_storage = false;
            // CPU engines created by EnginePool divide the machine's physical
            // cores across sibling engines; standalone engines keep ncnn's
            // default (one thread per logical CPU).
            if (inference_threads_ > 0) {
                net_->opt.num_threads = inference_threads_;
            }
        }
#else
        net_->opt.use_vulkan_compute = false;
        net_->opt.use_fp16_packed = false;
        net_->opt.use_fp16_storage = false;
        net_->opt.use_fp16_arithmetic = false;
        net_->opt.use_int8_storage = false;
        if (inference_threads_ > 0) {
            net_->opt.num_threads = inference_threads_;
        }
#endif
        
#if NCNN_VULKAN
        const uint32_t frozen_key = option_key(net_->opt);
        const bool expect_frozen_key = current_device_ == DeviceType::GPU_VULKAN &&
                                       net_->opt.use_vulkan_compute;
#endif
        // Load param file
        int ret = net_->load_param(param_path.c_str());
        if (ret != 0) {
            return std::unexpected(EngineError{3, std::format("Failed to load param file: {} (error {})", param_path, ret)});
        }
#if NCNN_VULKAN
        if (expect_frozen_key && option_key(net_->opt) != frozen_key) {
            return std::unexpected(EngineError{
                3,
                std::format("GPU Option sanitized away from frozen SPIR-V pack (0x{:x} -> 0x{:x})",
                            frozen_key, option_key(net_->opt))
            });
        }
#endif
        
        // Load model weights
        ret = net_->load_model(bin_path.c_str());
        if (ret != 0) {
            if (current_device_ == DeviceType::GPU_VULKAN && ret == -4) {
                const std::string detail = format_vulkan_runtime_snapshot("load_model_device_lost");
                LOG_ERROR("Engine", "Vulkan device lost during net_->load_model: {}", detail);
                return std::unexpected(EngineError{
                    4,
                    std::format("Failed to load model weights (VK_ERROR_DEVICE_LOST): {} (error {}); {}",
                                bin_path, ret, detail)
                });
            }
            return std::unexpected(EngineError{4, std::format("Failed to load model weights: {} (error {})", bin_path, ret)});
        }

#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr) {
            gpu_layout_pipeline_ready_ = init_gpu_layout_pipelines();
            if (!gpu_layout_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU pre/post layout pipeline disabled, fallback to CPU layout conversion");
            }
            gpu_layout_u8_pipeline_ready_ = net_->opt.use_int8_storage &&
                                            preproc_hwc_u8_to_chw_ != nullptr &&
                                            postproc_chw_to_hwc_u8_ != nullptr &&
                                            quantize_hwc_to_u8_ != nullptr;
            if (!gpu_layout_u8_pipeline_ready_) {
                if (net_->opt.use_int8_storage) {
                    LOG_WARNING("Engine", "GPU u8 pre/post layout pipeline disabled, fallback to CPU u8 conversion");
                } else {
                    LOG_INFO("Engine", "GPU u8 pre/post layout pipeline disabled (int8_storage off; unset UPIX_GPU_DISABLE_INT8 to enable the zero-copy u8 queue)");
                }
                LOG_WARNING("Engine", "GPU u8 pipeline readiness details: {}",
                            format_vulkan_runtime_snapshot("load_model_gpu_u8_not_ready"));
            }
            gpu_crop_pipeline_ready_ = init_gpu_crop_pipeline();
            if (!gpu_crop_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU crop pipeline disabled, fallback to CPU tile extraction");
            }
            gpu_crop_u8_pipeline_ready_ = net_->opt.use_int8_storage &&
                                          preproc_hwc_crop_u8_to_chw_ != nullptr;
            if (!gpu_crop_u8_pipeline_ready_) {
                if (net_->opt.use_int8_storage) {
                    LOG_WARNING("Engine", "GPU u8 crop pipeline disabled, fallback to CPU tile extraction");
                } else {
                    LOG_INFO("Engine", "GPU u8 crop pipeline disabled (int8_storage off)");
                }
            }
            gpu_tile_pipeline_ready_ = init_gpu_tile_pipelines();
            if (!gpu_tile_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU tile blend pipeline disabled, fallback to CPU tile blending");
            }
            LOG_INFO("Engine", "GPU pipeline readiness snapshot: {}",
                     format_vulkan_runtime_snapshot("load_model_after_pipeline_init"));
            if (std::getenv("UPIX_KERNEL_SELFTEST") != nullptr) {
                kernel_selftest();
            }
        } else {
            gpu_layout_pipeline_ready_ = false;
            gpu_layout_u8_pipeline_ready_ = false;
            gpu_crop_pipeline_ready_ = false;
            gpu_crop_u8_pipeline_ready_ = false;
            gpu_tile_pipeline_ready_ = false;
        }
#endif
        
        // Detect model info from param file
        detect_model_info(param_path);
        
        model_name_ = std::filesystem::path(param_path).stem().string();
        model_loaded_ = true;
        
        LOG_INFO("Engine", "Model loaded successfully (scale: {})", scale_factor_);
        return {};
    }
    
    auto NCNNEngineImpl::process(const float* input, float* output, int width, int height, int channels,
                 const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> {
        
        if (!initialized_) {
            return std::unexpected(EngineError{1, "Engine not initialized"});
        }
        if (!model_loaded_) {
            return std::unexpected(EngineError{2, "No model loaded"});
        }
        
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN) {
            if (vkdev_ == nullptr || !gpu_layout_pipeline_ready_) {
                return std::unexpected(EngineError{21, "GPU layout pipeline is not ready"});
            }

            auto vk_result = process_vulkan_with_gpu_layout(input, output, width, height, channels, cancel_flag);
            if (vk_result.has_value()) {
                return {};
            }
            if (!is_oom_error(vk_result.error())) {
                return std::unexpected(EngineError{22, "Vulkan layout pipeline failed: " + vk_result.error().message});
            }
            // Device memory refused (preflight or mid-run): fall through to the
            // CPU section below — no device memory needed, same pre-padding
            // semantics (replicate border), so tile callers keep working.
            LOG_WARNING("Engine", vk_result.error().message +
                ", falling back to CPU path");
        }
#endif
        
        if (input == nullptr || output == nullptr) {
            return std::unexpected(EngineError{18, "Null input/output buffer"});
        }
        if (width <= 0 || height <= 0 || channels <= 0) {
            return std::unexpected(EngineError{11, "Invalid input dimensions"});
        }

        const int expected_out_width = width * scale_factor_;
        const int expected_out_height = height * scale_factor_;
        
        // Create input Mat from float array (interleaved RGB -> planar)
        // ncnn expects planar format: RRR...GGG...BBB...
        ncnn::Mat in(width, height, channels);
        if (in.empty()) {
            return std::unexpected(EngineError{5, "Failed to create input Mat"});
        }
        
        // Convert interleaved float RGB to planar format
        for (int c = 0; c < channels; ++c) {
            float* in_channel = in.channel(c);
            for (int h = 0; h < height; ++h) {
                for (int w = 0; w < width; ++w) {
                    size_t src_idx = (static_cast<size_t>(h) * width + w) * channels + c;
                    size_t dst_idx = static_cast<size_t>(h) * width + w;
                    in_channel[dst_idx] = input[src_idx];
                }
            }
        }

        // Pre-padding models (Real-CUGAN "se"): feed a replicate-border padded
        // frame so the model's internal crops cancel out and output stays
        // exactly width*scale x height*scale. ncnn's CPU layers silently emit
        // garbage (or corrupt the heap) for this family at certain net input
        // WIDTH classes while the GPU path is unaffected everywhere — verified
        // by tile-probe sweeps: x2 needs an even net width, x3 needs
        // net % 4 == 0. Extend the replicate pad on the right into the
        // verified-safe class and crop the extra columns off below.
        ncnn::Mat in_feed;
        int pad_right = model_prepadding_;
        if (model_prepadding_ > 0) {
            const int net_w = width + 2 * model_prepadding_;
            int extra = 0;
            if (scale_factor_ == 3) {
                extra = (4 - net_w % 4) % 4;
            } else {
                extra = net_w % 2;
            }
            pad_right += extra;
            ncnn::copy_make_border(in, in_feed,
                model_prepadding_, model_prepadding_, model_prepadding_, pad_right,
                ncnn::BORDER_REPLICATE, 0.f);
        }
        
        // Create extractor
        ncnn::Extractor ex = net_->create_extractor();
        ex.set_light_mode(true);
        
        // Determine input and output using indices when available (more robust)
        int input_idx = -1;
        int output_idx = -1;
        if (!net_->input_indexes().empty()) {
            input_idx = net_->input_indexes()[0];
        }
        if (!net_->output_indexes().empty()) {
            output_idx = net_->output_indexes()[0];
        }
        
        // Set input
        int ret;
        const ncnn::Mat& feed = (model_prepadding_ > 0 && !in_feed.empty()) ? in_feed : in;
        if (input_idx >= 0) {
            ret = ex.input(input_idx, feed);
        } else {
            // Fallback to common name
            ret = ex.input("data", feed);
        }
        if (ret != 0) {
            return std::unexpected(EngineError{6, std::format("Failed to set input (error {})", ret)});
        }
        
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }
        
        // Extract output
        ncnn::Mat out;
        if (output_idx >= 0) {
            ret = ex.extract(output_idx, out);
        } else {
            // Fallback to common names
            ret = ex.extract("output", out);
            if (ret != 0) {
                ret = ex.extract("out", out);
                if (ret != 0) {
                    return std::unexpected(EngineError{7, std::format("Failed to extract output (error {})", ret)});
                }
            }
        }
        
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }
        
        // Pre-padding runs may emit up to (align-1)*scale extra columns/rows
        // from the alignment extension; the valid frame is the top-left one.
        if (out.c != channels || out.w < expected_out_width || out.h < expected_out_height) {
            return std::unexpected(EngineError{
                16,
                std::format("Output shape mismatch: expected={}x{}x{}, actual={}x{}x{}",
                            expected_out_width, expected_out_height, channels,
                            out.w, out.h, out.c)
            });
        }

        const int out_width = expected_out_width;
        const int out_height = expected_out_height;

        for (int c = 0; c < channels; ++c) {
            const float* out_channel = out.channel(c);
            for (int h = 0; h < out_height; ++h) {
                for (int w = 0; w < out_width; ++w) {
                    size_t dst_idx = (static_cast<size_t>(h) * out_width + w) * channels + c;
                    size_t src_idx = static_cast<size_t>(h) * out.w + w;
                    output[dst_idx] = out_channel[src_idx];
                }
            }
        }
        
        return {};
    }

namespace {

// TileConfig defaults + UPIX_*_TILE_* env overrides, shared by the u8
// GPU queue pipeline, the float Vulkan fallback, and the CPU path. The three
// call sites differ in defaults, clamp bounds, and which env vars they honor
// (the CPU path reads only the tile size), so those are passed in. A null
// overlap/batch env name means that variable is not honored.
TileConfig tile_config_from_env(
    int default_tile_size,
    int default_overlap,
    int default_batch_size,
    const char* tile_size_env,
    const char* tile_overlap_env,
    const char* tile_batch_env,
    int tile_size_lo,
    int tile_size_hi,
    int batch_size_hi) {
    TileConfig tile_config;
    tile_config.tile_size = default_tile_size;
    tile_config.overlap = default_overlap;
    tile_config.batch_size = default_batch_size;
    if (const char* tile_size_env_value = std::getenv(tile_size_env)) {
        try { tile_config.tile_size = std::clamp(std::stoi(tile_size_env_value), tile_size_lo, tile_size_hi); } catch (...) {}
    }
    if (tile_overlap_env != nullptr) {
        if (const char* tile_overlap_env_value = std::getenv(tile_overlap_env)) {
            try { tile_config.overlap = std::clamp(std::stoi(tile_overlap_env_value), 0, std::max(1, tile_config.tile_size / 2)); } catch (...) {}
        }
    }
    if (tile_batch_env != nullptr) {
        if (const char* tile_batch_env_value = std::getenv(tile_batch_env)) {
            try { tile_config.batch_size = std::clamp(std::stoi(tile_batch_env_value), 1, batch_size_hi); } catch (...) {}
        }
    }
    return tile_config;
}

// Whole-buffer u8 → float ([0,1]) conversion.
std::vector<float> u8_to_float_buffer(const uint8_t* input, size_t count) {
    std::vector<float> output_float(count);
    constexpr float kInv255 = 1.0f / 255.0f;
    for (size_t i = 0; i < count; ++i) {
        output_float[i] = static_cast<float>(input[i]) * kInv255;
    }
    return output_float;
}

// Whole-buffer float ([0,1]) → u8 conversion with non-finite rejection.
auto float_to_u8_checked(const float* input, uint8_t* output, size_t count)
    -> expected<void, EngineError> {
    for (size_t i = 0; i < count; ++i) {
        const float value = input[i];
        if (!std::isfinite(value)) {
            return std::unexpected(EngineError{20, "Non-finite output value detected"});
        }
        const float v = std::clamp(value * 255.0f, 0.0f, 255.0f);
        output[i] = static_cast<uint8_t>(std::lround(v));
    }
    return {};
}

// Host RAM preflight for full-image float buffers; refuses with a clean OOM
// error instead of throwing std::bad_alloc mid-run.
auto check_host_memory(size_t need_bytes, const std::pair<size_t, size_t>& mem_info)
    -> std::optional<EngineError> {
    if (mem_info.second == 0) return std::nullopt;  // unknown → let it throw/succeed
    if (need_bytes <= mem_info.second * 1024 * 1024) return std::nullopt;
    return EngineError{kErrOutOfMemory,
        std::format("out of host memory: need ~{}MB, available {}MB",
                    need_bytes / (1024 * 1024), mem_info.second)};
}

} // namespace

    auto NCNNEngineImpl::process_u8(const uint8_t* input, uint8_t* output, int width, int height, int channels,
                    const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> {
        if (!initialized_) {
            return std::unexpected(EngineError{1, "Engine not initialized"});
        }
        if (!model_loaded_) {
            return std::unexpected(EngineError{2, "No model loaded"});
        }
        if (input == nullptr || output == nullptr) {
            return std::unexpected(EngineError{18, "Null input/output buffer"});
        }
        if (cancel_flag) {
            return std::unexpected(EngineError{3, "Processing cancelled"});
        }

#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN) {
            if (vkdev_ == nullptr) {
                const std::string detail = format_vulkan_runtime_snapshot("process_u8_device_not_ready", width, height, channels);
                return std::unexpected(EngineError{23, "GPU device not ready; " + detail});            }

            // Use the most efficient U8 queue pipeline if available
            if (supports_gpu_tile_pipeline_u8_queue()) {
                LOG_INFO("Engine", "Processing u8 input through optimized Vulkan U8 queue pipeline");

                TileConfig tile_config = tile_config_from_env(
                    get_recommended_tile_size(), 16, 4,
                    "UPIX_GPU_TILE_SIZE", "UPIX_GPU_TILE_OVERLAP", "UPIX_GPU_TILE_BATCH",
                    64, 2048, 32);

                std::vector<TileRegion> regions;
                const int step = std::max(1, tile_config.tile_size - tile_config.overlap);
                for (int y = 0; y < height; y += step) {
                    int tile_h = std::min(tile_config.tile_size, height - y);
                    for (int x = 0; x < width; x += step) {
                        int tile_w = std::min(tile_config.tile_size, width - x);
                        TileRegion region;
                        region.x = static_cast<uint32_t>(x);
                        region.y = static_cast<uint32_t>(y);
                        region.width = static_cast<uint32_t>(tile_w);
                        region.height = static_cast<uint32_t>(tile_h);
                        region.overlap = static_cast<uint32_t>(tile_config.overlap);
                        regions.push_back(region);
                    }
                }

                GPUTileTaskU8 task;
                task.input_data = input;
                task.output_data = output;
                task.input_width = width;
                task.input_height = height;
                task.channels = channels;
                task.regions = &regions;
                task.config = tile_config;

                auto u8_result = process_tiles_gpu_u8_queue({task}, cancel_flag);
                if (!u8_result.has_value()) {
                    if (!is_oom_error(u8_result.error())) {
                        return u8_result;
                    }
                    // OOM in the queue pipeline (full-output buffers): the
                    // float pipeline needs slightly less device memory, and
                    // the CPU tile path after it needs none at all.
                    LOG_WARNING("Engine", u8_result.error().message +
                        ", falling back to float tile pipeline");
                } else {

                    // Adreno/Mali drivers with int8_storage + fp16_arithmetic have been
                    // observed to submit successfully but produce all-zero output on some
                    // shapes. Detect that and fall through to the float tile pipeline
                    // rather than handing back a black image.
                    const size_t out_bytes = static_cast<size_t>(width) * height *
                                             scale_factor_ * scale_factor_ * channels;
                    bool all_zero = true;
                    for (size_t i = 0; i < out_bytes; i += 4096) {
                        if (output[i] != 0) {
                            all_zero = false;
                            break;
                        }
                    }
                    if (all_zero && out_bytes > 0 && output[out_bytes - 1] == 0) {
                        LOG_WARNING("Engine",
                            "U8 queue pipeline produced all-zero output (bytes={}), falling back to float tile pipeline",
                            out_bytes);
                    } else {
                        return u8_result;
                    }
                }
            }

            bool fell_back_to_cpu = false;
            // supports_gpu_tile_pipeline() (not just gpu_tile_pipeline_ready_):
            // Crop-layer models without a known prepadding margin must skip
            // both tile pipelines and would fail with error 201 the moment
            // process_tiles_gpu re-checks the same gate.
            if (supports_gpu_tile_pipeline()) {
                LOG_INFO("Engine", "Processing u8 input through Vulkan float tile pipeline (fallback)");

                const size_t in_total = static_cast<size_t>(width) * height * channels;
                const int out_width = width * scale_factor_;
                const int out_height = height * scale_factor_;
                const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;

                if (auto oom = check_host_memory((in_total + out_total) * sizeof(float), get_memory_info())) {
                    return std::unexpected(*oom);
                }
                std::vector<float> input_float = u8_to_float_buffer(input, in_total);

                std::vector<float> output_float(out_total, 0.0f);

                TileConfig tile_config = tile_config_from_env(
                    256, 16, 2,
                    "UPIX_GPU_TILE_SIZE", "UPIX_GPU_TILE_OVERLAP", "UPIX_GPU_TILE_BATCH",
                    64, 1024, 8);

                std::vector<TileRegion> regions;
                const int effective_tile_size = std::max(1, tile_config.tile_size);
                const int effective_overlap = std::clamp(tile_config.overlap, 0, effective_tile_size - 1);
                const int step = std::max(1, effective_tile_size - effective_overlap);
                for (int y = 0; y < height; y += step) {
                    int tile_y = y;
                    if (tile_y + effective_tile_size >= height) {
                        tile_y = std::max(0, height - effective_tile_size);
                    }
                    bool reached_bottom = false;
                    for (int x = 0; x < width; x += step) {
                        int tile_x = x;
                        if (tile_x + effective_tile_size >= width) {
                            tile_x = std::max(0, width - effective_tile_size);
                        }
                        TileRegion region;
                        region.x = static_cast<uint32_t>(tile_x);
                        region.y = static_cast<uint32_t>(tile_y);
                        region.width = static_cast<uint32_t>(std::min(effective_tile_size, width - tile_x));
                        region.height = static_cast<uint32_t>(std::min(effective_tile_size, height - tile_y));
                        region.overlap = static_cast<uint32_t>(effective_overlap);
                        regions.push_back(region);

                        if (tile_x + effective_tile_size >= width) {
                            break;
                        }
                    }
                    if (tile_y + effective_tile_size >= height) {
                        reached_bottom = true;
                    }
                    if (reached_bottom) {
                        break;
                    }
                }

                auto tile_result = process_tiles_gpu(
                    input_float.data(),
                    output_float.data(),
                    width,
                    height,
                    channels,
                    regions,
                    tile_config,
                    {},
                    cancel_flag
                );
                if (!tile_result.has_value()) {
                    if (!is_oom_error(tile_result.error())) {
                        return std::unexpected(EngineError{24, "Vulkan float tile pipeline failed: " + tile_result.error().message});
                    }
                    // GPU full-output buffers still do not fit — skip the
                    // whole-image compatibility path (it needs even more)
                    // and continue to the CPU tile path below.
                    LOG_WARNING("Engine", tile_result.error().message +
                        ", falling back to CPU tile pipeline");
                    fell_back_to_cpu = true;
                } else {
                    return float_to_u8_checked(output_float.data(), output, out_total);
                }
            }

            const size_t compat_in_total = static_cast<size_t>(width) * height * channels;
            const int compat_out_width = width * scale_factor_;
            const int compat_out_height = height * scale_factor_;
            const size_t compat_out_total =
                static_cast<size_t>(compat_out_width) * compat_out_height * channels;

            if (!fell_back_to_cpu) {
                // Whole-image host staging: the CPU path below allocates the
                // same float buffers, so a shortfall here is terminal —
                // refuse cleanly instead of throwing mid-run. Every sibling
                // path preflights this; the compat path never did. The
                // device budget is preflighted at the entry of
                // process_vulkan_with_gpu_layout; the OOM error it returns
                // is handled below.
                if (auto oom = check_host_memory(
                        (compat_in_total + compat_out_total) * sizeof(float), get_memory_info())) {
                    return std::unexpected(*oom);
                }
            }

            if (!fell_back_to_cpu) {
                LOG_WARNING("Engine", "Vulkan tile pipeline unavailable, switching to non-tile Vulkan float compatibility path");

                std::vector<float> input_float = u8_to_float_buffer(input, compat_in_total);

                std::vector<float> output_float(compat_out_total);
                auto vk_result = process_vulkan_with_gpu_layout(
                    input_float.data(),
                    output_float.data(),
                    width,
                    height,
                    channels,
                    cancel_flag
                );
                if (vk_result.has_value()) {
                    return float_to_u8_checked(output_float.data(), output, compat_out_total);
                }
                if (!is_oom_error(vk_result.error())) {
                    return std::unexpected(EngineError{24, "Vulkan float compatibility path failed: " + vk_result.error().message});
                }
                // Device memory shortfall: the CPU tile path below needs no
                // device memory at all (its host staging already passed the
                // preflight above).
                LOG_WARNING("Engine", vk_result.error().message +
                    ", falling back to CPU tile pipeline");
            }
        }
#endif
        // CPU path: tile the image to avoid OOM on large photos.
        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;
        const size_t in_total = static_cast<size_t>(width) * height * channels;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        if (auto oom = check_host_memory((in_total + out_total) * sizeof(float), get_memory_info())) {
            return std::unexpected(*oom);
        }
        std::vector<float> input_float = u8_to_float_buffer(input, in_total);
        std::vector<float> output_float(out_total);

        TileConfig tile_config = tile_config_from_env(
            get_recommended_tile_size(), 16, 1,
            "UPIX_CPU_TILE_SIZE", nullptr, nullptr,
            32, 1024, 1);

        TileProcessor tp;
        auto tile_result = tp.process_image(
            input_float.data(), output_float.data(),
            width, height, channels,
            *this, tile_config, {}, cancel_flag
        );
        if (!tile_result.has_value()) {
            return std::unexpected(EngineError{25, "CPU tile pipeline failed: " + tile_result.error().message});
        }

        return float_to_u8_checked(output_float.data(), output, out_total);
    }

    auto NCNNEngineImpl::debug_blob_scan(const uint8_t* input, int width, int height, int channels)
        -> std::string {
        if (!initialized_ || !model_loaded_) {
            return "blobscan: engine or model not ready";
        }
        if (input == nullptr || width <= 0 || height <= 0 || channels <= 0) {
            return "blobscan: invalid input";
        }

        // Interleaved u8 -> planar float Mat, plus the same replicate-border
        // pre-padding process() applies, so blob shapes match production runs.
        const size_t in_total = static_cast<size_t>(width) * height * channels;
        const auto input_float = u8_to_float_buffer(input, in_total);
        ncnn::Mat in(width, height, channels);
        for (int c = 0; c < channels; ++c) {
            float* in_channel = in.channel(c);
            for (int h = 0; h < height; ++h) {
                for (int w = 0; w < width; ++w) {
                    const size_t src_idx = (static_cast<size_t>(h) * width + w) * channels + c;
                    in_channel[static_cast<size_t>(h) * width + w] = input_float[src_idx];
                }
            }
        }
        ncnn::Mat feed = in;
        if (model_prepadding_ > 0) {
            const int net_w = width + 2 * model_prepadding_;
            const int extra = (scale_factor_ == 3) ? (4 - net_w % 4) % 4 : net_w % 2;
            ncnn::Mat bordered;
            ncnn::copy_make_border(in, bordered, model_prepadding_, model_prepadding_,
                                   model_prepadding_, model_prepadding_ + extra,
                                   ncnn::BORDER_REPLICATE, 0.f);
            feed = bordered;
        }

        // One extractor reused across blobs: ncnn caches each blob's result
        // within an extractor, so this is one inference pass, not N.
        ncnn::Extractor ex = net_->create_extractor();
        ex.set_light_mode(true);
        const int input_idx = net_->input_indexes().empty() ? -1 : net_->input_indexes()[0];
        const int set_ret = input_idx >= 0 ? ex.input(input_idx, feed) : ex.input("data", feed);
        if (set_ret != 0) {
            return "blobscan: failed to set input";
        }

        std::string report;
        bool first_bad_logged = false;
        for (const auto& blob : net_->blobs()) {
            ncnn::Mat m;
            const int ret = ex.extract(blob.name.c_str(), m);
            if (ret != 0 || m.empty()) {
                const auto line = std::format("{} extract_failed({})", blob.name, ret);
                LOG_WARNING("Engine", "BLOBSCAN {}", line);
                report += line + "\n";
                continue;
            }
            double mn = std::numeric_limits<double>::infinity();
            double mx = -std::numeric_limits<double>::infinity();
            double sum = 0;
            size_t count = 0;
            size_t nonfinite = 0;
            for (int c = 0; c < m.c; ++c) {
                const float* p = m.channel(c);
                const size_t n = static_cast<size_t>(m.w) * m.h;
                for (size_t i = 0; i < n; ++i) {
                    const float v = p[i];
                    if (!std::isfinite(v)) {
                        nonfinite++;
                        continue;
                    }
                    mn = std::min<double>(mn, v);
                    mx = std::max<double>(mx, v);
                    sum += v;
                    count++;
                }
            }
            const double mean = count != 0 ? sum / static_cast<double>(count) : 0.0;
            auto line = std::format("{} {}x{}x{} min={:.4f} max={:.4f} mean={:.4f}",
                                    blob.name, m.c, m.w, m.h, mn, mx, mean);
            if (nonfinite != 0) {
                line += std::format(" NONFINITE={}", nonfinite);
                if (!first_bad_logged) {
                    line += " <-- FIRST_BAD";
                    first_bad_logged = true;
                }
            }
            LOG_INFO("Engine", "BLOBSCAN {}", line);
            report += line + "\n";
        }
        report += first_bad_logged ? "RESULT=nonfinite\n" : "RESULT=all_finite\n";

        // Self-test the custom Vulkan stages the production u8/float paths
        // wrap around ncnn: whole-image layout pipeline and the tile pipeline.
        // The blob loop above proves ncnn inference is sane, so any garbage
        // appearing here isolates the bug to these custom stages.
#if NCNN_VULKAN
        const auto stage_stats = [&](const char* stage, const float* data, size_t count) {
            size_t nonfinite = 0;
            double mn = std::numeric_limits<double>::infinity();
            double mx = -std::numeric_limits<double>::infinity();
            double sum = 0;
            for (size_t i = 0; i < count; ++i) {
                const float v = data[i];
                if (!std::isfinite(v)) {
                    nonfinite++;
                    continue;
                }
                mn = std::min<double>(mn, v);
                mx = std::max<double>(mx, v);
                sum += v;
            }
            const auto line = std::format("{} min={:.4f} max={:.4f} mean={:.4f} NONFINITE={}",
                                          stage, mn, mx, sum / static_cast<double>(count), nonfinite);
            LOG_INFO("Engine", "BLOBSCAN {}", line);
            report += line + "\n";
        };

        if (current_device_ == DeviceType::GPU_VULKAN && gpu_layout_pipeline_ready_) {
            const int ow = width * scale_factor_;
            const int oh = height * scale_factor_;
            const size_t out_count = static_cast<size_t>(ow) * oh * channels;
            std::vector<float> out(out_count, -777.0f);
            const std::atomic<bool> no_cancel{false};
            auto layout_result = process_vulkan_with_gpu_layout(
                input_float.data(), out.data(), width, height, channels, no_cancel);
            if (!layout_result.has_value()) {
                const auto line = std::format("LAYOUT error: {}", layout_result.error().message);
                LOG_WARNING("Engine", "BLOBSCAN {}", line);
                report += line + "\n";
            } else {
                stage_stats("LAYOUT", out.data(), out_count);
                size_t sentinels = 0;
                for (size_t i = 0; i < out_count; ++i) {
                    if (out[i] == -777.0f) sentinels++;
                }
                const auto line = std::format("LAYOUT sentinels={} / {}", sentinels, out_count);
                LOG_INFO("Engine", "BLOBSCAN {}", line);
                report += line + "\n";
            }
        }
        if (current_device_ == DeviceType::GPU_VULKAN && supports_gpu_tile_pipeline()) {
            const int ow = width * scale_factor_;
            const int oh = height * scale_factor_;
            const size_t out_count = static_cast<size_t>(ow) * oh * channels;
            std::vector<float> out(out_count, 0.0f);
            TileRegion region;
            region.x = 0;
            region.y = 0;
            region.width = static_cast<uint32_t>(width);
            region.height = static_cast<uint32_t>(height);
            region.overlap = 0;
            TileConfig config;
            config.tile_size = std::min(width, 1024);
            config.overlap = 0;
            config.batch_size = 1;
            const std::atomic<bool> no_cancel{false};
            auto tile_result = process_tiles_gpu(
                input_float.data(), out.data(), width, height, channels,
                {region}, config, {}, no_cancel);
            if (!tile_result.has_value()) {
                const auto line = std::format("TILE error: {}", tile_result.error().message);
                LOG_WARNING("Engine", "BLOBSCAN {}", line);
                report += line + "\n";
            } else {
                stage_stats("TILE", out.data(), out_count);
            }
        }
#endif
        return report;
    }

    auto NCNNEngineImpl::process_batch(
        const std::vector<const float*>& inputs,
        const std::vector<float*>& outputs,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr && gpu_layout_pipeline_ready_) {
            if (inputs.size() != outputs.size()) {
                return std::unexpected(EngineError{100, "Batch input/output size mismatch"});
            }
            if (inputs.empty()) {
                return {};
            }
            // Pre-padding models: run each item through the single-image path,
            // which pads the border on the way in and crops it on the way out.
            if (model_prepadding_ > 0) {
                bool fell_back_to_cpu = false;
                for (size_t i = 0; i < inputs.size(); ++i) {
                    if (cancel_flag) {
                        return std::unexpected(EngineError{3, "Processing cancelled"});
                    }
                    auto item_result = process_vulkan_with_gpu_layout(
                        inputs[i], outputs[i], width, height, channels, cancel_flag);
                    if (!item_result.has_value()) {
                        if (!is_oom_error(item_result.error())) {
                            return item_result;
                        }
                        // Device memory refused (preflight or mid-run). Skip the
                        // batched Vulkan section below — it never pads — and
                        // finish the whole batch on the CPU tail.
                        LOG_WARNING("Engine", item_result.error().message +
                            ", falling back to CPU whole-image path");
                        fell_back_to_cpu = true;
                        break;
                    }
                }
                if (!fell_back_to_cpu) {
                    return {};
                }
                return Engine::process_batch(inputs, outputs, width, height, channels, cancel_flag);
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
            const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;

            int input_idx = -1;
            int output_idx = -1;
            if (!net_->input_indexes().empty()) {
                input_idx = net_->input_indexes()[0];
            }
            if (!net_->output_indexes().empty()) {
                output_idx = net_->output_indexes()[0];
            }

            for (size_t i = 0; i < inputs.size(); ++i) {
                if (cancel_flag) {
                    return std::unexpected(EngineError{3, "Processing cancelled"});
                }

                ncnn::Mat in_cpu(static_cast<int>(in_total), const_cast<float*>(inputs[i]), 4u);
                ncnn::VkMat in_gpu_hwc;
                cmd.record_clone(in_cpu, in_gpu_hwc, opt);

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
                    return std::unexpected(EngineError{14, std::format("Failed to set Vulkan input (error {})", ret)});
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

                ncnn::Mat out_cpu(static_cast<int>(out_total), outputs[i], 4u);
                cmd.record_clone(out_gpu_hwc, out_cpu, opt);
            }

            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return std::unexpected(EngineError{17, std::format("Vulkan submit failed (error {})", submit_ret)});
            }

            return {};
        }
#endif

        return Engine::process_batch(inputs, outputs, width, height, channels, cancel_flag);
    }

    bool NCNNEngineImpl::supports_gpu_tile_pipeline() const {
#if NCNN_VULKAN
        // The tile pipelines grow each tile's crop rect by model_prepadding_
        // (replicate at the image border), so the internal border crops cancel
        // and pre-padding models tile fine. Only Crop-layer models WITHOUT a
        // known margin (model_prepadding_ == 0, unsupported scale) stay
        // excluded — they would lose tile borders to the internal crops.
        return (!model_has_border_crop_ || model_prepadding_ > 0) &&
               current_device_ == DeviceType::GPU_VULKAN &&
               vkdev_ != nullptr &&
               gpu_layout_pipeline_ready_ &&
               gpu_tile_pipeline_ready_;
#else
        return false;
#endif
    }

    bool NCNNEngineImpl::supports_gpu_tile_pipeline_u8() const {
#if NCNN_VULKAN
        return (!model_has_border_crop_ || model_prepadding_ > 0) &&
               current_device_ == DeviceType::GPU_VULKAN &&
               vkdev_ != nullptr &&
               gpu_layout_u8_pipeline_ready_ &&
               gpu_tile_pipeline_ready_;
#else
        return false;
#endif
    }

    bool NCNNEngineImpl::supports_gpu_tile_pipeline_u8_queue() const {
#if NCNN_VULKAN
        return supports_gpu_tile_pipeline_u8() &&
               gpu_crop_u8_pipeline_ready_ &&
               normalize_quantize_output_pipeline_ != nullptr;
#else
        return false;
#endif
    }

    auto NCNNEngineImpl::benchmark(int test_tile_size) -> float {
        if (!initialized_ || !model_loaded_) return 0.0f;
        
        std::vector<float> input(static_cast<size_t>(test_tile_size) * test_tile_size * 3, 0.5f);
        std::vector<float> output(static_cast<size_t>(test_tile_size) * test_tile_size * 3 * scale_factor_ * scale_factor_);
        
        auto result = upix::benchmark("tile_process", [&]() {
            std::atomic<bool> cancel = false;
            // fire-and-forget: benchmark measures timing only; a failure here
            // means the model is misconfigured and benchmark() returns 0.0 below.
            (void)process(input.data(), output.data(), test_tile_size, test_tile_size, 3, cancel);
        }, 5, 2);
        
        return 1000.0f / static_cast<float>(result.avg_time_ms);
    }
    
    void NCNNEngineImpl::unload_model() {
        if (net_) {
            net_->clear();
        }
#if NCNN_VULKAN
        preproc_hwc_to_chw_.reset();
        preproc_hwc_u8_to_chw_.reset();
        preproc_hwc_crop_to_chw_.reset();
        preproc_hwc_crop_u8_to_chw_.reset();
        postproc_chw_to_hwc_.reset();
        postproc_chw_to_hwc_u8_.reset();
        quantize_hwc_to_u8_.reset();
        gpu_layout_pipeline_ready_ = false;
        gpu_layout_u8_pipeline_ready_ = false;
        clear_output_pipeline_.reset();
        blend_tile_pipeline_.reset();
        normalize_output_pipeline_.reset();
        normalize_quantize_output_pipeline_.reset();
        gpu_crop_pipeline_ready_ = false;
        gpu_crop_u8_pipeline_ready_ = false;
        gpu_tile_pipeline_ready_ = false;
#endif
        model_loaded_ = false;
        model_name_.clear();
    }
    
    DeviceType NCNNEngineImpl::current_device() const { return current_device_; }
    bool NCNNEngineImpl::is_initialized() const { return initialized_; }
    std::string NCNNEngineImpl::model_name() const { return model_name_; }
    int NCNNEngineImpl::get_scale_factor() const { return scale_factor_; }
    bool NCNNEngineImpl::scale_factor_detected() const { return scale_detected_; }

    void NCNNEngineImpl::set_scale_factor(int scale) {
        if (scale > 0) scale_factor_ = scale;
    }

    void NCNNEngineImpl::set_inference_threads(int threads) {
        if (threads > 0) inference_threads_ = threads;
    }

    auto NCNNEngineImpl::get_memory_info() const -> std::pair<size_t, size_t> {
#if defined(__linux__)
        // Prefer MemAvailable (includes reclaimable cache); sysinfo.freeram
        // understates what a big allocation can actually use.
        std::ifstream meminfo("/proc/meminfo");
        std::string key;
        size_t value_kb = 0;
        std::string unit;
        size_t total_kb = 0;
        size_t avail_kb = 0;
        while (meminfo >> key >> value_kb >> unit) {
            if (key == "MemTotal:") total_kb = value_kb;
            if (key == "MemAvailable:") avail_kb = value_kb;
            meminfo.ignore(1 << 20, '\n');
        }
        if (total_kb > 0 && avail_kb > 0) {
            return {total_kb / 1024, avail_kb / 1024};
        }
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            size_t total = info.totalram * info.mem_unit / (1024 * 1024);
            size_t avail = info.freeram * info.mem_unit / (1024 * 1024);
            return {total, avail};
        }
#elif defined(__APPLE__)
        mach_port_t host = mach_host_self();
        vm_size_t page_size;
        host_page_size(host, &page_size);
        vm_statistics64_data_t vm_stat;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&vm_stat), &count) == KERN_SUCCESS) {
            size_t total = 0;
            size_t avail = vm_stat.free_count * page_size / (1024 * 1024);
            // simplified: total not precise
            total = avail * 2; // approximate
            return {total, avail};
        }
#endif
        return {0, 0};
    }

    auto NCNNEngineImpl::hardware_id() const -> std::string {
        if (!initialized_) {
            return {};
        }
        if (current_device_ == DeviceType::GPU_VULKAN) {
#if NCNN_VULKAN
            if (vkdev_ == nullptr) {
                return {};
            }
            // Device name + driver version: a driver update can shift Vulkan
            // performance as much as a card swap, so both belong in the
            // auto-tile cache key.
            const char* name = vkdev_->info.device_name();
            return std::format("vulkan|{}|driver-{}", name ? name : "unknown",
                               vkdev_->info.driver_version());
#else
            return {};
#endif
        }
        // CPU: physical core count plus the SIMD feature bits ncnn dispatches
        // on (feature functions return 0 on foreign architectures, so the
        // same expression is portable x86/ARM). A same-core-count CPU without
        // AVX2 measures differently anyway; the flags catch that class.
        return std::format("cpu|cores-{}|avx2-{}|fma-{}|avx512-{}|fp16-{}",
                           ncnn::get_physical_cpu_count(),
                           ncnn::cpu_support_x86_avx2(),
                           ncnn::cpu_support_x86_fma(),
                           ncnn::cpu_support_x86_avx512(),
                           ncnn::cpu_support_arm_asimdhp());
    }

    int NCNNEngineImpl::get_recommended_tile_size() const {
        if (current_device_ != DeviceType::GPU_VULKAN) {
            return 128;  // CPU: small tiles to limit memory on mobile
        }

#if NCNN_VULKAN
        if (vkdev_ == nullptr) return 128;

        // Use NCNN's built-in heap budget query (MB)
        const size_t heap_budget_mb = get_device_memory_budget_mb();

        // Detect mobile GPU families for conservative tiling
        const char* device_name = vkdev_->info.device_name();
        const auto name_sv = device_name ? std::string_view(device_name) : std::string_view{};
        const bool is_adreno   = name_sv.find("Adreno") != std::string_view::npos;
        const bool is_mali     = name_sv.find("Mali") != std::string_view::npos;
        const bool is_powervr = name_sv.find("PowerVR") != std::string_view::npos ||
                                 name_sv.find("Imagination") != std::string_view::npos;
        const bool is_mobile_gpu = is_adreno || is_mali || is_powervr;

        // VRAM-aware tile size selection (adapted from RealSR-NCNN-Android).
        // The desktop tiers bias large: at tile=200 the overlap strip wastes
        // ~15% of the GPU's compute on pixels a neighbour also produces
        // ((200² - 184²)/200²); at 384 that redundancy halves and the measured
        // batch wall time drops accordingly. The working-set guard below still
        // shrinks the size whenever the per-tile feature maps exceed 1/8 of
        // the heap budget, so small / busy GPUs keep the conservative tiers.
        int tilesize;
        if (heap_budget_mb > 6000 && !is_mobile_gpu) {
            tilesize = 384;
        } else if (heap_budget_mb > 2800) {
            tilesize = is_mobile_gpu ? 160 : 200;
        } else if (heap_budget_mb > 1600) {
            tilesize = is_mobile_gpu ? 128 : 160;
        } else if (heap_budget_mb > 800) {
            tilesize = is_mobile_gpu ? 96 : 128;
        } else if (heap_budget_mb > 200) {
            tilesize = 64;
        } else {
            tilesize = 32;  // extremely constrained
        }

        // The per-tile feature-map working set scales with tile²; shrink until
        // its estimate fits within 1/8 of the heap budget. For the RRDB family
        // this is ~(t+2*16)² × 64 ch × 2 B (fp16) × ~16 live blobs × batch 2.
        // Pre-padding models feed the net (t + 2*prepadding), which counts too.
        while (tilesize > 32) {
            const double t = static_cast<double>(tilesize) + 2 * 16 + 2 * model_prepadding_;
            const double working_set_mb = t * t * 64 * 2 * 16 * 2 / (1024.0 * 1024.0);
            if (working_set_mb <= static_cast<double>(heap_budget_mb) / 8.0) {
                break;
            }
            tilesize /= 2;
        }

        LOG_INFO("Engine", "Auto tile size: {} (VRAM {}MB{})",
                 tilesize, heap_budget_mb, is_mobile_gpu ? ", mobile GPU" : "");
        return tilesize;
#else
        return 128;
#endif
    }

    size_t NCNNEngineImpl::get_device_memory_budget_mb() const {
#if NCNN_VULKAN
        // Testing hook: pretend the device has less VRAM than it does, so the
        // OOM preflight / fallback ladder can be exercised on a big machine.
        if (const char* forced = std::getenv("UPIX_GPU_FORCE_BUDGET_MB")) {
            try { return static_cast<size_t>(std::max(0, std::stoi(forced))); } catch (...) {}
        }
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr) {
            return static_cast<size_t>(vkdev_->get_heap_budget());
        }
#endif
        return 0;
    }

std::unique_ptr<Engine> create_ncnn_engine() {
    return std::make_unique<NCNNEngineImpl>();
}

} // namespace upix
