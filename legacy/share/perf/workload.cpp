#include "perf/workload.hpp"

#include <cmath>

namespace upix::perf {

// Generate a deterministic gradient + sinusoid pattern. Reproducible across runs
// (no RNG) so different configs/tests are directly comparable. The pattern has
// enough spatial frequency content to exercise the conv stack realistically.
auto Workload::make_tile(int width, int height, int channels, int scale) -> TensorPair {
    TensorPair tp;
    tp.width = width;
    tp.height = height;
    tp.channels = channels;

    const size_t in_elems = static_cast<size_t>(width) * height * channels;
    tp.input = std::make_unique<float[]>(in_elems);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                const size_t idx = (static_cast<size_t>(y) * width + x) * channels + c;
                // Base gradient + per-channel phase. Range ~[0,1].
                float gx = static_cast<float>(x) / width;
                float gy = static_cast<float>(y) / height;
                float wave = std::sin((gx + gy) * 8.0f * 3.14159f + static_cast<float>(c));
                tp.input[idx] = 0.4f * gx + 0.3f * gy + 0.15f * wave + 0.1f;
            }
        }
    }

    // Output buffer sized for the upscaled result (scale² × input).
    const size_t out_w = static_cast<size_t>(width) * scale;
    const size_t out_h = static_cast<size_t>(height) * scale;
    tp.output = std::make_unique<float[]>(out_w * out_h * channels);

    return tp;
}

} // namespace upix::perf
