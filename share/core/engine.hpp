#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include <future>
#include <cstdint>
#include "core/tile_types.hpp"
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

// Error code reserved for "allocation refused by preflight or the device ran
// out of memory mid-run". Callers use is_oom_error() to trigger the shrink /
// fallback ladder instead of failing the whole job.
inline constexpr int kErrOutOfMemory = 230;

inline bool is_oom_error(const EngineError& e) {
    return e.code == kErrOutOfMemory || e.message.find("out of memory") != std::string::npos;
}

struct GPUTileTaskU8 {
    const uint8_t* input_data = nullptr;
    uint8_t* output_data = nullptr;
    int input_width = 0;
    int input_height = 0;
    int channels = 0;
    const std::vector<TileRegion>* regions = nullptr;
    TileConfig config{};
    std::function<void(const ProcessingProgress&)> progress_callback{};
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

    virtual auto process_u8(
        const uint8_t* input,
        uint8_t* output,
        int width,
        int height,
        int channels,
        const std::atomic<bool>& cancel_flag = {}
    ) -> expected<void, EngineError> {
        (void)input;
        (void)output;
        (void)width;
        (void)height;
        (void)channels;
        (void)cancel_flag;
        return std::unexpected(EngineError{102, "u8 process path not supported"});
    }

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
            return std::unexpected(EngineError{100, "Batch input/output size mismatch"});
        }
        // Vulkan backend is not safe for concurrent submits from multiple async tasks.
        // Keep GPU batch execution serialized and let upper layers pipeline CPU prep.
        if (current_device() == DeviceType::GPU_VULKAN) {
            for (size_t i = 0; i < inputs.size(); ++i) {
                auto result = process(inputs[i], outputs[i], width, height, channels, cancel_flag);
                if (!result) return result;
            }
            return {};
        }

        if (inputs.size() == 1) {
            return process(inputs[0], outputs[0], width, height, channels, cancel_flag);
        }

        std::vector<std::future<expected<void, EngineError>>> futures;
        futures.reserve(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            futures.push_back(std::async(
                std::launch::async,
                [this, i, &inputs, &outputs, width, height, channels, &cancel_flag]() {
                    return process(inputs[i], outputs[i], width, height, channels, cancel_flag);
                }
            ));
        }

        for (auto& fut : futures) {
            auto result = fut.get();
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

    // Override the scale factor detected from the param file. The authoritative
    // source is the model filename (parsed by model_manager, e.g. "x4" -> 4),
    // which is more reliable than the engine's heuristic scan of the param
    // content (which mishandles multi-stage Interp and case sensitivity).
    // Called after load_model(). No-op for engines without a mutable scale.
    virtual void set_scale_factor(int /*scale*/) {}

    // Cap inference threads for CPU engines. A pool of engines divides the
    // machine's physical cores among its engines so N concurrent engines do
    // not each spawn a thread per logical CPU. Must be called before
    // load_model(); <= 0 or unset keeps the engine default.
    virtual void set_inference_threads(int /*threads*/) {}

    // Get recommended tile size based on GPU VRAM (0 = not available)
    virtual int get_recommended_tile_size() const { return 256; }

    // Device memory available for allocations, in MB (0 = unknown). Used by
    // the OOM preflight to size the full-output buffers and by callers to
    // pick a default worker count. Honors CROSSSCALE_GPU_FORCE_BUDGET_MB
    // (testing: simulate a weaker device).
    virtual size_t get_device_memory_budget_mb() const { return 0; }

    // Get memory info: {total_mb, available_mb}
    virtual auto get_memory_info() const -> std::pair<size_t, size_t> {
        return {0, 0};
    }

    virtual bool supports_gpu_tile_pipeline() const { return false; }
    virtual bool supports_gpu_tile_pipeline_u8() const { return false; }
    virtual bool supports_gpu_tile_pipeline_u8_queue() const { return false; }

    virtual auto process_tiles_gpu(
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
        (void)input_data;
        (void)output_data;
        (void)input_width;
        (void)input_height;
        (void)channels;
        (void)regions;
        (void)config;
        (void)progress_callback;
        (void)cancel_flag;
        return std::unexpected(EngineError{200, "GPU tile pipeline not supported"});
    }

    virtual auto process_tiles_gpu_u8(
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
        (void)input_data;
        (void)output_data;
        (void)input_width;
        (void)input_height;
        (void)channels;
        (void)regions;
        (void)config;
        (void)progress_callback;
        (void)cancel_flag;
        return std::unexpected(EngineError{201, "GPU u8 tile pipeline not supported"});
    }

    virtual auto process_tiles_gpu_u8_queue(
        const std::vector<GPUTileTaskU8>& tasks,
        const std::atomic<bool>& cancel_flag
    ) -> expected<void, EngineError> {
        for (const auto& task : tasks) {
            if (task.regions == nullptr) {
                return std::unexpected(EngineError{202, "GPU u8 queue task has null regions"});
            }
            auto result = process_tiles_gpu_u8(
                task.input_data,
                task.output_data,
                task.input_width,
                task.input_height,
                task.channels,
                *task.regions,
                task.config,
                task.progress_callback,
                cancel_flag
            );
            if (!result.has_value()) {
                return result;
            }
        }
        return {};
    }
};

std::unique_ptr<Engine> create_ncnn_engine();

} // namespace crossscale
