#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include "utils/expected.hpp"

namespace crossscale {

enum class DeviceType {
    CPU,
    GPU_VULKAN
};

struct DeviceCapability {
    DeviceType type;
    std::string name;
    int compute_units;
    size_t memory_mb;
    float benchmark_score;
};

struct EngineError {
    int code;
    std::string message;
};

class Engine {
public:
    virtual ~Engine() = default;

    virtual auto initialize(DeviceType preferred = DeviceType::GPU_VULKAN)
        -> expected<void, EngineError> = 0;
    
    virtual auto list_devices() -> std::vector<DeviceCapability> = 0;
    
    virtual auto load_model(const std::string& param_path, const std::string& bin_path)
        -> expected<void, EngineError> = 0;
    
    virtual auto process(
        const float* input,
        float* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag = {}
    ) -> expected<void, EngineError> = 0;

    // Process a batch of tiles. All tiles must have same width, height, channels.
    // inputs: vector of input data pointers (each tile: interleaved HWC float)
    // outputs: vector of output data pointers (each tile: interleaved HWC float, pre-allocated)
    virtual auto process_batch(
        const std::vector<const float*>& inputs,
        const std::vector<float*>& outputs,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
        if (inputs.size() != outputs.size()) {
            return EngineError{100, "Batch input/output size mismatch"};
        }
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto result = process(inputs[i], outputs[i], width, height, channels, cancel_flag);
            if (!result) return result;
        }
        return {};
    }

    virtual auto benchmark(int test_tile_size = 256) -> float = 0;
    
    virtual void unload_model() = 0;
    
    virtual DeviceType current_device() const = 0;
    virtual bool is_initialized() const = 0;
    virtual std::string model_name() const = 0;
    
    virtual int get_scale_factor() const { return 2; }

    // Get memory info: {total_mb, available_mb}
    virtual auto get_memory_info() const -> std::pair<size_t, size_t> {
        return {0, 0};
    }
};

std::unique_ptr<Engine> create_ncnn_engine();

} // namespace crossscale
