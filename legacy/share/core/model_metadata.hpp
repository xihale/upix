// Scene metadata for known catalog models: what content a model is for
// (anime / photo / general / restoration…) and whether it is a recommended
// pick for that scene. Derived from the catalog write-ups in
// models/README.md ("Use it for" column); every shipped model must have a
// row here — test_model_scene pins the table against the full catalog list.
//
// This is presentation-side metadata, NOT wire data: the distributed
// protocol serializes ModelLibraryEntry field-by-field (name/scale/size),
// so new ModelInfo fields never reach the phone. Surfaces that want the
// labels port the same table (apps/app ModelScenes.kt is the literal
// Kotlin counterpart — keep the two in sync when the catalog changes).
#pragma once

#include <string_view>

namespace upix {

struct ModelSceneInfo {
    // zh display label, e.g. "动漫" or "动漫·修复"; empty for models outside
    // the curated catalog (user imports), which simply get no badge.
    std::string_view scene;
    bool recommended = false;
};

// Pattern table lookup on the model name. Normalizes case and the
// underscore/space → dash separators exactly like
// ModelManager::find_model, so every spelling of a catalog name resolves.
auto classify_model_scene(std::string_view name) -> ModelSceneInfo;

} // namespace upix
