#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include "utils/expected.hpp"

namespace crossscale {

struct ModelInfo {
    std::string name;
    std::filesystem::path param_path;
    std::filesystem::path bin_path;
    int scale_factor = 2;
    int input_channels = 3;
    int tile_size = 256;
    size_t total_size = 0;
    std::string hash;
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
    auto has_model(const std::string& name) const -> bool;
    
    auto infer_metadata(const std::filesystem::path& param_path) 
        -> expected<ModelInfo, ModelManagerError>;
    
    auto compute_hash(const std::filesystem::path& path) 
        -> expected<std::string, ModelManagerError>;
    
    auto verify_model(const std::string& name) -> bool;
    
    auto import_model(const std::filesystem::path& param_path, 
                      const std::filesystem::path& bin_path)
        -> expected<ModelInfo, ModelManagerError>;
    
    auto delete_model(const std::string& name) -> bool;
    
    auto models_directory() const -> const std::filesystem::path& { return models_dir_; }
    auto available_models() const -> const std::vector<ModelInfo>& { return models_; }

private:
    auto parse_param_file(const std::filesystem::path& path) 
        -> expected<ModelInfo, ModelManagerError>;

    std::filesystem::path models_dir_;
    std::vector<ModelInfo> models_;
};

} // namespace crossscale
