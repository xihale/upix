#include "ncnn_engine.hpp"
#include "utils/logger.hpp"
#include "utils/benchmark.hpp"
#include <ncnn/net.h>
#include <ncnn/cpu.h>
#if NCNN_VULKAN
#include <ncnn/gpu.h>
#endif
#include <algorithm>
#include <cmath>
#include <thread>
#include <filesystem>
#include <fstream>

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
            net_->opt.use_vulkan_compute = true;
            net_->opt.use_fp16_packed = true;
            net_->opt.use_fp16_storage = true;
            net_->opt.use_fp16_arithmetic = true;
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
    bool try_init_vulkan() {
#if NCNN_VULKAN
        ncnn::create_gpu_instance();
        int gpu_count = ncnn::get_gpu_count();
        if (gpu_count > 0) {
            vkdev_ = ncnn::get_gpu_device(0);
            return vkdev_ != nullptr;
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
