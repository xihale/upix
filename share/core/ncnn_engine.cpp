#include "ncnn_engine.hpp"
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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
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

namespace crossscale {

#if NCNN_VULKAN
namespace {

std::mutex g_vulkan_init_mutex;
std::mutex g_vulkan_model_load_mutex;

constexpr const char* kHwcToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = (gy * p.w + gx) * p.channels + gz;
    const int dst_idx = gz * p.cstep + gy * p.w + gx;
    dst_data[dst_idx] = sfp(src_data[src_idx]);
}
)";

constexpr const char* kHwcU8ToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

#if NCNN_int8_storage
layout (binding = 0) readonly buffer src_blob { uint8_t src_data[]; };
#else
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
#endif
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = (gy * p.w + gx) * p.channels + gz;
    const int dst_idx = gz * p.cstep + gy * p.w + gx;
#if NCNN_int8_storage
    dst_data[dst_idx] = sfp(float(uint(src_data[src_idx])) * (1.0 / 255.0));
#else
    dst_data[dst_idx] = sfp(src_data[src_idx] * (1.0 / 255.0));
#endif
}
)";

constexpr const char* kHwcCropToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int src_w;
    int src_h;
    int channels;
    int tile_x;
    int tile_y;
    int tile_w;
    int tile_h;
    int dst_cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.tile_w || gy >= p.tile_h || gz >= p.channels)
        return;

    int src_x = p.tile_x + gx;
    int src_y = p.tile_y + gy;
    src_x = abs(src_x);
    src_y = abs(src_y);
    src_x = (p.src_w - 1) - abs(src_x - (p.src_w - 1));
    src_y = (p.src_h - 1) - abs(src_y - (p.src_h - 1));

    const int src_idx = (src_y * p.src_w + src_x) * p.channels + gz;
    const int dst_idx = gz * p.dst_cstep + gy * p.tile_w + gx;
    dst_data[dst_idx] = sfp(src_data[src_idx]);
}
)";

constexpr const char* kHwcCropU8ToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

#if NCNN_int8_storage
layout (binding = 0) readonly buffer src_blob { uint8_t src_data[]; };
#else
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
#endif
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int src_w;
    int src_h;
    int channels;
    int tile_x;
    int tile_y;
    int tile_w;
    int tile_h;
    int dst_cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.tile_w || gy >= p.tile_h || gz >= p.channels)
        return;

    int src_x = p.tile_x + gx;
    int src_y = p.tile_y + gy;
    src_x = abs(src_x);
    src_y = abs(src_y);
    src_x = (p.src_w - 1) - abs(src_x - (p.src_w - 1));
    src_y = (p.src_h - 1) - abs(src_y - (p.src_h - 1));

    const int src_idx = (src_y * p.src_w + src_x) * p.channels + gz;
    const int dst_idx = gz * p.dst_cstep + gy * p.tile_w + gx;
#if NCNN_int8_storage
    dst_data[dst_idx] = sfp(float(uint(src_data[src_idx])) * (1.0 / 255.0));
#else
    dst_data[dst_idx] = sfp(src_data[src_idx] * (1.0 / 255.0));
#endif
}
)";

constexpr const char* kChwToHwcComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = gz * p.cstep + gy * p.w + gx;
    const int dst_idx = (gy * p.w + gx) * p.channels + gz;
    dst_data[dst_idx] = float(src_data[src_idx]);
}
)";

constexpr const char* kChwToHwcU8Comp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
#if NCNN_int8_storage
layout (binding = 1) writeonly buffer dst_blob { uint8_t dst_data[]; };
#else
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };
#endif

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = gz * p.cstep + gy * p.w + gx;
    const int dst_idx = (gy * p.w + gx) * p.channels + gz;
    const float v = clamp(float(src_data[src_idx]) * 255.0, 0.0, 255.0);
#if NCNN_int8_storage
    const uint v32 = clamp(uint(floor(v + 0.5)), 0u, 255u);
    dst_data[dst_idx] = uint8_t(v32);
#else
    dst_data[dst_idx] = v;
#endif
}
)";

constexpr const char* kHwcFloatToU8Comp = R"(
#version 450

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
#if NCNN_int8_storage
layout (binding = 1) writeonly buffer dst_blob { uint8_t dst_data[]; };
#else
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };
#endif

layout (push_constant) uniform parameter
{
    int total;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    if (gx >= p.total)
        return;

    const float v = clamp(src_data[gx] * 255.0, 0.0, 255.0);
#if NCNN_int8_storage
    const uint v32 = clamp(uint(floor(v + 0.5)), 0u, 255u);
    dst_data[gx] = uint8_t(v32);
#else
    dst_data[gx] = v;
#endif
}
)";

constexpr const char* kClearComp = R"(
#version 450

layout (binding = 0) writeonly buffer dst_blob { float dst_data[]; };

layout (push_constant) uniform parameter
{
    int total;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    if (gx >= p.total)
        return;
    dst_data[gx] = 0.0;
}
)";

constexpr const char* kBlendTileComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
layout (binding = 1) buffer dst_blob { float dst_data[]; };
layout (binding = 2) buffer weight_blob { float weight_data[]; };

layout (push_constant) uniform parameter
{
    int tile_w;
    int tile_h;
    int channels;
    int out_w;
    int out_h;
    int out_x;
    int out_y;
    int overlap;
    int src_w;
    int src_h;
    int src_offset_x;
    int src_offset_y;
    int src_cstep;
} p;

float blend_weight(int pos, int size, int overlap, int tile_start, int full_size)
{
    const int effective_overlap = min(overlap, size);
    if (effective_overlap <= 0)
        return 1.0;
    float w = 1.0;
    if (tile_start > 0 && pos < effective_overlap)
        w = float(pos + 1) / float(effective_overlap);
    else if (tile_start + size < full_size && pos >= size - effective_overlap)
        w = float(size - pos) / float(effective_overlap);
    return w * w;
}

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.tile_w || gy >= p.tile_h || gz >= p.channels)
        return;

    const int out_px = p.out_x + gx;
    const int out_py = p.out_y + gy;
    if (out_px < 0 || out_px >= p.out_w || out_py < 0 || out_py >= p.out_h)
        return;

    const float wx = blend_weight(gx, p.tile_w, p.overlap, p.out_x, p.out_w);
    const float wy = blend_weight(gy, p.tile_h, p.overlap, p.out_y, p.out_h);
    const float w = wx * wy;

    const int src_x = p.src_offset_x + gx;
    const int src_y = p.src_offset_y + gy;
    if (src_x < 0 || src_x >= p.src_w || src_y < 0 || src_y >= p.src_h)
        return;

    const int src_xy_idx = src_y * p.src_w + src_x;
    const int src_idx = gz * p.src_cstep + src_xy_idx;
    const int dst_idx = (out_py * p.out_w + out_px) * p.channels + gz;
    dst_data[dst_idx] += float(src_data[src_idx]) * w;
    if (gz == 0) {
        const int weight_idx = out_py * p.out_w + out_px;
        weight_data[weight_idx] += w;
    }
}
)";

constexpr const char* kNormalizeByWeightComp = R"(
#version 450

layout (binding = 0) buffer dst_blob { float dst_data[]; };
layout (binding = 1) readonly buffer weight_blob { float weight_data[]; };

