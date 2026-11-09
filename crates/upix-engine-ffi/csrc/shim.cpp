// extern "C" shim implementation. Thin adapters over upix::NCNNEngineImpl —
// no engine logic lives here; the engine sources in upix/core/ are verbatim
// ports-from-share (CONTRACT §7). Every entry point converts unexpected C++
// exceptions into UpixEngineStatus{199, message} so nothing escapes the FFI.

#include "shim.h"

#include "ncnn_engine_impl.hpp"
#include "utils/logger.hpp" // upix_engine_set_log_stderr: the engine's own sink

#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

// 199: reserved at this boundary for "C++ exception crossed internal scope".
// Never produced by the engine's own expected<> paths.
constexpr int kErrFfiException = 199;

UpixEngineStatus ok_status() {
    UpixEngineStatus s{};
    s.code = 0;
    s.message[0] = '\0';
    return s;
}

UpixEngineStatus error_status(int code, const std::string& message) {
    UpixEngineStatus s{};
    s.code = code;
    if (!message.empty()) {
        std::strncpy(s.message, message.c_str(), sizeof(s.message) - 1);
        s.message[sizeof(s.message) - 1] = '\0';
    }
    return s;
}

// catch-all adapter: f() runs engine code; any escaping exception becomes
// status 199 instead of unwinding across the C ABI.
template <typename F>
auto guarded(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const std::exception& e) {
        return error_status(kErrFfiException, e.what());
    } catch (...) {
        return error_status(kErrFfiException, "unhandled C++ exception in engine FFI");
    }
}

void copy_string(const std::string& src, char* buf, size_t cap) {
    if (buf == nullptr || cap == 0) {
        return;
    }
    std::strncpy(buf, src.c_str(), cap - 1);
    buf[cap - 1] = '\0';
}

UpixProgress to_c_progress(const upix::ProcessingProgress& p) {
    UpixProgress out{};
    out.total_tiles = p.total_tiles;
    out.completed_tiles = p.completed_tiles;
    out.current_tile_x = p.current_tile_x;
    out.current_tile_y = p.current_tile_y;
    return out;
}

std::function<void(const upix::ProcessingProgress&)> trampoline(
    void (*cb)(const UpixProgress*, void*), void* ctx) {
    if (cb == nullptr) {
        return {};
    }
    return [cb, ctx](const upix::ProcessingProgress& p) {
        UpixProgress cp = to_c_progress(p);
        cb(&cp, ctx);
    };
}

upix::DeviceType to_cpp_device(int preferred) {
    return preferred == UPIX_DEVICE_GPU_VULKAN ? upix::DeviceType::GPU_VULKAN
                                               : upix::DeviceType::CPU;
}

int32_t to_c_device(upix::DeviceType t) {
    return t == upix::DeviceType::GPU_VULKAN ? UPIX_DEVICE_GPU_VULKAN : UPIX_DEVICE_CPU;
}

std::vector<upix::TileRegion> to_cpp_regions(const UpixTileRegion* regions, int32_t count) {
    std::vector<upix::TileRegion> out;
    if (regions != nullptr && count > 0) {
        out.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            out.push_back(upix::TileRegion{regions[i].x, regions[i].y, regions[i].width,
                                           regions[i].height, regions[i].overlap});
        }
    }
    return out;
}

} // namespace

