#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include "utils/expected.hpp"

namespace upix {

struct ModelInfo {
    std::string name;
    std::filesystem::path param_path;
    std::filesystem::path bin_path;
    int scale_factor = 2;
    int input_channels = 3;
    size_t total_size = 0;
    // Scene labels for the curated catalog (see core/model_metadata.hpp).
    // Empty scene = not a catalog model (user import) → no badge anywhere.
    // Never serialized: the phone wire format (ModelLibraryEntry) copies
    // fields explicitly.
    std::string scene;
    bool recommended = false;
};

struct ModelManagerError {
    int code;
    std::string message;
};

class ModelManager {
public:
    ModelManager() = default;
    explicit ModelManager(const std::filesystem::path& models_dir);
    
    auto set_models_directory(const std::filesystem::path& dir) -> void;
    auto scan_models() -> expected<std::vector<ModelInfo>, ModelManagerError>;
    
    auto get_model(const std::string& name) const -> std::optional<ModelInfo>;

    // Tolerant lookup shared by get_model and engine_setup: case- and
    // separator-insensitive, and a bare base name ("realesrgan-x4plus")
    // resolves to its single scale variant ("...-x4") when unambiguous.
    // Returns nullptr when no entry matches or the base name is ambiguous.
    static auto find_model(const std::vector<ModelInfo>& models,
                           const std::string& name) -> const ModelInfo*;

    auto models_directory() const -> const std::filesystem::path& { return models_dir_; }
    auto available_models() const -> const std::vector<ModelInfo>& { return models_; }

private:
    auto parse_param_file(const std::filesystem::path& path) 
        -> expected<ModelInfo, ModelManagerError>;

    std::filesystem::path models_dir_;
    std::vector<ModelInfo> models_;
};

} // namespace upix
