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
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>

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
            unsigned int hw_threads = std::thread::hardware_concurrency();
            int tuned_threads = 2;
            if (hw_threads > 0) {
                tuned_threads = std::clamp(static_cast<int>(hw_threads / 4), 1, 4);
            }
            net_->opt.num_threads = tuned_threads;
        } else {
            net_->opt.use_vulkan_compute = false;
        }
#else
        net_->opt.use_vulkan_compute = false;
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
        } else {
            gpu_layout_pipeline_ready_ = false;
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
        postproc_chw_to_hwc_.reset();
        gpu_layout_pipeline_ready_ = false;
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
        static std::vector<uint32_t> spirv_chw_to_hwc;
        static std::mutex spirv_mutex;

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
    std::unique_ptr<ncnn::Pipeline> postproc_chw_to_hwc_;
    bool gpu_layout_pipeline_ready_ = false;
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
