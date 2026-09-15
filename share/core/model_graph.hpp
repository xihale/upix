#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace upix {

// The xN scale convention shared by every surface that names a model
// ("Real-ESRGANv3-anime-x2", nested "x2.param" stems, "Real-ESRGAN-x4plus"):
// the LAST "x<digits>" run whose value lands in 1..8 wins; 0 = no scale is
// encoded. Single C++ implementation — ModelManager's scan and the engine's
// no-upsampler fallback both call it, and test_model_catalog pins it against
// scripts/publish_models.py's scale_from_name.
auto scale_from_filename(std::string_view name) -> int;

// What the param graph alone says about a model — derived from the layer
// walk, never from the filename. Pure file analysis: no net load, no GPU,
// usable from tests and tools.
struct ModelGraphInfo {
    int scale_factor = 2;          // graph-derived scale (default 2 when the
                                   // graph carries no recognized upsampler)
    bool scale_detected = false;   // true only when derived from graph ops
    int input_width = 0;
    int input_height = 0;
    int input_channels = 0;        // 0 = Input layer carries no dims
    bool has_border_crop = false;  // Crop layers (Real-CUGAN "se" family)
};

// Analyze an ncnn .param file: Input layer dims + net scale via the
// upsampler ladder (Interp product; image-facing deconv stride;
// PixelShuffle trunk — trunk-deconv or pure backward BFS; divided by a
// head-Reorg stride). nullopt when the file cannot be read.
auto detect_model_graph(const std::string& param_path)
    -> std::optional<ModelGraphInfo>;

} // namespace upix
