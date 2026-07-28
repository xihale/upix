// Vulkan device initialization (enumeration / scoring / retry) and model
// shape detection. Split out of ncnn_engine.cpp for readability; the function
// bodies are byte-for-byte identical to the pre-split implementation.

#include "engine.hpp"
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
#endif
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace crossscale {

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

    void NCNNEngineImpl::detect_model_info(const std::string& param_path) {
        // Parse param file to detect input/output shapes
        std::ifstream file(param_path);
        if (!file.is_open()) {
            LOG_WARNING("Engine", "Could not parse param file for model info");
            return;
        }

        std::string line;
        int input_width = 0, input_height = 0, input_channels = 0;

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

} // namespace crossscale
