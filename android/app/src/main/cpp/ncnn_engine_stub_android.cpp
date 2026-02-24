#include "core/engine.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace crossscale {

namespace {

class AndroidStubEngine final : public Engine {
public:
    auto initialize(DeviceType preferred) -> expected<void, EngineError> override {
        current_device_ = preferred;
        initialized_ = true;
        return {};
    }

    auto list_devices() -> std::vector<DeviceCapability> override {
        return {
            DeviceCapability{DeviceType::CPU, "Android CPU (Stub)", 1, 1024, 1.0f},
        };
    }

    auto load_model(const std::string& param_path, const std::string& bin_path)
        -> expected<void, EngineError> override {
        (void)bin_path;
        if (!initialized_) {
            return EngineError{1, "engine is not initialized"};
        }
        model_name_ = param_path;
        scale_factor_ = infer_scale_factor(param_path);
        model_loaded_ = true;
        return {};
    }

    auto process(
        const float* input,
        float* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> override {
        if (!initialized_) {
            return EngineError{2, "engine is not initialized"};
        }
        if (!model_loaded_) {
            return EngineError{3, "model is not loaded"};
        }
        if (input == nullptr || output == nullptr || width <= 0 || height <= 0 || channels <= 0) {
            return EngineError{4, "invalid input arguments"};
        }
        if (cancel_flag.load()) {
            return EngineError{5, "processing canceled"};
        }

        const int out_width = width * scale_factor_;
        const int out_height = height * scale_factor_;

        for (int y = 0; y < out_height; ++y) {
            const int src_y = std::clamp(y / scale_factor_, 0, height - 1);
            for (int x = 0; x < out_width; ++x) {
                const int src_x = std::clamp(x / scale_factor_, 0, width - 1);
                const int src_index = (src_y * width + src_x) * channels;
                const int dst_index = (y * out_width + x) * channels;
                for (int c = 0; c < channels; ++c) {
                    output[dst_index + c] = input[src_index + c];
                }
            }
        }
        return {};
    }

    auto benchmark(int test_tile_size) -> float override {
        (void)test_tile_size;
        return 1.0f;
    }

    void unload_model() override {
        model_loaded_ = false;
        model_name_.clear();
    }

    auto current_device() const -> DeviceType override {
        return current_device_;
    }

    auto is_initialized() const -> bool override {
        return initialized_;
    }

    auto model_name() const -> std::string override {
        return model_name_;
    }

    auto get_scale_factor() const -> int override {
        return scale_factor_;
    }

private:
    static auto infer_scale_factor(const std::string& model_path) -> int {
        if (model_path.find("x4") != std::string::npos || model_path.find("X4") != std::string::npos) {
            return 4;
        }
        if (model_path.find("x3") != std::string::npos || model_path.find("X3") != std::string::npos) {
            return 3;
        }
        if (model_path.find("x2") != std::string::npos || model_path.find("X2") != std::string::npos) {
            return 2;
        }
        return 2;
    }

    DeviceType current_device_ = DeviceType::CPU;
    bool initialized_ = false;
    bool model_loaded_ = false;
    std::string model_name_;
    int scale_factor_ = 2;
};

}  // namespace

std::unique_ptr<Engine> create_ncnn_engine_impl() {
    return std::make_unique<AndroidStubEngine>();
}

}  // namespace crossscale
