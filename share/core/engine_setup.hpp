#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "core/engine.hpp"
#include "core/model_manager.hpp"
#include "utils/expected.hpp"

namespace crossscale {

// Result of the create→initialize→scan_models→select→load_model recipe.
// `selected` points into `models`' scanned model vector — valid as long as
// `models` is alive (until the result is destroyed or scan_models() reruns).
struct EngineSetupResult {
    std::unique_ptr<Engine> engine;
    ModelManager models;              // scanned; ready for future reloads
    const ModelInfo* selected = nullptr;  // points into models.available_models()
};

enum class SetupDevice {
    GPU,                // try GPU only
    CPU,                // try CPU only
    GPU_THEN_CPU,       // try GPU, fall back to CPU on failure
    ENGINE_DEFAULT,     // call initialize() with no arg — engine picks (GPU→CPU)
};

// Create an engine and initialize it for the given device strategy (including
// the GPU→CPU fallback for GPU_THEN_CPU). Returns the ready engine or a
// human-readable error string. Used by setup_engine and by callers that keep
// the engine alive across requests (e.g. the coordinator's lazy init).
auto initialize_engine(SetupDevice device)
    -> expected<std::unique_ptr<Engine>, std::string>;

// Lazy model (re)load shared by the coordinator and the serve daemon: keep the
// currently loaded model when `model_name` is empty or already loaded;
// otherwise look it up in `models` and load it on `engine`. Updates
// `loaded_model_name` on success. The caller serializes engine access.
auto switch_model(
    Engine& engine,
    ModelManager& models,
    std::string& loaded_model_name,
    const std::string& model_name,
    std::string_view log_tag
) -> bool;

// Compose the create_ncnn_engine → initialize → scan_models → load_model recipe.
// Returns the ready engine + scanned models + selected model, or a human-readable
// error string describing which step failed. Caller decides how to surface the
// error (LOG + return code, throw, etc.).
auto setup_engine(
    SetupDevice device,
    std::string_view models_dir,    // empty → "./models"
    std::string_view model_name     // empty → first available
) -> expected<EngineSetupResult, std::string>;

} // namespace crossscale