extern "C" {

// NOTE: upix_engine_set_spirv_pack is defined in spirv_pack.cpp (the injection
// point); only declared in shim.h.

void upix_engine_set_log_stderr(int enable) {
    upix::Logger::instance().set_output_stderr(enable != 0);
}

UpixEngine* upix_engine_create(void) {
    try {
        return reinterpret_cast<UpixEngine*>(new upix::NCNNEngineImpl());
    } catch (...) {
        return nullptr;
    }
}

void upix_engine_destroy(UpixEngine* engine) {
    delete reinterpret_cast<upix::NCNNEngineImpl*>(engine); // destructors must not throw
}

UpixEngineStatus upix_engine_initialize(UpixEngine* engine, int preferred) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        auto result = e->initialize(to_cpp_device(preferred));
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_list_devices(UpixEngine* engine, UpixDevice* out, int32_t cap,
                                          int32_t* out_len) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (out_len != nullptr) {
        *out_len = 0;
    }
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        auto devices = e->list_devices();
        if (out_len != nullptr) {
            *out_len = static_cast<int32_t>(devices.size());
        }
        if (out != nullptr) {
            int32_t n = 0;
            for (const auto& d : devices) {
                if (n >= cap) {
                    break;
                }
                UpixDevice cd{};
                cd.dtype = to_c_device(d.type);
                copy_string(d.name, cd.name, sizeof(cd.name));
                cd.compute_units = d.compute_units;
                cd.memory_mb = static_cast<uint64_t>(d.memory_mb);
                cd.benchmark_score = d.benchmark_score;
                out[n] = cd;
                ++n;
            }
            if (out_len != nullptr) {
                *out_len = n;
            }
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_load_model(UpixEngine* engine, const char* param_path,
                                        const char* bin_path) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    if (param_path == nullptr || bin_path == nullptr) {
        return error_status(2, "null model path");
    }
    return guarded([&]() -> UpixEngineStatus {
        auto result = e->load_model(param_path, bin_path);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

void upix_engine_unload_model(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e != nullptr) {
        try {
            e->unload_model();
        } catch (...) {
        }
    }
}

UpixEngineStatus upix_engine_process(UpixEngine* engine, const float* input, float* output,
                                     int32_t width, int32_t height, int32_t channels,
                                     const std::atomic<bool>* cancel_flag) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        static std::atomic<bool> never_cancel{false};
        auto result = e->process(input, output, width, height, channels,
                                 cancel_flag != nullptr ? *cancel_flag : never_cancel);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_process_u8(UpixEngine* engine, const uint8_t* input, uint8_t* output,
                                        int32_t width, int32_t height, int32_t channels,
                                        const std::atomic<bool>* cancel_flag) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        static std::atomic<bool> never_cancel{false};
        auto result = e->process_u8(input, output, width, height, channels,
                                    cancel_flag != nullptr ? *cancel_flag : never_cancel);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_process_batch(UpixEngine* engine, const float* const* inputs,
                                           float* const* outputs, int32_t count, int32_t width,
                                           int32_t height, int32_t channels,
                                           const std::atomic<bool>* cancel_flag) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        std::vector<const float*> in(inputs, inputs + (count > 0 ? count : 0));
        std::vector<float*> out(outputs, outputs + (count > 0 ? count : 0));
        static std::atomic<bool> never_cancel{false};
        auto result = e->process_batch(in, out, width, height, channels,
                                       cancel_flag != nullptr ? *cancel_flag : never_cancel);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_process_tiles_gpu(UpixEngine* engine, const float* input_data,
                                               float* output_data, int32_t input_width,
                                               int32_t input_height, int32_t channels,
                                               const UpixTileRegion* regions, int32_t region_count,
                                               UpixTileConfig config,
                                               void (*progress_cb)(const UpixProgress*, void*),
                                               void* progress_ctx,
                                               const std::atomic<bool>* cancel_flag) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        upix::TileConfig cfg{};
        cfg.tile_size = config.tile_size;
        cfg.overlap = config.overlap;
        cfg.batch_size = config.batch_size;
        static std::atomic<bool> never_cancel{false};
        auto result = e->process_tiles_gpu(
            input_data, output_data, input_width, input_height, channels,
            to_cpp_regions(regions, region_count), cfg,
            trampoline(progress_cb, progress_ctx),
            cancel_flag != nullptr ? *cancel_flag : never_cancel);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_process_tiles_gpu_u8(UpixEngine* engine, const uint8_t* input_data,
                                                  uint8_t* output_data, int32_t input_width,
                                                  int32_t input_height, int32_t channels,
                                                  const UpixTileRegion* regions,
                                                  int32_t region_count, UpixTileConfig config,
                                                  void (*progress_cb)(const UpixProgress*, void*),
                                                  void* progress_ctx,
                                                  const std::atomic<bool>* cancel_flag) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        upix::TileConfig cfg{};
        cfg.tile_size = config.tile_size;
        cfg.overlap = config.overlap;
        cfg.batch_size = config.batch_size;
        static std::atomic<bool> never_cancel{false};
        auto result = e->process_tiles_gpu_u8(
            input_data, output_data, input_width, input_height, channels,
            to_cpp_regions(regions, region_count), cfg,
            trampoline(progress_cb, progress_ctx),
            cancel_flag != nullptr ? *cancel_flag : never_cancel);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_process_tiles_gpu_u8_queue(UpixEngine* engine,
                                                        const UpixGpuTileTask* tasks,
                                                        int32_t task_count,
                                                        const std::atomic<bool>* cancel_flag) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        if (tasks == nullptr || task_count <= 0) {
            return error_status(100, "task array empty");
        }
        // Stable storage for per-task region vectors: deque never moves
        // elements on growth, so the pointers captured below stay valid for
        // the duration of the call.
        std::deque<std::vector<upix::TileRegion>> regions_storage;
        std::vector<upix::GPUTileTaskU8> converted;
        regions_storage.resize(static_cast<size_t>(task_count));
        converted.reserve(static_cast<size_t>(task_count));
        for (int32_t i = 0; i < task_count; ++i) {
            const UpixGpuTileTask& t = tasks[i];
            regions_storage[static_cast<size_t>(i)] =
                to_cpp_regions(t.regions, t.region_count);
            upix::GPUTileTaskU8 g{};
            g.input_data = t.input_data;
            g.output_data = t.output_data;
            g.input_width = t.input_width;
            g.input_height = t.input_height;
            g.channels = t.channels;
            g.regions = &regions_storage[static_cast<size_t>(i)];
            upix::TileConfig cfg{};
            cfg.tile_size = t.config.tile_size;
            cfg.overlap = t.config.overlap;
            cfg.batch_size = t.config.batch_size;
            g.config = cfg;
            g.progress_callback = [cb = t.progress_callback, ctx = t.progress_ctx](
                                      const upix::ProcessingProgress& p) {
                if (cb != nullptr) {
                    UpixProgress cp = to_c_progress(p);
                    cb(&cp, ctx);
                }
            };
            converted.push_back(std::move(g));
        }
        static std::atomic<bool> never_cancel{false};
        auto result = e->process_tiles_gpu_u8_queue(
            converted, cancel_flag != nullptr ? *cancel_flag : never_cancel);
        if (!result) {
            return error_status(result.error().code, result.error().message);
        }
        return ok_status();
    });
}

