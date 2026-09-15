// Vulkan device initialization (enumeration / scoring / retry) and model
// shape detection. Split out of ncnn_engine.cpp for readability; the function
// bodies are byte-for-byte identical to the pre-split implementation.

#include "engine.hpp"
#include "model_graph.hpp"
#include "ncnn_engine_impl.hpp"
#include "tile_processor.hpp"
#include "utils/logger.hpp"
#include "utils/benchmark.hpp"
#include <net.h>
#include <cpu.h>
#if NCNN_VULKAN
#include <gpu.h>
#include <command.h>
#include <pipeline.h>
#include "spirv_pack.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace upix {

#if NCNN_VULKAN
namespace detail {

// Process-global mutex serializing ncnn Vulkan instance creation. Declared
// extern in ncnn_engine_impl.hpp; defined here (the GPU init TU).
std::mutex g_vulkan_init_mutex;

} // namespace detail
#endif

    bool NCNNEngineImpl::try_init_vulkan() {
#if NCNN_VULKAN
        int max_attempts = 6;
        if (const char* retries_env = std::getenv("UPIX_GPU_INIT_RETRIES")) {
            try {
                max_attempts = std::stoi(retries_env);
            } catch (...) {
                max_attempts = 6;
            }
        }
        max_attempts = std::clamp(max_attempts, 1, 12);

        int retry_delay_ms = 200;
        if (const char* delay_env = std::getenv("UPIX_GPU_INIT_DELAY_MS")) {
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
                std::lock_guard<std::mutex> lock(detail::g_vulkan_init_mutex);

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

                        LOG_INFO("Engine", "Vulkan candidate #{} name={} type={} compute_queues={} score={}",
                                 i, gpu_name, info.type(), info.compute_queue_count(), score);

                        if (score > best_score) {
                            best_score = score;
                            best_gpu_idx = i;
                        }
                    }

                    if (best_gpu_idx >= 0) {
                        vkdev_ = ncnn::get_gpu_device(best_gpu_idx);
                        if (vkdev_ != nullptr) {
                            spirv_pack::register_ncnn_lookup();
                            LOG_INFO("Engine", "Selected Vulkan GPU index {}", best_gpu_idx);
                            attempt_ok = true;
                        }
                    }
                }

            }

            if (attempt_ok) {
                return true;
            }

            if (attempt < max_attempts) {
                LOG_WARNING("Engine", "Vulkan init attempt {} failed, retrying in {}ms",
                            attempt, retry_delay_ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
                retry_delay_ms = std::min(retry_delay_ms * 2, 1500);
            }
        }
#endif
        return false;
    }

    void NCNNEngineImpl::detect_model_info(const std::string& param_path) {
        // Reset per-model state: the same engine can load a second model after
        // unload, and a stale prepadding/flag from the previous model would
        // misroute the next one's pipelines.
        model_prepadding_ = 0;
        model_has_border_crop_ = false;
        scale_factor_ = 2;
        scale_detected_ = false;

        // The graph walk itself lives in core/model_graph.cpp (pure param-file
        // analysis, shared with ModelManager and test_model_catalog); this
        // wrapper applies the engine-facing fallbacks on top of it.
        auto graph = detect_model_graph(param_path);
        if (!graph) {
            LOG_WARNING("Engine", "Could not parse param file for model info");
            return;
        }

        scale_factor_ = graph->scale_factor;
        scale_detected_ = graph->scale_detected;

        // A graph with no upsampler op at all is either a scale-1 restoration
        // net or a net whose upsampler we don't recognize. Read the filename
        // scale convention ("-x2", "-x1") as the tiebreaker — the same source
        // scripts/publish_models.py generates the manifest from. Android
        // loads models by bare path with no manifest at hand, so without this
        // fallback a 1x model would run under the 2x default and fail with a
        // shape mismatch.
        if (!scale_detected_) {
            const auto slash = param_path.find_last_of("/\\");
            const std::string base = slash == std::string::npos
                                         ? param_path
                                         : param_path.substr(slash + 1);
            const int from_name = scale_from_filename(base);
            if (from_name >= 1) {
                scale_factor_ = from_name;
                scale_detected_ = true;
                LOG_INFO("Engine",
                         "No upsampler op in graph; scale {}x from model filename",
                         from_name);
            }
        }

        if (graph->has_border_crop) {
            model_has_border_crop_ = true;
            switch (scale_factor_) {
                case 2: model_prepadding_ = 18; break;
                case 3: model_prepadding_ = 14; break;
                case 4: model_prepadding_ = 19; break;
                default: model_prepadding_ = 0; break;
            }
            LOG_WARNING("Engine",
                "Model uses internal border crops; applying prepadding={}",
                model_prepadding_,
                model_prepadding_ > 0
                     ? "; GPU tile pipelines tile with replicate-border context"
                     : " (unsupported scale; GPU tile pipelines disabled)");
        }

        // If we couldn't detect, try inferring from model metadata or use default
        if (graph->input_channels > 0) {
            input_channels_ = graph->input_channels;
        }

        LOG_INFO("Engine", "Detected input: {}x{}x{}, scale: {}",
                 graph->input_width, graph->input_height, input_channels_, scale_factor_);
    }

} // namespace upix
