#pragma once

// Internal class declaration for the ncnn engine implementation. Extracted from
// ncnn_engine.cpp so that the GPU init / GPU pipeline split files can define
// member functions out-of-line. No logic lives here — declarations only.
//
// Includes: ncnn types are forward-declared to avoid pulling ncnn headers into
// every TU that references the class; the complete types are required only at
// member-variable destruction (in the .cpp that owns the definitions).

#include "engine.hpp"
#include <platform.h> // defines NCNN_VULKAN, needed by the member guards below
#include <memory>
#include <mutex>
#include <string>

// Forward declarations of ncnn types used by the class members / detail helpers.
namespace ncnn {
class Net;
class VulkanDevice;
class Pipeline;
class VkAllocator;
}

namespace upix {

#if NCNN_VULKAN
namespace detail {

// Vulkan init is global (one-time device selection). Defined in
// ncnn_gpu_init.cpp; the engine guards try_init_vulkan() with this mutex so
// parallel engine instances don't race the ncnn global Vulkan state.
extern std::mutex g_vulkan_init_mutex;

} // namespace detail
#endif

class NCNNEngineImpl : public Engine {
public:
    NCNNEngineImpl();
    ~NCNNEngineImpl() override;

    auto initialize(DeviceType preferred) -> expected<void, EngineError> override;
    auto list_devices() -> std::vector<DeviceCapability> override;
    auto load_model(const std::string& param_path, const std::string& bin_path)
        -> expected<void, EngineError> override;
    auto process(const float* input, float* output, int width, int height, int channels,
                 const std::atomic<bool>& cancel_flag) -> expected<void, EngineError> override;
    auto process_u8(const uint8_t* input, uint8_t* output, int width, int height, int channels,
                    const std::atomic<bool>& cancel_flag) -> expected<void, EngineError> override;
    auto process_batch(const std::vector<const float*>& inputs, const std::vector<float*>& outputs,
                       int width, int height, int channels,
                       const std::atomic<bool>& cancel_flag) -> expected<void, EngineError> override;
    auto benchmark(int test_tile_size) -> float override;
    void unload_model() override;

    DeviceType current_device() const override;
    bool is_initialized() const override;
    std::string model_name() const override;
    int get_scale_factor() const override;

    std::string debug_blob_scan(const uint8_t* input, int width, int height, int channels) override;

    void set_scale_factor(int scale) override;
    void set_inference_threads(int threads) override;
    auto get_memory_info() const -> std::pair<size_t, size_t> override;
    auto hardware_id() const -> std::string override;
    int get_recommended_tile_size() const override;
    size_t get_device_memory_budget_mb() const override;

    bool supports_gpu_tile_pipeline() const override;
    bool supports_gpu_tile_pipeline_u8() const override;
    bool supports_gpu_tile_pipeline_u8_queue() const override;

    auto process_tiles_gpu_u8_queue(const std::vector<GPUTileTaskU8>& tasks,
                                    const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> override;
    auto process_tiles_gpu(const float* input_data, float* output_data, int input_width,
                           int input_height, int channels, const std::vector<TileRegion>& regions,
                           const TileConfig& config,
                           std::function<void(const ProcessingProgress&)> progress_callback,
                           const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> override;
    auto process_tiles_gpu_u8(const uint8_t* input_data, uint8_t* output_data, int input_width,
                              int input_height, int channels, const std::vector<TileRegion>& regions,
                              const TileConfig& config,
                              std::function<void(const ProcessingProgress&)> progress_callback,
                              const std::atomic<bool>& cancel_flag)
        -> expected<void, EngineError> override;

private:
    auto format_vulkan_runtime_snapshot(std::string_view stage, int width = -1, int height = -1,
        int channels = -1, size_t submit_budget = 0, size_t submit_budget_cap = 0,
        size_t submit_memory_budget_mb = 0, double avg_tile_pixels = -1.0) const -> std::string;
    bool init_gpu_layout_pipelines();
    void kernel_selftest();
    bool init_gpu_crop_pipeline();
    bool init_gpu_tile_pipelines();
    auto process_vulkan_with_gpu_layout(const float* input, float* output, int width, int height,
        int channels, const std::atomic<bool>& cancel_flag) -> expected<void, EngineError>;
    auto process_vulkan_with_gpu_layout_run(const float* input, float* output, int width, int height,
        int channels, const std::atomic<bool>& cancel_flag) -> expected<void, EngineError>;
    bool try_init_vulkan();
    void detect_model_info(const std::string& param_path);

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
    // Fixed replicate-border prepadding required by the loaded model
    // (Real-CUGAN "se" family crops its receptive-field border internally;
    // callers must pad input so out == in*scale still holds). 0 = none.
    // The GPU tile pipelines honor it by growing each tile's net-input rect
    // by this margin (replicate at the image border).
    int model_prepadding_ = 0;
    // True when the param contains Crop layers, even at scales where no known
    // prepadding margin exists — only those margin-less models are barred
    // from the GPU tile pipelines (the internal crops would silently eat
    // tile borders with no prepadding to cancel them).
    bool model_has_border_crop_ = false;
    // Per-engine CPU inference thread cap; 0 = ncnn default (all logical CPUs).
    int inference_threads_ = 0;
};

} // namespace upix
