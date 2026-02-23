#include "ncnn_engine.hpp"
#include "utils/logger.hpp"
#include "utils/benchmark.hpp"
#include <ncnn/net.h>
#include <ncnn/cpu.h>
#if NCNN_VULKAN
#include <ncnn/gpu.h>
#include <ncnn/command.h>
#include <ncnn/pipeline.h>
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

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) buffer dst_blob { float dst_data[]; };

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
} p;

float blend_weight(int pos, int size, int overlap)
{
    if (overlap <= 0)
        return 1.0;
    float w = 1.0;
    if (pos < overlap)
        w = float(pos + 1) / float(overlap);
    else if (pos >= size - overlap)
        w = float(size - pos) / float(overlap);
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

    const float wx = blend_weight(gx, p.tile_w, p.overlap);
    const float wy = blend_weight(gy, p.tile_h, p.overlap);
    const float w = wx * wy;

    const int src_x = p.src_offset_x + gx;
    const int src_y = p.src_offset_y + gy;
    if (src_x < 0 || src_x >= p.src_w || src_y < 0 || src_y >= p.src_h)
        return;

    const int src_idx = (src_y * p.src_w + src_x) * p.channels + gz;
    const int dst_idx = (out_py * p.out_w + out_px) * p.channels + gz;
    dst_data[dst_idx] += src_data[src_idx] * w;
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
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr) {
            net_->set_vulkan_device(vkdev_);
            net_->opt.use_vulkan_compute = true;
            net_->opt.use_fp16_packed = true;
            net_->opt.use_fp16_storage = true;
            net_->opt.use_fp16_arithmetic = true;
            net_->opt.use_int8_storage = true;
            unsigned int hw_threads = std::thread::hardware_concurrency();
            int tuned_threads = 2;
            if (hw_threads > 0) {
                tuned_threads = std::clamp(static_cast<int>(hw_threads / 4), 1, 4);
            }
            net_->opt.num_threads = tuned_threads;
        } else {
            net_->opt.use_vulkan_compute = false;
            net_->opt.use_int8_storage = false;
        }
#else
        net_->opt.use_vulkan_compute = false;
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
            return EngineError{4, "Failed to load model weights: " + bin_path + " (error " + std::to_string(ret) + ")"};
        }

