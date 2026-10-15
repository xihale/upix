#include "core/model_graph.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/logger.hpp"

namespace upix {

namespace {

// Upper bound for a head Reorg (space-to-depth) stride read out of the param
// file: real values are 2/4, and the bound keeps stride² free of overflow.
constexpr int kMaxReorgStride = 16;

// Upper bound for the blob counts a param line declares. The counts are text
// from the file; sizing a vector from a claimed 2000000000 blobs would throw
// out of this optional-returning parser.
constexpr int kMaxBlobCount = 1024;

} // namespace

auto scale_from_filename(std::string_view name) -> int {
    int from_name = 0;
    for (size_t i = 0; i + 1 < name.size(); ++i) {
        if (name[i] != 'x' && name[i] != 'X') continue;
        if (!std::isdigit(static_cast<unsigned char>(name[i + 1]))) continue;
        int n = 0;
        size_t j = i + 1;
        // Stop accumulating past the accepted range: an unbounded digit run
        // ("x99999999999") would overflow `n` and could wrap into a value the
        // 1..8 filter accepts.
        while (j < name.size() && n <= 8 &&
               std::isdigit(static_cast<unsigned char>(name[j]))) {
            n = n * 10 + (name[j] - '0');
            ++j;
        }
        if (n >= 1 && n <= 8) from_name = n;
    }
    return from_name;
}

auto detect_model_graph(const std::string& param_path)
    -> std::optional<ModelGraphInfo> {
    std::ifstream file(param_path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    ModelGraphInfo info;

    // Input layer dims: "Input  <name>  <in> <out>  <blob>  0=W 1=H 2=C".
    // Prefix-matched and every parse guarded: a truncated or hand-edited param
    // must leave the dims at their defaults, not throw std::invalid_argument
    // out of this optional-returning API. The leading space in " 0=" keeps a
    // blob name that ends in a digit from being read as a param.
    auto param_int = [](const std::string& text, std::string_view key) -> std::optional<int> {
        const size_t pos = text.find(key);
        if (pos == std::string::npos) return std::nullopt;
        try {
            return std::stoi(text.substr(pos + key.size()));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("Input", 0) == 0) {
            if (const auto w = param_int(line, " 0=")) info.input_width = *w;
            if (const auto h = param_int(line, " 1=")) info.input_height = *h;
            if (const auto c = param_int(line, " 2=")) info.input_channels = *c;
        }
    }

    // Infer the net scale factor from Interp layers. SR nets compose
    // upsampling, so the net scale is the product of every Interp layer's
    // per-axis scale (param 1 = height scale, 2 = width scale). Only
    // scale-based resize types (param 0 in {1,3,4,5}) carry a scale;
    // type 2 resizes to fixed output dims instead and is skipped.
    // e.g. realesrgan-x4plus: two 2x Interps -> 4;
    //      Real-ESRGANv3-anime x2: 4x Interp then 0.5x downsample -> 2.
    double interp_product = 1.0;
    bool found_interp = false;
    // A space-to-depth Reorg head (pnnx's lowering of torch.pixel_unshuffle,
    // e.g. Real-ESRGAN x2plus) shrinks the net's working resolution by its
    // stride before the upsamplers run; the image-facing scale divides the
    // upsampler product by that stride. Only a Reorg before any upsampler
    // op counts — later Reorgs are internal reshapes.
    int head_reorg_stride = 0;
    bool upsampler_seen = false;
    // Deconvolution-upsampler nets (Real-CUGAN): the final deconv that
    // restores image channels (0=3) carries the net scale in its stride.
    int deconv_scale = 0;
    // Real-CUGAN "se" nets carry internal Crop layers trimming the
    // receptive-field border; they need a fixed replicate prepadding so
    // the out == in*scale contract holds (margins from upstream
    // realcugan-ncnn-vulkan: x2=18, x3=14, x4=19).
    // PixelShuffle-trunk bookkeeping (Real-CUGAN x4): trunk deconvs feed a
    // Split; auxiliary deconvs feed residual adds. The net output is the
    // last layer's top blob.
    struct UpsampleOp { std::string top; int ratio = 1; };
    std::vector<UpsampleOp> deconvs, pixelshuffles;
    bool ps_trunk_detected = false;
    std::unordered_map<std::string, char> blob_consumers;  // bit 1: consumed, bit 2: by Split
    std::string net_output_blob;
    file.clear();
    file.seekg(0);
    while (std::getline(file, line)) {
        {
            // Tokenized pass for graph bookkeeping: type name n_b n_t [bottoms] [tops] ...
            std::istringstream iss(line);
            std::string type, name;
            int bottom_count = -1, top_count = -1;
            if (iss >> type >> name >> bottom_count >> top_count
                && bottom_count >= 0 && top_count >= 0
                && bottom_count <= kMaxBlobCount && top_count <= kMaxBlobCount) {
                std::vector<std::string> bottoms(static_cast<size_t>(bottom_count));
                std::vector<std::string> tops(static_cast<size_t>(top_count));
                bool tokens_ok = true;
                for (auto& b : bottoms) { if (!(iss >> b)) { tokens_ok = false; break; } }
                for (auto& t : tops) { if (!(iss >> t)) { tokens_ok = false; break; } }
                if (tokens_ok) {
                    net_output_blob = tops.empty() ? net_output_blob : tops.back();
                    for (const auto& b : bottoms) blob_consumers[b] |= 1;
                    if (type == "Split") {
                        for (const auto& b : bottoms) blob_consumers[b] |= 2;
                    }
                    auto param_i = [&line](const std::string& key) -> int {
                        try {
                            size_t pos = line.find(key);
                            if (pos == std::string::npos) return -1;
                            return std::stoi(line.substr(pos + key.size()));
                        } catch (const std::exception&) {
                            return -1;
                        }
                    };
                    if (type == "Reorg") {
                        const int stride = param_i(" 0=");
                        const int mode = param_i(" 1=");
                        if (!upsampler_seen && stride > 1 && stride <= kMaxReorgStride &&
                            mode <= 0) {
                            head_reorg_stride = stride;
                        }
                    } else if (type == "Deconvolution") {
                        const int stride = param_i(" 3=");
                        if (stride >= 1 && stride <= kMaxGraphScaleFactor) {
                            deconvs.push_back({tops.empty() ? "" : tops.back(), stride});
                            upsampler_seen = upsampler_seen || stride > 1;
                        }
                    } else if (type == "PixelShuffle") {
                        const int ratio = param_i(" 0=");
                        if (ratio >= 1 && ratio <= kMaxGraphScaleFactor) {
                            pixelshuffles.push_back({tops.empty() ? "" : tops.back(), ratio});
                            upsampler_seen = upsampler_seen || ratio > 1;
                        }
                    }
                }
            }
        }
        if (line.rfind("Interp", 0) == 0) {
            auto param = [&line](const std::string& key) -> double {
                try {
                    size_t pos = line.find(key);
                    if (pos == std::string::npos) return 1.0;
                    return std::stod(line.substr(pos + key.size()));
                } catch (const std::exception&) {
                    return 1.0;
                }
            };
            if (param(" 0=") == 2.0) continue;  // fixed output size, not a scale
            // A scale-based Interp always carries 1=<height scale>. Without it
            // the layer is not a scale witness at all: treating it as one (the
            // default 1.0) pinned the net at x1 and suppressed the filename
            // fallback the engine relies on.
            if (line.find(" 1=") == std::string::npos) continue;
            const double h_scale = param(" 1=");
            if (!(h_scale > 0.0)) continue;
            found_interp = true;
            interp_product *= h_scale;
            if (h_scale > 1.0) upsampler_seen = true;
        }
        if (line.rfind("Crop", 0) == 0) {
            info.has_border_crop = true;
        }
        if (deconv_scale == 0 && line.rfind("Deconvolution", 0) == 0) {
            auto param_i = [&line](const std::string& key) -> int {
                try {
                    size_t pos = line.find(key);
                    if (pos == std::string::npos) return -1;
                    return std::stoi(line.substr(pos + key.size()));
                } catch (const std::exception&) {
                    return -1;
                }
            };
            if (param_i(" 0=") == 3) {
                const int stride = param_i(" 3=");
                // Bounded: an unclamped stride token propagates into every
                // width*scale multiply downstream (int overflow).
                if (stride >= 1 && stride <= kMaxGraphScaleFactor) {
                    deconv_scale = stride;
                }
            }
        }
    }
    // A graph-derived scale is accepted only inside the supported range: every
    // caller multiplies image dimensions by it in `int`, so a param file whose
    // upsampler arithmetic lands outside 1..kMaxGraphScaleFactor must be
    // reported as "not detected" (callers then trust the filename convention)
    // instead of exported as a bogus scale.
    int interp_scale = 0;
    if (found_interp) {
        const double detected = interp_product + 0.5;
        if (detected >= 1.0 && detected <= static_cast<double>(kMaxGraphScaleFactor)) {
            interp_scale = static_cast<int>(detected);
        }
    }
    if (interp_scale >= 1) {
        info.scale_factor = interp_scale;
    } else if (deconv_scale >= 1) {
        info.scale_factor = deconv_scale;
    } else if (!pixelshuffles.empty()) {
        // PixelShuffle-trunk nets (Real-CUGAN x4): the trunk upsampler is
        // the Deconvolution feeding a Split (auxiliary deconvs feed
        // residual adds), and the RGB merge happens in a trailing
        // PixelShuffle. Net scale = trunk deconv strides x PS ratio.
        long long trunk = 1;
        bool found_trunk = false;
        for (const auto& op : deconvs) {
            const char consumers = blob_consumers[op.top];
            if ((consumers & 1) && (consumers & 2)) {
                trunk *= op.ratio;
                found_trunk = true;
            }
        }
        if (found_trunk) {
            for (const auto& ps : pixelshuffles) {
                if (ps.top == net_output_blob) {
                    trunk *= ps.ratio;
                    if (trunk < 1 || trunk > kMaxGraphScaleFactor) {
                        // Implausible product: report no graph scale rather than
                        // an out-of-range one that overflows callers' int math.
                        break;
                    }
                    info.scale_factor = static_cast<int>(trunk);
                    ps_trunk_detected = true;
                    // Real-CUGAN se/pro x4 nets are known to execute to
                    // garbage on the pinned ncnn (both CPU and Vulkan;
                    // upstream ncnn 2022 runs them fine) — x2/x3 from the
                    // same families are unaffected. Warn so a garbage
                    // result from a custom-imported model is explainable.
                    LOG_WARNING("Engine",
                        "PixelShuffle-trunk model (Real-CUGAN x4 family): "
                        "known broken on this ncnn build; output may be garbage");
                    break;
                }
            }
        } else {
            // Pure PixelShuffle-trunk nets (SPAN family): no Interp and no
            // deconv trunk — the upsampling is a PixelShuffle chain feeding
            // the output, possibly through trailing pointwise ops. Net
            // scale = product of the ratios of the PixelShuffles reachable
            // backwards from the output blob.
            std::unordered_map<std::string, std::pair<std::string, int>> producer_type_ratio;
            std::unordered_map<std::string, std::vector<std::string>> producer_bottoms;
            file.clear();
            file.seekg(0);
            while (std::getline(file, line)) {
                std::istringstream iss(line);
                std::string type, name;
                int bottom_count = -1, top_count = -1;
                if (!(iss >> type >> name >> bottom_count >> top_count)
                    || bottom_count < 0 || top_count < 0
                    || bottom_count > kMaxBlobCount || top_count > kMaxBlobCount) {
                    continue;
                }
                std::vector<std::string> bottoms(static_cast<size_t>(bottom_count));
                std::vector<std::string> tops(static_cast<size_t>(top_count));
                bool tokens_ok = true;
                for (auto& b : bottoms) { if (!(iss >> b)) { tokens_ok = false; break; } }
                for (auto& t : tops) { if (!(iss >> t)) { tokens_ok = false; break; } }
                if (!tokens_ok) continue;
                int ratio = 0;
                if (type == "PixelShuffle") {
                    size_t pos = line.find(" 0=");
                    try {
                        if (pos != std::string::npos) {
                            const int parsed = std::stoi(line.substr(pos + 3));
                            // Bounded here as well as in the first pass: this
                            // map drives the BFS product below, where an
                            // unclamped token would overflow the accumulator.
                            ratio = (parsed >= 1 && parsed <= kMaxGraphScaleFactor) ? parsed : 0;
                        }
                    } catch (const std::exception&) {
                        ratio = 0;
                    }
                }
                for (const auto& t : tops) {
                    producer_type_ratio[t] = {type, ratio};
                    producer_bottoms[t] = bottoms;
                }
            }
            std::vector<std::string> frontier{net_output_blob};
            std::unordered_map<std::string, bool> seen_blob;
            long long ps_scale = 1;
            bool any_trunk_ps = false;
            while (!frontier.empty()) {
                const std::string blob = frontier.back();
                frontier.pop_back();
                if (seen_blob.count(blob)) continue;
                seen_blob[blob] = true;
                const auto type_it = producer_type_ratio.find(blob);
                if (type_it == producer_type_ratio.end()) continue;
                if (type_it->second.first == "PixelShuffle" && type_it->second.second >= 1) {
                    ps_scale *= type_it->second.second;
                    any_trunk_ps = true;
                    if (ps_scale > kMaxGraphScaleFactor) {
                        // Already outside the accepted range: the range check
                        // below rejects it either way, and stopping keeps a long
                        // chain from overflowing the accumulator first.
                        break;
                    }
                }
                const auto bottom_it = producer_bottoms.find(blob);
                if (bottom_it != producer_bottoms.end()) {
                    for (const auto& b : bottom_it->second) frontier.push_back(b);
                }
            }
            if (any_trunk_ps && ps_scale >= 1 && ps_scale <= 16) {
                info.scale_factor = static_cast<int>(ps_scale);
                ps_trunk_detected = true;
            }
        }
    }
    if (head_reorg_stride > 1 && info.scale_factor % head_reorg_stride == 0) {
        info.scale_factor /= head_reorg_stride;
    }
    info.scale_detected = interp_scale >= 1 || deconv_scale >= 1 || ps_trunk_detected;

    return info;
}

} // namespace upix
