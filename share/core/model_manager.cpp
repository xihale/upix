#include "model_manager.hpp"
#include "utils/logger.hpp"
#include <cctype>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>

namespace crossscale {

namespace {
// Model names arrive from two independent sources — the Android app's asset
// layout ("Real-ESRGANv3-anime_x2", underscore before the scale suffix) and
// the PC's models/ directory ("Real-ESRGANv3-anime-x2", dash, see
// parse_param_file below) — that disagree only on the word separator. Fold
// both into one canonical lowercase/dash-joined form so a phone-submitted
// name resolves regardless of which separator each side happened to use.
std::string normalize_model_name(std::string s) {
    for (char& c : s) {
        c = (c == '_') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
}  // namespace

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

auto ModelManager::find_model(const std::vector<ModelInfo>& models,
                              const std::string& name) -> const ModelInfo* {
    // Tolerant match: normalize both sides so "_x2" / "-x2" and case
    // differences resolve to the same entry.
    const auto key = normalize_model_name(name);
    for (const auto& m : models) {
        if (normalize_model_name(m.name) == key) {
            return &m;
        }
    }
    // Base-name fallback: accept "realesrgan-x4plus" for "realesrgan-x4plus-x4".
    // Only resolve when exactly one variant matches; with several scale
    // variants the request is ambiguous and we report not-found.
    const ModelInfo* unique = nullptr;
    for (const auto& m : models) {
        const auto norm = normalize_model_name(m.name);
        if (norm.size() > key.size() && norm.compare(0, key.size(), key) == 0
            && norm[key.size()] == '-') {
            if (unique != nullptr) {
                return nullptr;  // multiple scale variants share this base name
            }
            unique = &m;
        }
    }
    return unique;
}

auto ModelManager::get_model(const std::string& name) const -> std::optional<ModelInfo> {
    if (const auto* m = find_model(models_, name)) {
        return *m;
    }
    return std::nullopt;
}

auto ModelManager::parse_param_file(const std::filesystem::path& path)
    -> expected<ModelInfo, ModelManagerError> {
    
    std::ifstream file(path);
    if (!file) {
        return std::unexpected(ModelManagerError{2, "Cannot open param file"});
    }
    
    ModelInfo info;
    // Construct model name. If the param file sits directly in the models
    // directory (e.g. models/Real-ESRGANv3-anime-x2.param), the name is just
    // the file stem. If it lives in a model-specific subdirectory
    // (e.g. models/Real-ESRGANv3-anime/x2.param), the name is
    // "parent_dir-param_stem" to disambiguate.
    std::string param_stem = path.stem().string();  // e.g., "x2", "x3"
    std::string parent_name = path.parent_path().stem().string();
    const bool in_models_root =
        std::filesystem::weakly_canonical(path.parent_path()) ==
        std::filesystem::weakly_canonical(models_dir_);
    info.name = in_models_root ? param_stem : (parent_name + "-" + param_stem);
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

    std::string line;
    std::getline(file, line);

    int layer_count = 0;
    int blob_count = 0;
    file >> layer_count >> blob_count;
    (void)layer_count;
    (void)blob_count;

    bool inferred_input_channels = false;
    std::unordered_set<std::string> input_dependent_blobs;

    auto read_param_int = [](const std::vector<std::string>& param_tokens, int key) -> std::optional<int> {
        const std::string prefix = std::to_string(key) + "=";
        for (const auto& token : param_tokens) {
            if (token.rfind(prefix, 0) != 0) {
                continue;
            }

            const std::string value_str = token.substr(prefix.size());
            if (value_str.empty()) {
                return std::nullopt;
            }

            try {
                size_t parsed = 0;
                int value = std::stoi(value_str, &parsed);
                if (parsed == value_str.size()) {
                    return value;
                }
            } catch (...) {
            }

            try {
                return static_cast<int>(std::stod(value_str));
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);
        std::string type;
        std::string name;
        int bottom_count = 0;
        int top_count = 0;
        if (!(iss >> type >> name >> bottom_count >> top_count)) {
            continue;
        }

        if (bottom_count < 0 || top_count < 0) {
            continue;
        }

        std::vector<std::string> bottoms(static_cast<size_t>(bottom_count));
        std::vector<std::string> tops(static_cast<size_t>(top_count));

        bool parse_failed = false;
        for (int i = 0; i < bottom_count; ++i) {
            if (!(iss >> bottoms[static_cast<size_t>(i)])) {
                parse_failed = true;
                break;
            }
        }
        for (int i = 0; i < top_count && !parse_failed; ++i) {
            if (!(iss >> tops[static_cast<size_t>(i)])) {
                parse_failed = true;
                break;
            }
        }
        if (parse_failed) {
            continue;
        }

        std::vector<std::string> param_tokens;
        std::string token;
        while (iss >> token) {
            param_tokens.push_back(token);
        }

        bool depends_on_input = false;
        if (type == "Input") {
            depends_on_input = true;
            for (const auto& top : tops) {
                input_dependent_blobs.insert(top);
            }

            auto channels = read_param_int(param_tokens, 2);
            if (channels.has_value() && channels.value() > 0) {
                info.input_channels = channels.value();
                inferred_input_channels = true;
            }
            continue;
        }

        for (const auto& bottom : bottoms) {
            if (input_dependent_blobs.find(bottom) != input_dependent_blobs.end()) {
                depends_on_input = true;
                break;
            }
        }
        if (!depends_on_input) {
            continue;
        }

        for (const auto& top : tops) {
            input_dependent_blobs.insert(top);
        }

        if (inferred_input_channels) {
            continue;
        }
        if (type != "Convolution" && type != "ConvolutionDepthWise") {
            continue;
        }

        auto num_output = read_param_int(param_tokens, 0);
        auto kernel_w = read_param_int(param_tokens, 1);
        auto kernel_h = read_param_int(param_tokens, 11);
        auto weight_data_size = read_param_int(param_tokens, 6);
        auto group = read_param_int(param_tokens, 7);

        if (!num_output.has_value() || !kernel_w.has_value() || !weight_data_size.has_value()) {
            continue;
        }
        if (num_output.value() <= 0 || kernel_w.value() <= 0 || weight_data_size.value() <= 0) {
            continue;
        }

        const int kh = kernel_h.value_or(kernel_w.value());
        if (kh <= 0) {
            continue;
        }

        const int groups = std::max(1, group.value_or(1));
        const long long denominator =
            static_cast<long long>(num_output.value()) * kernel_w.value() * kh;
        const long long numerator =
            static_cast<long long>(weight_data_size.value()) * groups;
        if (denominator <= 0 || numerator <= 0 || (numerator % denominator) != 0) {
            continue;
        }

        const int inferred_channels = static_cast<int>(numerator / denominator);
        if (inferred_channels <= 0) {
            continue;
        }

        info.input_channels = inferred_channels;
        inferred_input_channels = true;
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
