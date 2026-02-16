#include "model_manager.hpp"
#include "utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace crossscale {

ModelManager::ModelManager(const std::filesystem::path& models_dir)
    : models_dir_(models_dir) {}

void ModelManager::set_models_directory(const std::filesystem::path& dir) {
    models_dir_ = dir;
}

auto ModelManager::scan_models() -> expected<std::vector<ModelInfo>, ModelManagerError> {
    models_.clear();
    
    if (!std::filesystem::exists(models_dir_)) {
        LOG_WARNING("ModelManager", "Models directory does not exist: " + models_dir_.string());
        return models_;
    }
    
    std::vector<std::filesystem::path> param_files;
    // Recursively search for .param files, following directory symlinks
    auto iter_opts = std::filesystem::directory_options::follow_directory_symlink;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(models_dir_, iter_opts)) {
        if (entry.is_regular_file() && entry.path().extension() == ".param") {
            param_files.push_back(entry.path());
        }
    }
    
    for (const auto& param_path : param_files) {
        auto result = parse_param_file(param_path);
        if (result.has_value()) {
            models_.push_back(result.value());
        }
    }
    
    return models_;
}

auto ModelManager::get_model(const std::string& name) const -> std::optional<ModelInfo> {
    auto it = std::find_if(models_.begin(), models_.end(), 
        [&](const ModelInfo& m) { return m.name == name; });
    
    if (it != models_.end()) {
        return *it;
    }
    return std::nullopt;
}

bool ModelManager::has_model(const std::string& name) const {
    return get_model(name).has_value();
}

auto ModelManager::infer_metadata(const std::filesystem::path& param_path)
    -> expected<ModelInfo, ModelManagerError> {
    return parse_param_file(param_path);
}

auto ModelManager::compute_hash(const std::filesystem::path& path)
    -> expected<std::string, ModelManagerError> {
    
    if (!std::filesystem::exists(path)) {
        return ModelManagerError{1, "File not found: " + path.string()};
    }
    
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return ModelManagerError{2, "Cannot open file: " + path.string()};
    }
    
    std::vector<uint8_t> buffer(8192);
    std::vector<uint8_t> data;
    
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        data.insert(data.end(), buffer.begin(), buffer.begin() + file.gcount());
    }
    
    uint32_t hash = 0;
    for (uint8_t b : data) {
        hash = hash * 31 + b;
    }
    
    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

bool ModelManager::verify_model(const std::string& name) {
    auto model = get_model(name);
    if (!model) return false;
    
    return std::filesystem::exists(model->param_path) && 
           std::filesystem::exists(model->bin_path);
}

auto ModelManager::import_model(const std::filesystem::path& param_path,
                                 const std::filesystem::path& bin_path)
    -> expected<ModelInfo, ModelManagerError> {
    
    if (!std::filesystem::exists(param_path)) {
        return ModelManagerError{1, "Param file not found"};
    }
    if (!std::filesystem::exists(bin_path)) {
        return ModelManagerError{1, "Bin file not found"};
    }
    
    auto result = parse_param_file(param_path);
    if (!result.has_value()) {
        return result.error();
    }
    
    auto info = result.value();
    info.bin_path = bin_path;
    info.total_size = std::filesystem::file_size(param_path) + std::filesystem::file_size(bin_path);
    
    if (!models_dir_.empty()) {
        auto dest_param = models_dir_ / param_path.filename();
        auto dest_bin = models_dir_ / bin_path.filename();
        
        std::filesystem::copy_file(param_path, dest_param, 
                                    std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(bin_path, dest_bin,
                                    std::filesystem::copy_options::overwrite_existing);
        
        info.param_path = dest_param;
        info.bin_path = dest_bin;
    }
    
    models_.push_back(info);
    return info;
}

bool ModelManager::delete_model(const std::string& name) {
    auto it = std::find_if(models_.begin(), models_.end(),
        [&](const ModelInfo& m) { return m.name == name; });
    
    if (it == models_.end()) return false;
    
    std::filesystem::remove(it->param_path);
    std::filesystem::remove(it->bin_path);
    models_.erase(it);
    
    return true;
}

auto ModelManager::parse_param_file(const std::filesystem::path& path)
    -> expected<ModelInfo, ModelManagerError> {
    
    std::ifstream file(path);
    if (!file) {
        return ModelManagerError{2, "Cannot open param file"};
    }
    
    ModelInfo info;
    // Construct model name as "parent_dir-param_stem", e.g., "Real-CUGAN-conservative-x2"
    std::string parent_name = path.parent_path().stem().string();
    std::string param_stem = path.stem().string();  // e.g., "x2", "x3"
    info.name = parent_name + "-" + param_stem;
    info.param_path = path;
    
    // Infer scale factor from param filename (x2 -> 2, x3 -> 3, etc.)
    info.scale_factor = 2;  // default
    if (param_stem.size() > 1 && param_stem[0] == 'x') {
        try {
            info.scale_factor = std::stoi(param_stem.substr(1));
        } catch (...) {
            // keep default
        }
    }
    
    info.input_channels = 3;
    info.tile_size = 256;
    
    std::string line;
    std::getline(file, line);
    
    int layer_count = 0, blob_count = 0;
    file >> layer_count >> blob_count;
    
    std::string last_layer_type;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        std::istringstream iss(line);
        std::string type, name;
        iss >> type >> name;
        
        if (!type.empty() && type != "Input") {
            last_layer_type = type;
        }
    }
    
    std::string bin_path_str = path.string();
    bin_path_str.replace(bin_path_str.find(".param"), 6, ".bin");
    info.bin_path = bin_path_str;
    
    if (std::filesystem::exists(path)) {
        info.total_size = std::filesystem::file_size(path);
        if (std::filesystem::exists(info.bin_path)) {
            info.total_size += std::filesystem::file_size(info.bin_path);
        }
    }
    
    return info;
}

} // namespace crossscale
