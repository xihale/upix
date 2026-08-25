#include "engine_setup.hpp"

#include "utils/logger.hpp"

#include <string>

namespace upix {

auto initialize_engine(SetupDevice device)
    -> expected<std::unique_ptr<Engine>, std::string> {
    auto engine = create_ncnn_engine();

    // ── initialize ───────────────────────────────────────────────────────────
    switch (device) {
        case SetupDevice::GPU:
            if (auto r = engine->initialize(DeviceType::GPU_VULKAN); !r) {
                return std::unexpected("engine initialize (GPU) failed: " + r.error().message);
            }
            break;
        case SetupDevice::CPU:
            if (auto r = engine->initialize(DeviceType::CPU); !r) {
                return std::unexpected("engine initialize (CPU) failed: " + r.error().message);
            }
            break;
        case SetupDevice::GPU_THEN_CPU: {
            if (auto gpu = engine->initialize(DeviceType::GPU_VULKAN); !gpu) {
                LOG_WARNING("Engine", "GPU not available, falling back to CPU: {}", gpu.error().message);
                if (auto cpu = engine->initialize(DeviceType::CPU); !cpu) {
                    return std::unexpected("engine initialize failed on both GPU and CPU: " + cpu.error().message);
                }
            }
            break;
        }
        case SetupDevice::ENGINE_DEFAULT:
            // initialize() with no arg lets the engine pick (NCNNEngine tries
            // GPU→CPU internally).
            if (auto r = engine->initialize(); !r) {
                return std::unexpected("engine initialize failed: " + r.error().message);
            }
            break;
    }

    return engine;
}

auto switch_model(
    Engine& engine,
    ModelManager& models,
    std::string& loaded_model_name,
    const std::string& model_name,
    std::string_view log_tag
) -> bool {
    // Empty model name = "use whatever is loaded" (caller didn't specify).
    if (model_name.empty() || model_name == loaded_model_name) {
        return true;
    }
    auto info = models.get_model(model_name);
    if (!info) {
        LOG_ERROR(log_tag, "unknown model requested: {}", model_name);
        return false;
    }
    auto load_res = engine.load_model(info->param_path.string(), info->bin_path.string());
    if (!load_res) {
        LOG_ERROR(log_tag, "model load failed: {}", load_res.error().message);
        return false;
    }
    loaded_model_name = model_name;
    LOG_INFO(log_tag, "switched model to {}", model_name);
    return true;
}

auto setup_engine(
    SetupDevice device,
    std::string_view models_dir,
    std::string_view model_name
) -> expected<EngineSetupResult, std::string> {
    EngineSetupResult result;

    auto init = initialize_engine(device);
    if (!init) {
        return std::unexpected(init.error());
    }
    result.engine = std::move(*init);

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
        result.selected = ModelManager::find_model(scanned, std::string(model_name));
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

} // namespace upix
