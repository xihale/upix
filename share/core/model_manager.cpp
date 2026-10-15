#include "model_manager.hpp"
#include "core/model_graph.hpp"
#include "core/model_metadata.hpp"
#include "utils/logger.hpp"
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <algorithm>
#include <unordered_set>

namespace upix {

namespace {
// Model names arrive from two independent sources — the Android app's asset
// layout ("Real-ESRGANv3-anime_x2", underscore before the scale suffix) and
// the PC's models/ directory ("Real-ESRGANv3-anime-x2", dash, see
// parse_param_file below) — that disagree only on the word separator. Fold
// both into one canonical lowercase/dash-joined form (spaces count as
// separators too, so shell-friendly "real cugan conservative x2" resolves)
// so a submitted name resolves regardless of which separator each side
// happened to use.
std::string normalize_model_name(std::string s) {
    for (char& c : s) {
        c = (c == '_' || c == ' ') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Upper bound for a head Reorg (pixel_unshuffle) stride read from a param file.
// Real values are 2 or 4; the bound keeps the stride² divisor below anything
// that could overflow and rejects garbage tokens outright.
constexpr int kMaxReorgStride = 16;

// Upper bound for the blob counts a .param line declares. Real layers connect a
// handful of blobs; the bound stops a hostile/truncated line from sizing a
// vector (or a loop) from a count like 2000000000.
constexpr int kMaxBlobCount = 1024;
}  // namespace

ModelManager::ModelManager(const std::filesystem::path& models_dir)
    : models_dir_(models_dir) {}

void ModelManager::set_models_directory(const std::filesystem::path& dir) {
    models_dir_ = dir;
}

auto ModelManager::scan_models() -> expected<std::vector<ModelInfo>, ModelManagerError> {
    models_.clear();
    
    std::error_code exists_ec;
    const bool dir_exists = std::filesystem::exists(models_dir_, exists_ec);
    if (exists_ec || !dir_exists) {
        LOG_WARNING("ModelManager", "Models directory does not exist: {}", models_dir_.string());
        return models_;
    }
    
    std::vector<std::filesystem::path> param_files;
    // Recursively search for .param files, following directory symlinks (models
    // are routinely symlinked in from another drive). The scan must never
    // throw: scan_models() reports failures through `expected`, but an
    // unreadable sub-directory or a dangling link would otherwise escape as
    // std::filesystem_error and terminate the CLI/daemon at startup. Errors
    // are reported through the error_code overloads instead — a partially
    // unreadable tree still yields every model that could be read. Depth is
    // capped because follow_directory_symlink turns a link cycle inside
    // models/ into unbounded traversal.
    constexpr int kMaxScanDepth = 8;
    auto iter_opts = std::filesystem::directory_options::follow_directory_symlink;
    try {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(models_dir_, iter_opts, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                LOG_WARNING("ModelManager", "Model scan aborted at {}: {}",
                            it->path().string(), ec.message());
                break;
            }
            if (it.depth() >= kMaxScanDepth) {
                // Deep enough that only a symlink cycle reaches here; keep the
                // entries at this level and stop descending.
                LOG_DEBUG("ModelManager", "Model scan depth cap reached at {}",
                          it->path().string());
                it.disable_recursion_pending();
                continue;
            }
            // Case-insensitive: model directories copied from a
            // case-insensitive filesystem ("X2.PARAM") must still be
            // catalogued, matching how the batch input scan folds extensions.
            std::string ext = it->path().extension().string();
            for (char& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            std::error_code type_ec;
            if (ext == ".param" && it->is_regular_file(type_ec) && !type_ec) {
                param_files.push_back(it->path());
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LOG_WARNING("ModelManager", "Model scan failed on {}: {}", models_dir_.string(),
                    e.what());
    }
    
    // Directory order is filesystem-dependent; sort so the catalog — and in
    // particular the "first available" default model an empty -m selects —
    // is reproducible across machines and filesystems.
    std::sort(param_files.begin(), param_files.end());

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
    // weakly_canonical's throwing overload would escape parse_param_file's
    // contract on a denied/IO-error path component; the error_code form plus a
    // lexical fallback keeps the comparison total.
    const auto same_directory = [](const std::filesystem::path& a,
                                   const std::filesystem::path& b) {
        std::error_code ec_a;
        std::error_code ec_b;
        const auto canon_a = std::filesystem::weakly_canonical(a, ec_a);
        const auto canon_b = std::filesystem::weakly_canonical(b, ec_b);
        if (ec_a || ec_b) {
            return a.lexically_normal() == b.lexically_normal();
        }
        return canon_a == canon_b;
    };
    const bool in_models_root = same_directory(path.parent_path(), models_dir_);
    info.name = in_models_root ? param_stem : (parent_name + "-" + param_stem);
    info.param_path = path;
    
    // Infer scale factor from the param stem. Nested layouts name the file
    // "x4.param"; flat layouts carry the full model name where the trailing
    // "x4" may be followed by more characters ("Real-ESRGAN-x4plus"). The
    // shared convention implementation lives in core/model_graph.cpp — the
    // same one the engine's no-upsampler fallback uses, and the one
    // test_model_catalog pins against scripts/publish_models.py. No match →
    // keep the 2 default.
    const int from_name = scale_from_filename(param_stem);
    info.scale_factor = from_name >= 1 ? from_name : 2;

    // Scene badge data (动漫/照片/…+ ★推荐) from the catalog table; empty
    // for imported non-catalog models.
    const auto meta = classify_model_scene(info.name);
    info.scene = std::string{meta.scene};
    info.recommended = meta.recommended;

    info.input_channels = 3;

    std::string line;
    std::getline(file, line);

    int layer_count = 0;
    int blob_count = 0;
    file >> layer_count >> blob_count;
    (void)layer_count;
    (void)blob_count;

    bool inferred_input_channels = false;
    bool seen_net_conv = false;
    int head_reorg_stride = 0;
    std::unordered_set<std::string> input_dependent_blobs;

    auto read_param_int = [](const std::vector<std::string>& param_tokens, int key) -> std::optional<int> {
        const std::string prefix = std::format("{}=", key);
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

            // Float form ("0=0.5"): require full consumption and an in-range
            // value — stod alone accepts a leading number and ignores the tail
            // ("1=2abc" -> 2) and an out-of-int-range cast is UB.
            try {
                size_t parsed = 0;
                const double value = std::stod(value_str, &parsed);
                if (parsed != value_str.size() || !std::isfinite(value) ||
                    value < static_cast<double>(std::numeric_limits<int>::min()) ||
                    value > static_cast<double>(std::numeric_limits<int>::max())) {
                    return std::nullopt;
                }
                return static_cast<int>(value);
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

        if (bottom_count < 0 || top_count < 0 ||
            bottom_count > kMaxBlobCount || top_count > kMaxBlobCount) {
            // The counts are attacker-controlled text: sizing a vector from a
            // claimed 2e9 blobs would throw out of this expected-returning
            // parser. No real layer has more than a handful of connections.
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
        if (type == "Reorg") {
            // pnnx lowers torch.pixel_unshuffle to Reorg mode 0 (space-to-depth).
            // As a head op it widens the first conv's channels relative to the
            // image: conv_C = image_C * stride^2 (Real-ESRGAN x2plus feeds
            // 12 = 3 * 2^2), so the image-facing count divides the conv count.
            // Only track it before the first conv; later Reorgs are internal.
            const auto stride = read_param_int(param_tokens, 0);
            const auto mode = read_param_int(param_tokens, 1);
            if (depends_on_input && !seen_net_conv && stride.has_value() &&
                stride.value() > 1 && stride.value() <= kMaxReorgStride &&
                (!mode.has_value() || mode.value() == 0)) {
                head_reorg_stride = stride.value();
            }
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

        seen_net_conv = true;
        // 64-bit: the stride comes from the param file, so stride*stride in
        // `int` is an overflow waiting for a hostile/corrupt token.
        const long long reorg_sq = static_cast<long long>(head_reorg_stride) * head_reorg_stride;
        if (head_reorg_stride > 1 && static_cast<long long>(inferred_channels) % reorg_sq == 0) {
            info.input_channels = static_cast<int>(inferred_channels / reorg_sq);
        } else {
            info.input_channels = inferred_channels;
        }
        inferred_input_channels = true;
    }
    
    // Derive the weights path from the path itself. Replacing the first
    // ".param" substring (the previous implementation) threw out_of_range when
    // the param path did not end in ".param", and rewrote a *parent* component
    // whenever one contained that substring ("models/my.param.dir/x2.param").
    std::filesystem::path bin_path = path;
    bin_path.replace_extension(".bin");
    {
        std::error_code ec;
        // Model directories copied from a case-insensitive filesystem pair
        // "x2.PARAM" with "x2.BIN"; fall back to that spelling only when the
        // lowercase one is absent.
        if (path.extension() == ".PARAM" && !std::filesystem::exists(bin_path, ec)) {
            std::filesystem::path alt = path;
            alt.replace_extension(".BIN");
            if (std::filesystem::exists(alt, ec)) {
                bin_path = alt;
            }
        }
    }
    info.bin_path = bin_path;

    // error_code overloads: the throwing file_size would escape declare-time
    // contracts when an import replaces the file between the two probes.
    std::error_code size_ec;
    const auto param_size = std::filesystem::file_size(path, size_ec);
    if (!size_ec) {
        info.total_size = param_size;
        const auto bin_size = std::filesystem::file_size(info.bin_path, size_ec);
        if (!size_ec) {
            info.total_size += bin_size;
        }
    }
    
    return info;
}

} // namespace upix
