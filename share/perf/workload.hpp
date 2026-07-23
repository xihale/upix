// Synthetic tensor workload generator. Produces deterministic in-memory input
// tensors to feed Engine::process, bypassing image load/save I/O so the test
// isolates inference cost (the thing GPU efficiency measures).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace crossscale::perf {

// A pair of input/output buffers for one tile. Output is pre-allocated by the
// engine contract (caller provides the output buffer).
struct TensorPair {
    int width = 0;
    int height = 0;
    int channels = 0;          // interleaved HWC, matching Engine::process
    std::unique_ptr<float[]> input;
    std::unique_ptr<float[]> output;
};

// Generates a deterministic gradient tensor of the given geometry. The pattern
// is reproducible across runs so runs are comparable.
class Workload {
public:
    // Generates one tensor pair. Output buffer is sized for the engine's output
    // (scale² × input), caller passes scale.
    auto make_tile(int width, int height, int channels, int scale) -> TensorPair;
};

} // namespace crossscale::perf
