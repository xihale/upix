#pragma once

#include "engine.hpp"
#include <memory>

namespace ncnn {
    class Net;
    class VulkanDevice;
}

namespace crossscale {

class NCNNEngine : public Engine {
public:
    NCNNEngine();
    ~NCNNEngine() override;

    auto initialize(DeviceType preferred = DeviceType::GPU_VULKAN)
        -> expected<void, EngineError> override;
    
    auto list_devices() -> std::vector<DeviceCapability> override;
    
    auto load_model(const std::string& param_path, const std::string& bin_path)
        -> expected<void, EngineError> override;
    
    auto process(
        const float* input,
        float* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag = {}
    ) -> expected<void, EngineError> override;
    
    auto benchmark(int test_tile_size = 256) -> float override;
    
    void unload_model() override;
    
    DeviceType current_device() const override { return current_device_; }
    bool is_initialized() const override { return initialized_; }
    std::string model_name() const override { return model_name_; }

    auto get_scale_factor() const -> int { return scale_factor_; }
    auto get_tile_size() const -> int { return tile_size_; }

private:
    auto detect_vulkan_devices() -> std::vector<DeviceCapability>;
    auto infer_model_info() -> void;
    auto create_test_tile(int size) -> std::vector<float>;

    std::unique_ptr<ncnn::Net> net_;
    ncnn::VulkanDevice* vkdev_ = nullptr;
    
    DeviceType current_device_ = DeviceType::CPU;
    bool initialized_ = false;
    bool model_loaded_ = false;
    std::string model_name_;
    
    int scale_factor_ = 2;
    int tile_size_ = 256;
    int input_channels_ = 3;
};

} // namespace crossscale