#if NCNN_VULKAN
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr) {
            gpu_layout_pipeline_ready_ = init_gpu_layout_pipelines();
            if (!gpu_layout_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU pre/post layout pipeline disabled, fallback to CPU layout conversion");
            }
            gpu_layout_u8_pipeline_ready_ = preproc_hwc_u8_to_chw_ != nullptr &&
                                            postproc_chw_to_hwc_u8_ != nullptr &&
                                            quantize_hwc_to_u8_ != nullptr;
            if (!gpu_layout_u8_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU u8 pre/post layout pipeline disabled, fallback to CPU u8 conversion");
            }
            gpu_crop_pipeline_ready_ = init_gpu_crop_pipeline();
            if (!gpu_crop_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU crop pipeline disabled, fallback to CPU tile extraction");
            }
            gpu_crop_u8_pipeline_ready_ = preproc_hwc_crop_u8_to_chw_ != nullptr;
            if (!gpu_crop_u8_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU u8 crop pipeline disabled, fallback to CPU tile extraction");
            }
            gpu_tile_pipeline_ready_ = init_gpu_tile_pipelines();
            if (!gpu_tile_pipeline_ready_) {
                LOG_WARNING("Engine", "GPU tile blend pipeline disabled, fallback to CPU tile blending");
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
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr && gpu_layout_pipeline_ready_) {
            auto vk_result = process_vulkan_with_gpu_layout(input, output, width, height, channels, cancel_flag);
            if (vk_result.has_value()) {
                return {};
            }

            LOG_WARNING("Engine", "Vulkan layout pipeline failed, fallback to CPU layout path: " + vk_result.error().message);
        }
#endif
        
        int out_width = width * scale_factor_;
        int out_height = height * scale_factor_;
        
        // Create input Mat from float array (interleaved RGB -> planar)
        // ncnn expects planar format: RRR...GGG...BBB...
        ncnn::Mat in(width, height, channels);
        if (in.empty()) {
            return EngineError{5, "Failed to create input Mat"};
        }
        
        // Convert interleaved float RGB to planar format
        for (int c = 0; c < channels; ++c) {
            for (int h = 0; h < height; ++h) {
                for (int w = 0; w < width; ++w) {
                    size_t src_idx = (static_cast<size_t>(h) * width + w) * channels + c;
                    size_t dst_idx = static_cast<size_t>(c) * height * width + h * width + w;
                    in[dst_idx] = input[src_idx];
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
        
        // Copy output to float array
        // ncnn::Mat stores as planar (channel, height, width)
        for (int c = 0; c < channels; ++c) {
            for (int h = 0; h < out_height; ++h) {
                for (int w = 0; w < out_width; ++w) {
                    size_t dst_idx = (static_cast<size_t>(h) * out_width + w) * channels + c;
                    size_t src_idx = static_cast<size_t>(c) * out_height * out_width + h * out_width + w;
                    output[dst_idx] = out[src_idx];
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
        if (current_device_ == DeviceType::GPU_VULKAN && vkdev_ != nullptr && gpu_layout_u8_pipeline_ready_) {
            auto vk_result = process_vulkan_with_gpu_layout_u8(input, output, width, height, channels, cancel_flag);
            if (vk_result.has_value()) {
                return {};
            }

            LOG_WARNING("Engine", "Vulkan u8 layout pipeline failed, fallback to CPU u8 conversion path: " +
                                      vk_result.error().message);
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
            const float v = std::clamp(output_float[i] * 255.0f, 0.0f, 255.0f);
            output[i] = static_cast<uint8_t>(v);
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

        {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::VkMat> bindings(1);
            bindings[0] = out_gpu_hwc;
            std::vector<ncnn::vk_constant_type> constants(1);
            constants[0].i = static_cast<int>(out_total);
            ncnn::VkMat dispatcher;
            dispatcher.w = static_cast<int>(out_total);
            dispatcher.h = 1;
            dispatcher.c = 1;
            cmd.record_pipeline(clear_output_pipeline_.get(), bindings, constants, dispatcher);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return EngineError{204, "Vulkan clear failed (error " + std::to_string(submit_ret) + ")"};
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
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

        bool use_gpu_crop = gpu_crop_pipeline_ready_;
        ncnn::VkMat in_gpu_full;
        if (use_gpu_crop) {
            ncnn::VkCompute cmd(vkdev_);
            ncnn::Mat in_cpu_full(static_cast<int>(in_total_full), const_cast<float*>(input_data), 4u);
            cmd.record_clone(in_cpu_full, in_gpu_full, opt);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                LOG_WARNING("Engine", "Vulkan input upload failed, fallback to CPU tile extraction");
                use_gpu_crop = false;
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
                                       int tile_in_w,
                                       int tile_in_h) -> expected<void, EngineError> {
            const int tile_out_full_w = tile_in_w * scale_factor_;
            const int tile_out_full_h = tile_in_h * scale_factor_;
            const int tile_out_inner_w = static_cast<int>(region.width) * scale_factor_;
            const int tile_out_inner_h = static_cast<int>(region.height) * scale_factor_;
            const int src_offset_x = pad_info.pad_left * scale_factor_;
            const int src_offset_y = pad_info.pad_top * scale_factor_;
            const size_t tile_out_total = static_cast<size_t>(tile_out_full_w) * tile_out_full_h * channels;
            if (tile_out_total == 0) {
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

            ncnn::VkMat out_gpu_hwc_tile;
            out_gpu_hwc_tile.create(static_cast<int>(tile_out_total), 4u, blob_vkallocator);

            std::vector<ncnn::VkMat> post_bindings(2);
            post_bindings[0] = out_gpu;
            post_bindings[1] = out_gpu_hwc_tile;

            std::vector<ncnn::vk_constant_type> post_constants(4);
            post_constants[0].i = tile_out_full_w;
            post_constants[1].i = tile_out_full_h;
            post_constants[2].i = channels;
            post_constants[3].i = static_cast<int>(out_gpu.cstep);

            ncnn::VkMat post_dispatcher;
            post_dispatcher.w = tile_out_full_w;
            post_dispatcher.h = tile_out_full_h;
            post_dispatcher.c = channels;
            cmd.record_pipeline(postproc_chw_to_hwc_.get(), post_bindings, post_constants, post_dispatcher);

            std::vector<ncnn::VkMat> blend_bindings(2);
            blend_bindings[0] = out_gpu_hwc_tile;
            blend_bindings[1] = out_gpu_hwc;

            std::vector<ncnn::vk_constant_type> blend_constants(12);
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

            ncnn::VkMat blend_dispatcher;
            blend_dispatcher.w = tile_out_inner_w;
            blend_dispatcher.h = tile_out_inner_h;
            blend_dispatcher.c = channels;
            cmd.record_pipeline(blend_tile_pipeline_.get(), blend_bindings, blend_constants, blend_dispatcher);

            return {};
        };
        if (use_gpu_crop && preproc_hwc_crop_to_chw_ == nullptr) {
            use_gpu_crop = false;
        }

        if (use_gpu_crop) {
            size_t region_idx = 0;
            while (region_idx < regions.size()) {
                if (cancel_flag) {
                    return EngineError{3, "Processing cancelled"};
                }

                size_t batch_end = std::min(regions.size(), region_idx + tiles_per_cmd);
                ncnn::VkCompute cmd(vkdev_);
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

                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, tile_in_w, tile_in_h);
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
                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return record_result.error();
                    }
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
            ncnn::Mat out_cpu(static_cast<int>(out_total), output_data, 4u);
            cmd.record_clone(out_gpu_hwc, out_cpu, opt);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return EngineError{211, "Vulkan download failed (error " + std::to_string(submit_ret) + ")"};
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

        {
            ncnn::VkCompute cmd(vkdev_);
            std::vector<ncnn::VkMat> bindings(1);
            bindings[0] = out_gpu_hwc;
            std::vector<ncnn::vk_constant_type> constants(1);
            constants[0].i = static_cast<int>(out_total);
            ncnn::VkMat dispatcher;
            dispatcher.w = static_cast<int>(out_total);
            dispatcher.h = 1;
            dispatcher.c = 1;
            cmd.record_pipeline(clear_output_pipeline_.get(), bindings, constants, dispatcher);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return EngineError{204, "Vulkan clear failed (error " + std::to_string(submit_ret) + ")"};
            }
        }

        ProcessingProgress progress;
        progress.total_tiles = static_cast<int>(regions.size());
        progress.completed_tiles = 0;

        const size_t tiles_per_cmd = static_cast<size_t>(std::max(1, config.batch_size));
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
                LOG_WARNING("Engine", "Vulkan u8 input upload failed, fallback to CPU tile extraction");
                use_gpu_crop = false;
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
                                       int tile_in_w,
                                       int tile_in_h) -> expected<void, EngineError> {
            const int tile_out_full_w = tile_in_w * scale_factor_;
            const int tile_out_full_h = tile_in_h * scale_factor_;
            const int tile_out_inner_w = static_cast<int>(region.width) * scale_factor_;
            const int tile_out_inner_h = static_cast<int>(region.height) * scale_factor_;
            const int src_offset_x = pad_info.pad_left * scale_factor_;
            const int src_offset_y = pad_info.pad_top * scale_factor_;
            const size_t tile_out_total = static_cast<size_t>(tile_out_full_w) * tile_out_full_h * channels;
            if (tile_out_total == 0) {
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

            ncnn::VkMat out_gpu_hwc_tile;
            out_gpu_hwc_tile.create(static_cast<int>(tile_out_total), 4u, blob_vkallocator);

            std::vector<ncnn::VkMat> post_bindings(2);
            post_bindings[0] = out_gpu;
            post_bindings[1] = out_gpu_hwc_tile;

            std::vector<ncnn::vk_constant_type> post_constants(4);
            post_constants[0].i = tile_out_full_w;
            post_constants[1].i = tile_out_full_h;
            post_constants[2].i = channels;
            post_constants[3].i = static_cast<int>(out_gpu.cstep);

            ncnn::VkMat post_dispatcher;
            post_dispatcher.w = tile_out_full_w;
            post_dispatcher.h = tile_out_full_h;
            post_dispatcher.c = channels;
            cmd.record_pipeline(postproc_chw_to_hwc_.get(), post_bindings, post_constants, post_dispatcher);

            std::vector<ncnn::VkMat> blend_bindings(2);
            blend_bindings[0] = out_gpu_hwc_tile;
            blend_bindings[1] = out_gpu_hwc;

            std::vector<ncnn::vk_constant_type> blend_constants(12);
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

            ncnn::VkMat blend_dispatcher;
            blend_dispatcher.w = tile_out_inner_w;
            blend_dispatcher.h = tile_out_inner_h;
            blend_dispatcher.c = channels;
            cmd.record_pipeline(blend_tile_pipeline_.get(), blend_bindings, blend_constants, blend_dispatcher);

            return {};
        };

        if (use_gpu_crop && preproc_hwc_crop_u8_to_chw_ == nullptr) {
            use_gpu_crop = false;
        }

        if (use_gpu_crop) {
            size_t region_idx = 0;
            while (region_idx < regions.size()) {
                if (cancel_flag) {
                    return EngineError{3, "Processing cancelled"};
                }

                size_t batch_end = std::min(regions.size(), region_idx + tiles_per_cmd);
                ncnn::VkCompute cmd(vkdev_);
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

                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, tile_in_w, tile_in_h);
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
                    auto record_result = record_tile_outputs(cmd, region, pad_info, in_gpu_chw, tile_in_w, tile_in_h);
                    if (!record_result.has_value()) {
                        return record_result.error();
                    }
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

        if (quantize_hwc_to_u8_ == nullptr) {
            return EngineError{212, "Vulkan quantize pipeline not available"};
        }

        {
            ncnn::VkCompute cmd(vkdev_);
            ncnn::VkMat out_gpu_u8;
            out_gpu_u8.create(static_cast<int>(out_total), 1u, blob_vkallocator);

            std::vector<ncnn::VkMat> quant_bindings(2);
            quant_bindings[0] = out_gpu_hwc;
            quant_bindings[1] = out_gpu_u8;

            std::vector<ncnn::vk_constant_type> quant_constants(1);
            quant_constants[0].i = static_cast<int>(out_total);

            ncnn::VkMat quant_dispatcher;
            quant_dispatcher.w = static_cast<int>(out_total);
            quant_dispatcher.h = 1;
            quant_dispatcher.c = 1;
            cmd.record_pipeline(quantize_hwc_to_u8_.get(), quant_bindings, quant_constants, quant_dispatcher);

            ncnn::Mat out_cpu(static_cast<int>(out_total), output_data, 1u);
            cmd.record_clone(out_gpu_u8, out_cpu, opt);
            int submit_ret = cmd.submit_and_wait();
            if (submit_ret != 0) {
                return EngineError{213, "Vulkan u8 download failed (error " + std::to_string(submit_ret) + ")"};
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
            return EngineError{19, "Vulkan u8 layout pipeline not initialized"};
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

        ncnn::Mat in_cpu(static_cast<int>(in_total), const_cast<uint8_t*>(input), 1u);
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
            return EngineError{17, "Vulkan submit failed (error " + std::to_string(ret) + ")"};
        }

        return {};
    }
#endif

    bool try_init_vulkan() {
#if NCNN_VULKAN
        constexpr int kMaxAttempts = 3;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
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
                        return true;
                    }
                }
            }

            LOG_WARNING("Engine", "Vulkan init attempt " + std::to_string(attempt) +
                                  " failed, retrying");
            ncnn::destroy_gpu_instance();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
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