UpixEngineStatus upix_engine_debug_blob_scan(UpixEngine* engine, const uint8_t* input,
                                             int32_t width, int32_t height, int32_t channels,
                                             char* buf, size_t cap) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (buf != nullptr && cap > 0) {
        buf[0] = '\0';
    }
    if (e == nullptr) {
        return error_status(1, "null engine");
    }
    return guarded([&]() -> UpixEngineStatus {
        copy_string(e->debug_blob_scan(input, width, height, channels), buf, cap);
        return ok_status();
    });
}

float upix_engine_benchmark(UpixEngine* engine, int32_t test_tile_size) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0.0f;
    }
    try {
        return e->benchmark(test_tile_size);
    } catch (...) {
        return 0.0f;
    }
}

int32_t upix_engine_current_device(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return UPIX_DEVICE_CPU;
    }
    try {
        return to_c_device(e->current_device());
    } catch (...) {
        return UPIX_DEVICE_CPU;
    }
}

int32_t upix_engine_is_initialized(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->is_initialized() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

void upix_engine_model_name(UpixEngine* engine, char* buf, size_t cap) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (buf != nullptr && cap > 0) {
        buf[0] = '\0';
    }
    if (e == nullptr) {
        return;
    }
    try {
        copy_string(e->model_name(), buf, cap);
    } catch (...) {
    }
}

int32_t upix_engine_get_scale_factor(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->get_scale_factor();
    } catch (...) {
        return 0;
    }
}

int32_t upix_engine_scale_factor_detected(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->scale_factor_detected() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int32_t upix_engine_model_input_channels(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->model_input_channels();
    } catch (...) {
        return 0;
    }
}

void upix_engine_set_scale_factor(UpixEngine* engine, int32_t scale) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return;
    }
    try {
        e->set_scale_factor(scale);
    } catch (...) {
    }
}

void upix_engine_set_inference_threads(UpixEngine* engine, int32_t threads) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return;
    }
    try {
        e->set_inference_threads(threads);
    } catch (...) {
    }
}

int32_t upix_engine_get_recommended_tile_size(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->get_recommended_tile_size();
    } catch (...) {
        return 0;
    }
}

uint64_t upix_engine_get_device_memory_budget_mb(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return static_cast<uint64_t>(e->get_device_memory_budget_mb());
    } catch (...) {
        return 0;
    }
}

void upix_engine_get_memory_info(UpixEngine* engine, uint64_t* total_mb, uint64_t* available_mb) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (total_mb != nullptr) {
        *total_mb = 0;
    }
    if (available_mb != nullptr) {
        *available_mb = 0;
    }
    if (e == nullptr) {
        return;
    }
    try {
        auto info = e->get_memory_info();
        if (total_mb != nullptr) {
            *total_mb = static_cast<uint64_t>(info.first);
        }
        if (available_mb != nullptr) {
            *available_mb = static_cast<uint64_t>(info.second);
        }
    } catch (...) {
    }
}

void upix_engine_hardware_id(UpixEngine* engine, char* buf, size_t cap) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (buf != nullptr && cap > 0) {
        buf[0] = '\0';
    }
    if (e == nullptr) {
        return;
    }
    try {
        copy_string(e->hardware_id(), buf, cap);
    } catch (...) {
    }
}

int32_t upix_engine_supports_gpu_tile_pipeline(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->supports_gpu_tile_pipeline() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int32_t upix_engine_supports_gpu_tile_pipeline_u8(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->supports_gpu_tile_pipeline_u8() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int32_t upix_engine_supports_gpu_tile_pipeline_u8_queue(UpixEngine* engine) {
    auto* e = reinterpret_cast<upix::NCNNEngineImpl*>(engine);
    if (e == nullptr) {
        return 0;
    }
    try {
        return e->supports_gpu_tile_pipeline_u8_queue() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

} // extern "C"
