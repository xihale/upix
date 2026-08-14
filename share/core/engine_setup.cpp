#include "engine_setup.hpp"

#include "utils/logger.hpp"

#include <string>

namespace crossscale {

auto setup_engine(
    SetupDevice device,
    std::string_view models_dir,
    std::string_view model_name
) -> expected<EngineSetupResult, std::string> {
    EngineSetupResult result;
    result.engine = create_ncnn_engine();

    // ── initialize ───────────────────────────────────────────────────────────
    switch (device) {
        case SetupDevice::GPU:
            if (auto r = result.engine->initialize(DeviceType::GPU_VULKAN); !r) {
                return std::unexpected("engine initialize (GPU) failed: " + r.error().message);
            }
            break;
        case SetupDevice::CPU:
            if (auto r = result.engine->initialize(DeviceType::CPU); !r) {
                return std::unexpected("engine initialize (CPU) failed: " + r.error().message);
            }
            break;
        case SetupDevice::GPU_THEN_CPU: {
            if (auto gpu = result.engine->initialize(DeviceType::GPU_VULKAN); !gpu) {
                LOG_WARNING("Engine", "GPU not available, falling back to CPU");
                if (auto cpu = result.engine->initialize(DeviceType::CPU); !cpu) {
                    return std::unexpected("engine initialize failed on both GPU and CPU");
                }
            }
            break;
        }
        case SetupDevice::ENGINE_DEFAULT:
            // initialize() with no arg lets the engine pick (NCNNEngine tries
            // GPU→CPU internally).
            if (auto r = result.engine->initialize(); !r) {
                return std::unexpected("engine initialize failed: " + r.error().message);
            }
            break;
    }

    // ── scan_models ──────────────────────────────────────────────────────────
    const std::string resolved_models_dir =
        models_dir.empty() ? std::string("./models") : std::string(models_dir);
    result.models.set_models_directory(resolved_models_dir);
    auto scan = result.models.scan_models();
    if (!scan) {
        return std::unexpected("model scan failed: " + scan.error().message);
    }
    if (scan->empty()) {
        return std::unexpected("no models found in: " + resolved_models_dir);
    }

    // ── select ───────────────────────────────────────────────────────────────
    // scan_models() returns a copy of its internal vector; point `selected`
    // into the manager-owned vector (available_models()) instead so the pointer
    // stays valid for the lifetime of EngineSetupResult.models.
    const auto& scanned = result.models.available_models();
    if (model_name.empty()) {
        result.selected = &scanned.front();
    } else {
        for (const auto& m : scanned) {
            if (m.name == model_name) {
                result.selected = &m;
                break;
            }
        }
        if (!result.selected) {
            return std::unexpected("model not found: " + std::string(model_name));
        }
    }

    // ── load_model ───────────────────────────────────────────────────────────
    auto load = result.engine->load_model(
        result.selected->param_path.string(),
        result.selected->bin_path.string()
    );
    if (!load) {
        return std::unexpected("model load failed: " + load.error().message);
    }

    return result;
}

} // namespace crossscale