layout (push_constant) uniform parameter
{
    int total_pixels;
    int channels;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.total_pixels || gz >= p.channels)
        return;

    const float w = weight_data[gx];
    const int dst_idx = gx * p.channels + gz;
    if (w <= 1e-6) {
        dst_data[dst_idx] = 0.0;
        return;
    }

    const float inv_w = 1.0 / w;
    dst_data[dst_idx] = dst_data[dst_idx] * inv_w;
}
)";

constexpr const char* kNormalizeQuantizeByWeightComp = R"(
#version 450

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) readonly buffer weight_blob { float weight_data[]; };
#if NCNN_int8_storage
layout (binding = 2) writeonly buffer dst_blob { uint8_t dst_data[]; };
#else
layout (binding = 2) writeonly buffer dst_blob { float dst_data[]; };
#endif

layout (push_constant) uniform parameter
{
    int total_pixels;
    int channels;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.total_pixels || gz >= p.channels)
        return;

    const float w = weight_data[gx];
    const int dst_idx = gx * p.channels + gz;
    if (w <= 1e-6) {
#if NCNN_int8_storage
        dst_data[dst_idx] = uint8_t(0u);
#else
        dst_data[dst_idx] = 0.0;
#endif
        return;
    }

    const float inv_w = 1.0 / w;
    const float normalized = src_data[dst_idx] * inv_w;
    const float v = clamp(normalized * 255.0, 0.0, 255.0);
#if NCNN_int8_storage
    const uint v32 = clamp(uint(floor(v + 0.5)), 0u, 255u);
    dst_data[dst_idx] = uint8_t(v32);
#else
    dst_data[dst_idx] = v;
#endif
}
)";

} // namespace
#endif

class NCNNEngineImpl : public Engine {
public:
    NCNNEngineImpl() : net_(std::make_unique<ncnn::Net>()) {}
    ~NCNNEngineImpl() override { unload_model(); }

    auto initialize(DeviceType preferred) 
        -> expected<void, EngineError> override {
        
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
    
    auto list_devices() -> std::vector<DeviceCapability> override {
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
            gpu.name = "Vulkan GPU-" + std::to_string(i);
            gpu.compute_units = 1;
            gpu.memory_mb = 0;
            gpu.benchmark_score = 10.0f;
            devices.push_back(gpu);
        }
#endif
        
        return devices;
    }
    
    auto load_model(const std::string& param_path, const std::string& bin_path)
        -> expected<void, EngineError> override {
        
        if (!initialized_) {
            return EngineError{1, "Engine not initialized"};
        }
        
        LOG_INFO("Engine", "Loading model: " + param_path);
        
        model_loaded_ = false;
        
        // Set Vulkan device if using GPU
#if NCNN_VULKAN
        static const bool enable_gpu_fast_math = []() {
            const char* env_value = std::getenv("CROSSSCALE_GPU_FAST_MATH");
            return env_value != nullptr && std::string(env_value) == "1";
        }();
        std::unique_lock<std::mutex> gpu_model_load_lock;
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr) {
            gpu_model_load_lock = std::unique_lock<std::mutex>(g_vulkan_model_load_mutex);
            LOG_INFO("Engine", "Serializing Vulkan model load to avoid concurrent queue submits");
            net_->set_vulkan_device(vkdev_);
            net_->opt.use_vulkan_compute = true;
            const auto& gpu_info = vkdev_->info;
            net_->opt.use_fp16_packed = enable_gpu_fast_math && gpu_info.support_fp16_packed();
            net_->opt.use_fp16_storage = enable_gpu_fast_math && gpu_info.support_fp16_storage();
            net_->opt.use_fp16_arithmetic = enable_gpu_fast_math && gpu_info.support_fp16_arithmetic();
            net_->opt.use_int8_storage = false;
            unsigned int hw_threads = std::thread::hardware_concurrency();
            int tuned_threads = 2;
            if (hw_threads > 0) {
                tuned_threads = std::clamp(static_cast<int>(hw_threads / 4), 1, 4);
            }
            net_->opt.num_threads = tuned_threads;
            LOG_INFO(
                "Engine",
                "Vulkan precision profile: fp16_packed=" +
                    std::to_string(net_->opt.use_fp16_packed ? 1 : 0) +
                    ", fp16_storage=" + std::to_string(net_->opt.use_fp16_storage ? 1 : 0) +
                    ", fp16_arithmetic=" + std::to_string(net_->opt.use_fp16_arithmetic ? 1 : 0) +
                    ", int8_storage=0"
            );
        } else {
            net_->opt.use_vulkan_compute = false;
            net_->opt.use_fp16_packed = false;
            net_->opt.use_fp16_storage = false;
            net_->opt.use_fp16_arithmetic = false;
            net_->opt.use_int8_storage = false;
        }
#else
        net_->opt.use_vulkan_compute = false;
        net_->opt.use_fp16_packed = false;
        net_->opt.use_fp16_storage = false;
        net_->opt.use_fp16_arithmetic = false;
        net_->opt.use_int8_storage = false;
#endif
        
        // Load param file
        int ret = net_->load_param(param_path.c_str());
        if (ret != 0) {
            return EngineError{3, "Failed to load param file: " + param_path + " (error " + std::to_string(ret) + ")"};
        }
        
