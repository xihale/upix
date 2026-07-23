// PerfTest: one performance scenario. Composes probes + workload + engine to
// produce metrics. Stage 1 implements GpuEfficiencyTest; the interface is shared
// by future tests (tile-sweep, precision-sweep, distributed).
#pragma once

#include <string>

namespace crossscale::perf {

class Reporter;

// Test configuration parsed from CLI flags.
struct TestConfig {
    std::string model_param;     // resolved .param path
    std::string model_bin;       // resolved .bin path
    std::string model_name;      // display name
    int tile_width = 256;
    int tile_height = 256;
    int channels = 3;
    int scale = 2;
    int iterations = 0;          // 0 = derived from --duration
    double duration_s = 5.0;     // run for this long if iterations == 0
    double warmup_s = 1.0;       // warmup window before timing
    int sample_ms = 100;         // probe sample interval
    bool cpu_baseline = true;    // also run CPU backend for comparison
};

class PerfTest {
public:
    virtual ~PerfTest() = default;
    virtual auto name() const -> std::string = 0;
    // Returns 0 on success, non-zero error code.
    virtual auto run(const TestConfig& cfg, Reporter& rep) -> int = 0;
};

// Stage 1 test. Measures GPU utilization/memory/power/clock under sustained
// synthetic-tensor inference load, identifies CPU-vs-GPU bottleneck, optionally
// compares against the CPU backend.
class GpuEfficiencyTest : public PerfTest {
public:
    auto name() const -> std::string override { return "gpu-efficiency"; }
    auto run(const TestConfig& cfg, Reporter& rep) -> int override;
};

} // namespace crossscale::perf