        // Load model weights
        ret = net_->load_model(bin_path.c_str());
        if (ret != 0) {
            if (current_device_ == DeviceType::GPU_VULKAN && ret == -4) {
                const std::string detail = format_vulkan_runtime_snapshot("load_model_device_lost");
                LOG_ERROR("Engine", "Vulkan device lost during net_->load_model: " + detail);
                return EngineError{
                    4,
                    "Failed to load model weights (VK_ERROR_DEVICE_LOST): " + bin_path +
                        " (error " + std::to_string(ret) + "); " + detail
                };
            }
            return EngineError{4, "Failed to load model weights: " + bin_path + " (error " + std::to_string(ret) + ")"};
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
                    LOG_INFO("Engine", "GPU u8 pre/post layout pipeline disabled by stability profile");
                }
                LOG_WARNING("Engine", "GPU u8 pipeline readiness details: " + format_vulkan_runtime_snapshot("load_model_gpu_u8_not_ready"));
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
                    LOG_INFO("Engine", "GPU u8 crop pipeline disabled by stability profile");
                }
            }
            gpu_tile_pipeline_ready_ = init_gpu_tile_pipelines();
            if (!gpu_tile_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU tile blend pipeline disabled, fallback to CPU tile blending");
            }
            LOG_INFO("Engine", "GPU pipeline readiness snapshot: " + format_vulkan_runtime_snapshot("load_model_after_pipeline_init"));
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
        
        LOG_INFO("Engine", "Model loaded successfully (scale: " + std::to_string(scale_factor_) + ")");
        return {};
    }
    
    auto process(const float* input, float* output, int width, int height, int channels,
                 const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> override {
        
        if (!initialized_) {
            return EngineError{1, "Engine not initialized"};
        }
        if (!model_loaded_) {
            return EngineError{2, "No model loaded"};
        }
        
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN) {
            if (vkdev_ == nullptr || !gpu_layout_pipeline_ready_) {
                return EngineError{21, "GPU layout pipeline is not ready"};
            }

            auto vk_result = process_vulkan_with_gpu_layout(input, output, width, height, channels, cancel_flag);
            if (!vk_result.has_value()) {
                return EngineError{22, "Vulkan layout pipeline failed: " + vk_result.error().message};
            }
            return {};
        }
#endif
        
        if (input == nullptr || output == nullptr) {
            return EngineError{18, "Null input/output buffer"};
        }
        if (width <= 0 || height <= 0 || channels <= 0) {
            return EngineError{11, "Invalid input dimensions"};
        }

        const int expected_out_width = width * scale_factor_;
        const int expected_out_height = height * scale_factor_;
        
        // Create input Mat from float array (interleaved RGB -> planar)
        // ncnn expects planar format: RRR...GGG...BBB...
        ncnn::Mat in(width, height, channels);
        if (in.empty()) {
            return EngineError{5, "Failed to create input Mat"};
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
        if (input_idx >= 0) {
            ret = ex.input(input_idx, in);
        } else {
            // Fallback to common name
            ret = ex.input("data", in);
        }
        if (ret != 0) {
            return EngineError{6, "Failed to set input (error " + std::to_string(ret) + ")"};
        }
        
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
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
                    return EngineError{7, "Failed to extract output (error " + std::to_string(ret) + ")"};
                }
            }
        }
        
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }
        
        if (out.w != expected_out_width || out.h != expected_out_height || out.c != channels) {
            return EngineError{
                16,
                "Output shape mismatch: expected=" +
                    std::to_string(expected_out_width) + "x" + std::to_string(expected_out_height) + "x" +
                    std::to_string(channels) + ", actual=" +
                    std::to_string(out.w) + "x" + std::to_string(out.h) + "x" + std::to_string(out.c)
            };
        }

        const int out_width = out.w;
        const int out_height = out.h;

        for (int c = 0; c < channels; ++c) {
            const float* out_channel = out.channel(c);
            for (int h = 0; h < out_height; ++h) {
                for (int w = 0; w < out_width; ++w) {
                    size_t dst_idx = (static_cast<size_t>(h) * out_width + w) * channels + c;
                    size_t src_idx = static_cast<size_t>(h) * out_width + w;
                    output[dst_idx] = out_channel[src_idx];
                }
            }
        }
        
        return {};
    }

    auto process_u8(const uint8_t* input, uint8_t* output, int width, int height, int channels,
                    const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> override {
        if (!initialized_) {
            return EngineError{1, "Engine not initialized"};
        }
        if (!model_loaded_) {
            return EngineError{2, "No model loaded"};
        }
        if (input == nullptr || output == nullptr) {
            return EngineError{18, "Null input/output buffer"};
        }
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN) {
            if (vkdev_ == nullptr) {
                const std::string detail = format_vulkan_runtime_snapshot("process_u8_device_not_ready", width, height, channels);
                return EngineError{23, "GPU device not ready; " + detail};
            }

            // Use the most efficient U8 queue pipeline if available
            if (supports_gpu_tile_pipeline_u8_queue()) {
                LOG_INFO("Engine", "Processing u8 input through optimized Vulkan U8 queue pipeline");

                TileConfig tile_config;
                tile_config.tile_size = 512; // Increased default tile size for modern GPUs
                tile_config.overlap = 16;
                tile_config.batch_size = 4; // Increased batch size
                
                if (const char* tile_size_env = std::getenv("CROSSSCALE_GPU_TILE_SIZE")) {
                    try { tile_config.tile_size = std::clamp(std::stoi(tile_size_env), 64, 2048); } catch (...) {}
                }
                if (const char* tile_overlap_env = std::getenv("CROSSSCALE_GPU_TILE_OVERLAP")) {
                    try { tile_config.overlap = std::clamp(std::stoi(tile_overlap_env), 0, tile_config.tile_size / 2); } catch (...) {}
                }
                if (const char* tile_batch_env = std::getenv("CROSSSCALE_GPU_TILE_BATCH")) {
                    try { tile_config.batch_size = std::clamp(std::stoi(tile_batch_env), 1, 32); } catch (...) {}
                }

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

                return process_tiles_gpu_u8_queue({task}, cancel_flag);
            }

            if (gpu_tile_pipeline_ready_) {
                LOG_INFO("Engine", "Processing u8 input through Vulkan float tile pipeline (fallback)");

                const size_t in_total = static_cast<size_t>(width) * height * channels;
                const int out_width = width * scale_factor_;
                const int out_height = height * scale_factor_;
                const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;

                std::vector<float> input_float(in_total);
                constexpr float kInv255 = 1.0f / 255.0f;
                for (size_t i = 0; i < in_total; ++i) {
                    input_float[i] = static_cast<float>(input[i]) * kInv255;
                }

                std::vector<float> output_float(out_total, 0.0f);

                TileConfig tile_config;
                tile_config.tile_size = 256;
                tile_config.overlap = 16;
                tile_config.batch_size = 2;
                if (const char* tile_size_env = std::getenv("CROSSSCALE_GPU_TILE_SIZE")) {
                    try {
                        tile_config.tile_size = std::clamp(std::stoi(tile_size_env), 64, 1024);
                    } catch (...) {
                    }
                }
                if (const char* tile_overlap_env = std::getenv("CROSSSCALE_GPU_TILE_OVERLAP")) {
                    try {
                        const int max_overlap = std::max(1, tile_config.tile_size / 2);
                        tile_config.overlap = std::clamp(std::stoi(tile_overlap_env), 0, max_overlap);
                    } catch (...) {
                    }
                }
                if (const char* tile_batch_env = std::getenv("CROSSSCALE_GPU_TILE_BATCH")) {
                    try {
                        tile_config.batch_size = std::clamp(std::stoi(tile_batch_env), 1, 8);
                    } catch (...) {
                    }
                }

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
                    return EngineError{24, "Vulkan float tile pipeline failed: " + tile_result.error().message};
                }

                for (size_t i = 0; i < out_total; ++i) {
                    const float value = output_float[i];
                    if (!std::isfinite(value)) {
                        return EngineError{20, "Non-finite output value detected"};
                    }
                    const float v = std::clamp(value * 255.0f, 0.0f, 255.0f);
                    output[i] = static_cast<uint8_t>(std::lround(v));
                }

                return {};
            }

            LOG_WARNING("Engine", "Vulkan tile pipeline unavailable, switching to non-tile Vulkan float compatibility path");

            const size_t in_total = static_cast<size_t>(width) * height * channels;
            const int out_width = width * scale_factor_;
            const int out_height = height * scale_factor_;
            const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;

            std::vector<float> input_float(in_total);
            constexpr float kInv255 = 1.0f / 255.0f;
            for (size_t i = 0; i < in_total; ++i) {
                input_float[i] = static_cast<float>(input[i]) * kInv255;
            }

            std::vector<float> output_float(out_total);
            auto vk_result = process_vulkan_with_gpu_layout(
                input_float.data(),
                output_float.data(),
                width,
                height,
                channels,
                cancel_flag
            );
            if (!vk_result.has_value()) {
                return EngineError{24, "Vulkan float compatibility path failed: " + vk_result.error().message};
            }

            for (size_t i = 0; i < out_total; ++i) {
                const float value = output_float[i];
                if (!std::isfinite(value)) {
                    return EngineError{20, "Non-finite output value detected"};
                }
                const float v = std::clamp(value * 255.0f, 0.0f, 255.0f);
                output[i] = static_cast<uint8_t>(std::lround(v));
            }

            return {};
        }
#endif

        const size_t in_total = static_cast<size_t>(width) * height * channels;
        std::vector<float> input_float(in_total);
        constexpr float kInv255 = 1.0f / 255.0f;
        for (size_t i = 0; i < in_total; ++i) {
            input_float[i] = static_cast<float>(input[i]) * kInv255;
        }

        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        std::vector<float> output_float(out_total);

        auto result = process(input_float.data(), output_float.data(), width, height, channels, cancel_flag);
        if (!result.has_value()) {
            return result;
        }

        for (size_t i = 0; i < out_total; ++i) {
            const float value = output_float[i];
            if (!std::isfinite(value)) {
                return EngineError{20, "Non-finite output value detected"};
            }
            const float v = std::clamp(value * 255.0f, 0.0f, 255.0f);
            output[i] = static_cast<uint8_t>(std::lround(v));
        }

        return {};
    }

    auto process_batch(
        const std::vector<const float*>& inputs,
        const std::vector<float*>& outputs,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> override {
#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr && gpu_layout_pipeline_ready_) {
            if (inputs.size() != outputs.size()) {
                return EngineError{100, "Batch input/output size mismatch"};
            }
            if (inputs.empty()) {
                return {};
            }

            if (cancel_flag) {
                return EngineError{3, "Processing cancelled"};
            }

            const int out_width = width * scale_factor_;
            const int out_height = height * scale_factor_;
            const size_t in_total = static_cast<size_t>(width) * height * channels;
            const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
            if (in_total > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
                return EngineError{13, "Image too large for Vulkan layout path"};
            }

            ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
            ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
            struct VkAllocGuard {
                ncnn::VulkanDevice* dev = nullptr;
                ncnn::VkAllocator* blob = nullptr;
                ncnn::VkAllocator* staging = nullptr;
                ~VkAllocGuard() {
                    if (dev != nullptr) {
                        if (blob) dev->reclaim_blob_allocator(blob);
                        if (staging) dev->reclaim_staging_allocator(staging);
                    }
                }
            } alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

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
                    return EngineError{3, "Processing cancelled"};
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
                    return EngineError{14, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"};
                }

                ncnn::VkMat out_gpu;
                if (output_idx >= 0) {
                    ret = ex.extract(output_idx, out_gpu, cmd);
                } else {
                    ret = ex.extract("output", out_gpu, cmd);
                    if (ret != 0) {
                        ret = ex.extract("out", out_gpu, cmd);
                        if (ret != 0) {
                            return EngineError{15, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"};
                        }
                    }
                }

                if (out_gpu.elempack != 1) {
                    ncnn::VkMat out_unpacked;
                    vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                    out_gpu = out_unpacked;
                }

                if (out_gpu.w != out_width || out_gpu.h != out_height || out_gpu.c != channels) {
                    return EngineError{16, "Vulkan output shape mismatch"};
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
                return EngineError{17, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"};
            }

            return {};
        }
#endif

        return Engine::process_batch(inputs, outputs, width, height, channels, cancel_flag);
    }

    bool supports_gpu_tile_pipeline() const override {
#if NCNN_VULKAN
        return current_device_ == DeviceType::GPU_VULKAN &&
               vkdev_ != nullptr &&
               gpu_layout_pipeline_ready_ &&
               gpu_tile_pipeline_ready_;
#else
        return false;
#endif
    }

    bool supports_gpu_tile_pipeline_u8() const override {
#if NCNN_VULKAN
        return current_device_ == DeviceType::GPU_VULKAN &&
               vkdev_ != nullptr &&
               gpu_layout_u8_pipeline_ready_ &&
               gpu_tile_pipeline_ready_;
#else
        return false;
#endif
    }

    bool supports_gpu_tile_pipeline_u8_queue() const override {
#if NCNN_VULKAN
        return supports_gpu_tile_pipeline_u8() &&
               gpu_crop_u8_pipeline_ready_ &&
               normalize_quantize_output_pipeline_ != nullptr;
#else
        return false;
#endif
    }

    auto process_tiles_gpu_u8_queue(
        const std::vector<GPUTileTaskU8>& tasks,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> override {
#if NCNN_VULKAN
        if (tasks.empty()) {
            return {};
        }
        if (!supports_gpu_tile_pipeline_u8_queue()) {
            return Engine::process_tiles_gpu_u8_queue(tasks, cancel_flag);
        }
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        struct VkAllocGuard {
            ncnn::VulkanDevice* dev = nullptr;
            ncnn::VkAllocator* blob = nullptr;
            ncnn::VkAllocator* staging = nullptr;
            ~VkAllocGuard() {
                if (dev != nullptr) {
                    if (blob) dev->reclaim_blob_allocator(blob);
                    if (staging) dev->reclaim_staging_allocator(staging);
                }
            }
        } alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

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
        size_t submit_budget = 0;
        size_t total_tiles = 0;
        size_t total_tile_pixels = 0;
        size_t max_task_channels = 1;
        for (const auto& task : tasks) {
            if (task.input_data == nullptr || task.output_data == nullptr || task.regions == nullptr) {
                return EngineError{205, "GPU queue task has null buffers"};
            }
            if (task.channels <= 0 || task.input_width <= 0 || task.input_height <= 0) {
                return EngineError{202, "Invalid input dimensions"};
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
                return EngineError{203, "Output image too large for Vulkan tile pipeline"};
            }

            state.out_gpu_hwc.create(static_cast<int>(state.out_total), 4u, blob_vkallocator);
            state.out_gpu_weight.create(static_cast<int>(state.out_total_pixels), 4u, blob_vkallocator);
            state.out_gpu_u8.create(static_cast<int>(state.out_total), 1u, blob_vkallocator);
            state.progress.total_tiles = static_cast<int>(task.regions->size());
            state.progress.completed_tiles = 0;
            states.emplace_back(std::move(state));

            submit_budget += static_cast<size_t>(std::max(1, task.config.batch_size));
            total_tiles += task.regions->size();
            max_task_channels = std::max(max_task_channels, static_cast<size_t>(task.channels));
            std::vector<TilePadInfo> pad_infos;
            pad_infos.reserve(task.regions->size());
            for (const auto& region : *task.regions) {
                total_tile_pixels += static_cast<size_t>(region.width) * static_cast<size_t>(region.height);
                pad_infos.emplace_back(compute_pad(task, region));
            }
            task_pad_infos.emplace_back(std::move(pad_infos));
        }
        if (total_tiles == 0) {
            return {};
        }
        const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
        const double avg_tile_pixels = static_cast<double>(total_tile_pixels) /
                                       static_cast<double>(std::max<size_t>(1, total_tiles));

        size_t tiles_per_task_target = 24;
        if (avg_tile_pixels <= (256.0 * 256.0)) {
            tiles_per_task_target = 40;
        } else if (avg_tile_pixels <= (384.0 * 384.0)) {
            tiles_per_task_target = 32;
        }
        if (const char* target_env = std::getenv("CROSSSCALE_GPU_SUBMIT_TILES_PER_TASK")) {
            try {
                tiles_per_task_target = std::clamp(
                    static_cast<size_t>(std::stoul(target_env)),
                    static_cast<size_t>(8),
                    static_cast<size_t>(96)
                );
            } catch (...) {
                // Ignore malformed env input and keep adaptive target.
            }
        }

        submit_budget = std::max(submit_budget * 8, tasks.size() * tiles_per_task_target);
        submit_budget = std::max(submit_budget, static_cast<size_t>(32));

        size_t submit_memory_budget_mb = 768;
        if (vkdev_ != nullptr) {
            const bool is_discrete_gpu = vkdev_->info.type() == 0;
            const size_t heap_budget_mb = static_cast<size_t>(vkdev_->get_heap_budget());
            if (heap_budget_mb > 0) {
                // Keep submit memory bounded to a fraction of the live heap budget so
                // high-end dGPUs can keep deeper in-flight work without starving small GPUs.
                const size_t budget_divisor = is_discrete_gpu ? 8 : 14;
                const size_t min_budget_mb = is_discrete_gpu ? 768 : 256;
                const size_t max_budget_mb = is_discrete_gpu ? 1536 : 640;
                submit_memory_budget_mb = std::clamp(
                    heap_budget_mb / budget_divisor,
                    min_budget_mb,
                    max_budget_mb
                );
            } else {
                submit_memory_budget_mb = is_discrete_gpu ? 1024 : 384;
            }
        }
        if (const char* mem_env = std::getenv("CROSSSCALE_GPU_SUBMIT_MB")) {
            try {
                submit_memory_budget_mb = std::clamp(
                    static_cast<size_t>(std::stoul(mem_env)),
                    static_cast<size_t>(128),
                    static_cast<size_t>(2048)
                );
            } catch (...) {
                // Ignore malformed env input and keep default memory budget.
            }
        }

        const double scale_squared = static_cast<double>(scale_factor_) * static_cast<double>(scale_factor_);
        const double bytes_per_tile = std::max(
            1.0,
            avg_tile_pixels *
                static_cast<double>(max_task_channels) *
                static_cast<double>(in_elemsize) *
                (1.0 + scale_squared)
        );
        size_t submit_budget_cap = static_cast<size_t>(
            (static_cast<double>(submit_memory_budget_mb) * 1024.0 * 1024.0) / bytes_per_tile
        );
        submit_budget_cap = std::clamp(submit_budget_cap, static_cast<size_t>(32), static_cast<size_t>(256));
        submit_budget_cap = std::min(total_tiles, submit_budget_cap);
        if (const char* budget_env = std::getenv("CROSSSCALE_GPU_SUBMIT_BUDGET")) {
            try {
                const size_t env_budget = static_cast<size_t>(std::stoul(budget_env));
                submit_budget_cap = std::min(total_tiles, std::clamp(env_budget, static_cast<size_t>(16), static_cast<size_t>(256)));
            } catch (...) {
                // Ignore malformed env input and keep default cap.
            }
        }
        submit_budget = std::min(submit_budget, submit_budget_cap);
        submit_budget = std::max(submit_budget, static_cast<size_t>(16));
        LOG_INFO("Engine", "GPU queue submit tuning: " + format_vulkan_runtime_snapshot(
            "queue_budget",
            -1,
            -1,
            static_cast<int>(max_task_channels),
            submit_budget,
            submit_budget_cap,
            submit_memory_budget_mb,
            avg_tile_pixels
        ));

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
                return EngineError{206, "Invalid tile dimensions"};
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
                return EngineError{207, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"};
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return EngineError{208, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"};
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                return EngineError{209, "Vulkan output shape mismatch"};
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
                return EngineError{3, "Processing cancelled"};
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
                        return EngineError{205, "Input image too large for Vulkan tile pipeline"};
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
                        return EngineError{206, "Invalid tile dimensions"};
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
                        return record_result.error();
                    }
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
                    return EngineError{214, "Vulkan queue normalize+quantize/download failed (error " +
                                            std::to_string(submit_ret) + ")"};
                }
                if (has_init_work) {
                    return EngineError{206, "Vulkan queue init failed (error " + std::to_string(submit_ret) + ")"};
                }
                return EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"};
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
                return EngineError{214, "Vulkan queue normalize+quantize/download failed (error " +
                                        std::to_string(submit_ret) + ")"};
            }
        }

        return {};
#else
        (void)tasks;
        (void)cancel_flag;
        return EngineError{200, "Vulkan not available"};
#endif
    }

    auto process_tiles_gpu(
        const float* input_data,
        float* output_data,
        int input_width,
        int input_height,
        int channels,
        const std::vector<TileRegion>& regions,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> override {
#if NCNN_VULKAN
        if (!supports_gpu_tile_pipeline()) {
            return EngineError{201, "GPU tile pipeline not available"};
        }
        if (channels <= 0 || input_width <= 0 || input_height <= 0) {
            return EngineError{202, "Invalid input dimensions"};
        }
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        const int out_width = input_width * scale_factor_;
        const int out_height = input_height * scale_factor_;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        const size_t out_total_pixels = static_cast<size_t>(out_width) * out_height;
        if (out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return EngineError{203, "Output image too large for Vulkan tile pipeline"};
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        struct VkAllocGuard {
            ncnn::VulkanDevice* dev = nullptr;
            ncnn::VkAllocator* blob = nullptr;
            ncnn::VkAllocator* staging = nullptr;
            ~VkAllocGuard() {
                if (dev != nullptr) {
                    if (blob) dev->reclaim_blob_allocator(blob);
                    if (staging) dev->reclaim_staging_allocator(staging);
                }
            }
        } alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

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
                return EngineError{204, "Vulkan clear failed (error " + std::to_string(submit_ret) + ")"};
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
        const size_t tiles_per_submit = tiles_per_cmd * 4;
        const size_t in_total_full = static_cast<size_t>(input_width) * input_height * channels;
        if (in_total_full > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return EngineError{205, "Input image too large for Vulkan tile pipeline"};
        }
        if (normalize_quantize_output_pipeline_ == nullptr) {
            return EngineError{212, "Vulkan normalize+quantize pipeline not available"};
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
                return EngineError{210, "Vulkan input upload failed (error " + std::to_string(submit_ret) + ")"};
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
                return EngineError{206, "Invalid tile dimensions"};
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
                return EngineError{207, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"};
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return EngineError{208, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"};
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                return EngineError{209, "Vulkan output shape mismatch"};
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
            return EngineError{211, "GPU crop pipeline unavailable"};
        }

        if (use_gpu_crop) {
            size_t region_idx = 0;
            while (region_idx < regions.size()) {
                if (cancel_flag) {
                    return EngineError{3, "Processing cancelled"};
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
                        return EngineError{206, "Invalid tile dimensions"};
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
                        return record_result.error();
                    }
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    return EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"};
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
                        return EngineError{206, "Invalid tile dimensions"};
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
                        return record_result.error();
                    }
                    batch_keepalive.emplace_back(in_gpu_hwc);
                }

                int submit_ret = cmd.submit_and_wait();
                if (submit_ret != 0) {
                    return EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"};
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
                return EngineError{214, "Vulkan normalize/download failed (error " + std::to_string(submit_ret) + ")"};
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
        return EngineError{200, "Vulkan not available"};
#endif
    }

    auto process_tiles_gpu_u8(
        const uint8_t* input_data,
        uint8_t* output_data,
        int input_width,
        int input_height,
        int channels,
        const std::vector<TileRegion>& regions,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> override {
#if NCNN_VULKAN
        if (!supports_gpu_tile_pipeline_u8()) {
            return EngineError{201, "GPU u8 tile pipeline not available"};
        }
        if (channels <= 0 || input_width <= 0 || input_height <= 0) {
            return EngineError{202, "Invalid input dimensions"};
        }
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        const int out_width = input_width * scale_factor_;
        const int out_height = input_height * scale_factor_;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        const size_t out_total_pixels = static_cast<size_t>(out_width) * out_height;
        if (out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return EngineError{203, "Output image too large for Vulkan tile pipeline"};
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        struct VkAllocGuard {
            ncnn::VulkanDevice* dev = nullptr;
            ncnn::VkAllocator* blob = nullptr;
            ncnn::VkAllocator* staging = nullptr;
            ~VkAllocGuard() {
                if (dev != nullptr) {
                    if (blob) dev->reclaim_blob_allocator(blob);
                    if (staging) dev->reclaim_staging_allocator(staging);
                }
            }
        } alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

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
                return EngineError{204, "Vulkan clear failed (error " + std::to_string(submit_ret) + ")"};
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
        const size_t tiles_per_submit = tiles_per_cmd * 4;
        const size_t in_total_full = static_cast<size_t>(input_width) * input_height * channels;
        if (in_total_full > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return EngineError{205, "Input image too large for Vulkan tile pipeline"};
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
                return EngineError{212, "Vulkan u8 input upload failed (error " + std::to_string(submit_ret) + ")"};
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
                return EngineError{206, "Invalid tile dimensions"};
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
                return EngineError{207, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"};
            }

            ncnn::VkMat out_gpu;
            if (output_idx >= 0) {
                ret = ex.extract(output_idx, out_gpu, cmd);
            } else {
                ret = ex.extract("output", out_gpu, cmd);
                if (ret != 0) {
                    ret = ex.extract("out", out_gpu, cmd);
                    if (ret != 0) {
                        return EngineError{208, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"};
                    }
                }
            }

            if (out_gpu.elempack != 1) {
                ncnn::VkMat out_unpacked;
                vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
                out_gpu = out_unpacked;
            }

            if (out_gpu.w != tile_out_full_w || out_gpu.h != tile_out_full_h || out_gpu.c != channels) {
                return EngineError{209, "Vulkan output shape mismatch"};
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
            return EngineError{213, "GPU u8 crop pipeline unavailable"};
        }

        if (use_gpu_crop) {
            LOG_INFO("Engine", "Using GPU tile pipeline with crop u8 preprocessing");
            
            size_t region_idx = 0;
            while (region_idx < regions.size()) {
                if (cancel_flag) {
                    return EngineError{3, "Processing cancelled"};
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
                        return EngineError{206, "Invalid tile dimensions"};
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
                        return record_result.error();
                    }
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
                        return EngineError{214, "Vulkan normalize+quantize/download failed (error " +
                                                std::to_string(submit_ret) + ")"};
                    }
                    return EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"};
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
                        return EngineError{206, "Invalid tile dimensions"};
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
                        return record_result.error();
                    }
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
                        return EngineError{214, "Vulkan normalize+quantize/download failed (error " +
                                                std::to_string(submit_ret) + ")"};
                    }
                    return EngineError{210, "Vulkan submit failed (error " + std::to_string(submit_ret) + ")"};
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
                return EngineError{214, "Vulkan normalize+quantize/download failed (error " + std::to_string(submit_ret) + ")"};
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
        return EngineError{200, "Vulkan not available"};
#endif
    }


    auto benchmark(int test_tile_size) -> float override {
        if (!initialized_ || !model_loaded_) return 0.0f;
        
        std::vector<float> input(static_cast<size_t>(test_tile_size) * test_tile_size * 3, 0.5f);
        std::vector<float> output(static_cast<size_t>(test_tile_size) * test_tile_size * 3 * scale_factor_ * scale_factor_);
        
        auto result = crossscale::benchmark("tile_process", [&]() {
            std::atomic<bool> cancel = false;
            process(input.data(), output.data(), test_tile_size, test_tile_size, 3, cancel);
        }, 5, 2);
        
        return 1000.0f / static_cast<float>(result.avg_time_ms);
    }
    
    void unload_model() override {
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
    
    DeviceType current_device() const override { return current_device_; }
    bool is_initialized() const override { return initialized_; }
    std::string model_name() const override { return model_name_; }
    int get_scale_factor() const override { return scale_factor_; }

    auto get_memory_info() const -> std::pair<size_t, size_t> override {
#if defined(__linux__)
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            size_t total = info.totalram * info.mem_unit / (1024 * 1024);
            size_t avail = info.freeram * info.mem_unit / (1024 * 1024);
            return {total, avail};
        }
#elif defined(_WIN32)
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            return {status.ullTotalPhys / (1024 * 1024),
                    status.ullAvailPhys / (1024 * 1024)};
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
            // 简化：total 不精确
            total = avail * 2; // 近似
            return {total, avail};
        }
#endif
        return {0, 0};
    }

private:
#if NCNN_VULKAN
    auto format_vulkan_runtime_snapshot(
        std::string_view stage,
        int width = -1,
        int height = -1,
        int channels = -1,
        size_t submit_budget = 0,
        size_t submit_budget_cap = 0,
        size_t submit_memory_budget_mb = 0,
        double avg_tile_pixels = -1.0
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

    bool init_gpu_layout_pipelines() {
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
                if (ncnn::compile_spirv_module(kHwcToChwComp, net_->opt, spirv_hwc_to_chw) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC->CHW shader");
                    return false;
                }
            }
            if (spirv_chw_to_hwc.empty()) {
                if (ncnn::compile_spirv_module(kChwToHwcComp, net_->opt, spirv_chw_to_hwc) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan CHW->HWC shader");
                    return false;
                }
            }
            if (spirv_hwc_u8_to_chw.empty()) {
                if (ncnn::compile_spirv_module(kHwcU8ToChwComp, net_->opt, spirv_hwc_u8_to_chw) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC-u8->CHW shader");
                    u8_shader_ready = false;
                }
            }
            if (spirv_chw_to_hwc_u8.empty()) {
                if (ncnn::compile_spirv_module(kChwToHwcU8Comp, net_->opt, spirv_chw_to_hwc_u8) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan CHW->HWC-u8 shader");
                    u8_shader_ready = false;
                }
            }
            if (spirv_hwc_to_u8.empty()) {
                if (ncnn::compile_spirv_module(kHwcFloatToU8Comp, net_->opt, spirv_hwc_to_u8) != 0) {
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

    bool init_gpu_crop_pipeline() {
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
                if (ncnn::compile_spirv_module(kHwcCropToChwComp, net_->opt, spirv_crop) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan HWC crop shader");
                    return false;
                }
            }
            if (spirv_crop_u8.empty()) {
                if (ncnn::compile_spirv_module(kHwcCropU8ToChwComp, net_->opt, spirv_crop_u8) != 0) {
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

    bool init_gpu_tile_pipelines() {
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
                if (ncnn::compile_spirv_module(kClearComp, net_->opt, spirv_clear) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan clear shader");
                    return false;
                }
            }
            if (spirv_blend.empty()) {
                if (ncnn::compile_spirv_module(kBlendTileComp, net_->opt, spirv_blend) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan blend shader");
                    return false;
                }
            }
            if (spirv_normalize.empty()) {
                if (ncnn::compile_spirv_module(kNormalizeByWeightComp, net_->opt, spirv_normalize) != 0) {
                    LOG_WARNING("Engine", "Failed to compile Vulkan normalize shader");
                    return false;
                }
            }
            if (spirv_normalize_quantize.empty()) {
                if (ncnn::compile_spirv_module(kNormalizeQuantizeByWeightComp, net_->opt, spirv_normalize_quantize) != 0) {
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

    auto process_vulkan_with_gpu_layout(
        const float* input,
        float* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
        if (vkdev_ == nullptr || preproc_hwc_to_chw_ == nullptr || postproc_chw_to_hwc_ == nullptr) {
            return EngineError{10, "Vulkan layout pipeline not initialized"};
        }
        if (channels <= 0 || width <= 0 || height <= 0) {
            return EngineError{11, "Invalid input dimensions"};
        }
        if (channels != 1 && channels != 3 && channels != 4) {
            return EngineError{12, "Unsupported channel count for Vulkan layout path"};
        }
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;
        const size_t in_total = static_cast<size_t>(width) * height * channels;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        if (in_total > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return EngineError{13, "Image too large for Vulkan layout path"};
        }

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        struct VkAllocGuard {
            ncnn::VulkanDevice* dev = nullptr;
            ncnn::VkAllocator* blob = nullptr;
            ncnn::VkAllocator* staging = nullptr;
            ~VkAllocGuard() {
                if (dev != nullptr) {
                    if (blob) dev->reclaim_blob_allocator(blob);
                    if (staging) dev->reclaim_staging_allocator(staging);
                }
            }
        } alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

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
            return EngineError{3, "Processing cancelled"};
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
            return EngineError{14, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"};
        }

        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        ncnn::VkMat out_gpu;
        if (output_idx >= 0) {
            ret = ex.extract(output_idx, out_gpu, cmd);
        } else {
            ret = ex.extract("output", out_gpu, cmd);
            if (ret != 0) {
                ret = ex.extract("out", out_gpu, cmd);
                if (ret != 0) {
                    return EngineError{15, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"};
                }
            }
        }

        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        if (out_gpu.elempack != 1) {
            ncnn::VkMat out_unpacked;
            vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
            out_gpu = out_unpacked;
        }

        if (out_gpu.w != out_width || out_gpu.h != out_height || out_gpu.c != channels) {
            return EngineError{16, "Vulkan output shape mismatch"};
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
            return EngineError{3, "Processing cancelled"};
        }

        ncnn::Mat out_cpu(static_cast<int>(out_total), output, 4u);
        cmd.record_clone(out_gpu_hwc, out_cpu, opt);

        ret = cmd.submit_and_wait();
        if (ret != 0) {
            return EngineError{17, "Vulkan submit failed (error " + std::to_string(ret) + ")"};
        }

        return {};
    }

    auto process_vulkan_with_gpu_layout_u8(
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
            return EngineError{19, "Vulkan u8 layout pipeline not initialized; " + detail};
        }
        if (channels <= 0 || width <= 0 || height <= 0) {
            return EngineError{11, "Invalid input dimensions"};
        }
        if (channels != 1 && channels != 3 && channels != 4) {
            return EngineError{12, "Unsupported channel count for Vulkan layout path"};
        }
        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;
        const size_t in_total = static_cast<size_t>(width) * height * channels;
        const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;
        if (in_total > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            out_total > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return EngineError{13, "Image too large for Vulkan layout path"};
        }
        LOG_INFO("Engine", "Starting Vulkan u8 layout path: " + format_vulkan_runtime_snapshot("u8_layout_begin", width, height, channels));

        ncnn::VkAllocator* blob_vkallocator = vkdev_->acquire_blob_allocator();
        ncnn::VkAllocator* staging_vkallocator = vkdev_->acquire_staging_allocator();
        struct VkAllocGuard {
            ncnn::VulkanDevice* dev = nullptr;
            ncnn::VkAllocator* blob = nullptr;
            ncnn::VkAllocator* staging = nullptr;
            ~VkAllocGuard() {
                if (dev != nullptr) {
                    if (blob) dev->reclaim_blob_allocator(blob);
                    if (staging) dev->reclaim_staging_allocator(staging);
                }
            }
        } alloc_guard{vkdev_, blob_vkallocator, staging_vkallocator};

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
            return EngineError{210, "Failed to allocate Vulkan input CHW buffer; " + detail};
        }

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
            return EngineError{3, "Processing cancelled"};
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
            return EngineError{14, "Failed to set Vulkan input (error " + std::to_string(ret) + ")"};
        }

        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        ncnn::VkMat out_gpu;
        if (output_idx >= 0) {
            ret = ex.extract(output_idx, out_gpu, cmd);
        } else {
            ret = ex.extract("output", out_gpu, cmd);
            if (ret != 0) {
                ret = ex.extract("out", out_gpu, cmd);
                if (ret != 0) {
                    return EngineError{15, "Failed to extract Vulkan output (error " + std::to_string(ret) + ")"};
                }
            }
        }

        if (cancel_flag) {
            return EngineError{3, "Processing cancelled"};
        }

        if (out_gpu.elempack != 1) {
            ncnn::VkMat out_unpacked;
            vkdev_->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
            out_gpu = out_unpacked;
        }

        if (out_gpu.w != out_width || out_gpu.h != out_height || out_gpu.c != channels) {
            return EngineError{16, "Vulkan output shape mismatch"};
        }

        ncnn::VkMat out_gpu_hwc_u8;
        out_gpu_hwc_u8.create(static_cast<int>(out_total), 1u, blob_vkallocator);
        if (out_gpu_hwc_u8.empty()) {
            const std::string detail = format_vulkan_runtime_snapshot("u8_out_gpu_hwc_alloc_failed", width, height, channels);
            LOG_ERROR("Engine", "Failed to allocate Vulkan output u8 buffer: " + detail);
            return EngineError{211, "Failed to allocate Vulkan output u8 buffer; " + detail};
        }

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
            return EngineError{3, "Processing cancelled"};
        }

        ncnn::Mat out_cpu(static_cast<int>(out_total), output, 1u);
        cmd.record_clone(out_gpu_hwc_u8, out_cpu, opt);

        ret = cmd.submit_and_wait();
        if (ret != 0) {
            const std::string detail = format_vulkan_runtime_snapshot("u8_submit_failed", width, height, channels);
            LOG_ERROR("Engine", "Vulkan u8 submit failed error=" + std::to_string(ret) + ", " + detail);
            return EngineError{17, "Vulkan submit failed (error " + std::to_string(ret) + "); " + detail};
        }

        LOG_INFO("Engine", "Finished Vulkan u8 layout path: " + format_vulkan_runtime_snapshot("u8_layout_done", width, height, channels));

        return {};
    }
#endif

    bool try_init_vulkan() {
#if NCNN_VULKAN
        int max_attempts = 6;
        if (const char* retries_env = std::getenv("CROSSSCALE_GPU_INIT_RETRIES")) {
            try {
                max_attempts = std::stoi(retries_env);
            } catch (...) {
                max_attempts = 6;
            }
        }
        max_attempts = std::clamp(max_attempts, 1, 12);

        int retry_delay_ms = 200;
        if (const char* delay_env = std::getenv("CROSSSCALE_GPU_INIT_DELAY_MS")) {
            try {
                retry_delay_ms = std::stoi(delay_env);
            } catch (...) {
                retry_delay_ms = 200;
            }
        }
        retry_delay_ms = std::clamp(retry_delay_ms, 50, 2000);

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            bool attempt_ok = false;
            {
                // ncnn GPU instance creation/destruction is process-global. Serialize retries
                // so one engine does not tear down Vulkan state while another is probing.
                std::lock_guard<std::mutex> lock(g_vulkan_init_mutex);

                ncnn::create_gpu_instance();
                int gpu_count = ncnn::get_gpu_count();
                if (gpu_count > 0) {
                    int best_gpu_idx = -1;
                    int best_score = std::numeric_limits<int>::min();

                    for (int i = 0; i < gpu_count; ++i) {
                        const auto& info = ncnn::get_gpu_info(i);
                        const std::string gpu_name = info.device_name() ? info.device_name() : ("GPU-" + std::to_string(i));

                        int score = 0;
                        // Prefer discrete GPU strongly, then integrated.
                        if (info.type() == 0) score += 10000;
                        else if (info.type() == 1) score += 1000;
                        else score += 100;

                        score += static_cast<int>(info.compute_queue_count()) * 10;
                        if (info.support_fp16_arithmetic()) score += 100;
                        if (info.support_fp16_storage()) score += 50;
                        if (info.support_fp16_packed()) score += 50;

                        LOG_INFO("Engine", "Vulkan candidate #" + std::to_string(i) +
                                " name=" + gpu_name +
                                " type=" + std::to_string(info.type()) +
                                " compute_queues=" + std::to_string(info.compute_queue_count()) +
                                " score=" + std::to_string(score));

                        if (score > best_score) {
                            best_score = score;
                            best_gpu_idx = i;
                        }
                    }

                    if (best_gpu_idx >= 0) {
                        vkdev_ = ncnn::get_gpu_device(best_gpu_idx);
                        if (vkdev_ != nullptr) {
                            LOG_INFO("Engine", "Selected Vulkan GPU index " + std::to_string(best_gpu_idx));
                            attempt_ok = true;
                        }
                    }
                }

            }

            if (attempt_ok) {
                return true;
            }

            if (attempt < max_attempts) {
                LOG_WARNING("Engine", "Vulkan init attempt " + std::to_string(attempt) +
                                      " failed, retrying in " + std::to_string(retry_delay_ms) + "ms");
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
                retry_delay_ms = std::min(retry_delay_ms * 2, 1500);
            }
        }
#endif
        return false;
    }
    
    void detect_model_info(const std::string& param_path) {
        // Parse param file to detect input/output shapes
        std::ifstream file(param_path);
        if (!file.is_open()) {
            LOG_WARNING("Engine", "Could not parse param file for model info");
            return;
        }
        
        std::string line;
        int input_width = 0, input_height = 0, input_channels = 0;
        int output_width = 0, output_height = 0;
        
        while (std::getline(file, line)) {
            // Look for Input layer to get input dimensions
            if (line.find("Input") != std::string::npos) {
                // Parse input dimensions from the line
                // Format: Input	layer_name	bottom_count top_count blob_name 0=W 1=H 2=C
                size_t pos = line.find("0=");
                if (pos != std::string::npos) {
                    input_width = std::stoi(line.substr(pos + 2));
                }
                pos = line.find("1=");
                if (pos != std::string::npos) {
                    input_height = std::stoi(line.substr(pos + 2));
                }
                pos = line.find("2=");
                if (pos != std::string::npos) {
                    input_channels = std::stoi(line.substr(pos + 2));
                }
            }
        }
        
        // Try to infer scale factor from model architecture
        // Many SR models have scale in the param file or we can compute from known shapes
        // For now, use default or try to detect from common patterns
        
        // Reset file to search for output dimensions or scale indicators
        file.clear();
        file.seekg(0);
        
        while (std::getline(file, line)) {
            // Check for scale in layer names (common in SR models)
            if (line.find("upsample") != std::string::npos ||
                line.find("Upsample") != std::string::npos ||
                line.find("interp") != std::string::npos) {
                // Check for scale factor in the line
                size_t pos = line.find("scale_factor=");
                if (pos != std::string::npos) {
                    scale_factor_ = std::stoi(line.substr(pos + 13));
                    break;
                }
                pos = line.find("6=");  // Often scale is param 6 in Interp layers
                if (pos != std::string::npos) {
                    float scale = std::stof(line.substr(pos + 2));
                    if (scale > 1.0f && scale < 10.0f) {
                        scale_factor_ = static_cast<int>(scale);
                        break;
                    }
                }
            }
        }
        
        // If we couldn't detect, try inferring from model metadata or use default
        if (input_channels > 0) {
            input_channels_ = input_channels;
        }
        
        LOG_INFO("Engine", "Detected input: " + std::to_string(input_width) + "x" + 
                 std::to_string(input_height) + "x" + std::to_string(input_channels_) + 
                 ", scale: " + std::to_string(scale_factor_));
    }
    
    std::unique_ptr<ncnn::Net> net_;
#if NCNN_VULKAN
    ncnn::VulkanDevice* vkdev_ = nullptr;
    std::unique_ptr<ncnn::Pipeline> preproc_hwc_to_chw_;
    std::unique_ptr<ncnn::Pipeline> preproc_hwc_u8_to_chw_;
    std::unique_ptr<ncnn::Pipeline> preproc_hwc_crop_to_chw_;
    std::unique_ptr<ncnn::Pipeline> preproc_hwc_crop_u8_to_chw_;
    std::unique_ptr<ncnn::Pipeline> postproc_chw_to_hwc_;
    std::unique_ptr<ncnn::Pipeline> postproc_chw_to_hwc_u8_;
    std::unique_ptr<ncnn::Pipeline> quantize_hwc_to_u8_;
    std::unique_ptr<ncnn::Pipeline> clear_output_pipeline_;
    std::unique_ptr<ncnn::Pipeline> blend_tile_pipeline_;
    std::unique_ptr<ncnn::Pipeline> normalize_output_pipeline_;
    std::unique_ptr<ncnn::Pipeline> normalize_quantize_output_pipeline_;
    bool gpu_layout_pipeline_ready_ = false;
    bool gpu_layout_u8_pipeline_ready_ = false;
    bool gpu_crop_pipeline_ready_ = false;
    bool gpu_crop_u8_pipeline_ready_ = false;
    bool gpu_tile_pipeline_ready_ = false;
#endif
    DeviceType current_device_ = DeviceType::CPU;
    bool initialized_ = false;
    bool model_loaded_ = false;
    std::string model_name_;
    int scale_factor_ = 2;
    int input_channels_ = 3;
};

std::unique_ptr<Engine> create_ncnn_engine_impl() {
    return std::make_unique<NCNNEngineImpl>();
}

} // namespace crossscale
