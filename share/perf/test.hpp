// PerfTest: one performance scenario. Composes probes + workload + engine to
// produce metrics. GpuEfficiencyTest measures single-engine util;
// GpuConcurrencyTest measures N-engine concurrent util. The interface is shared
// by future tests (tile-sweep, precision-sweep, distributed).
#pragma once

#include <string>

namespace upix::perf {

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
    int concurrency = 1;         // independent engines/threads (gpu-concurrency test)
};

class PerfTest {
public:
    virtual ~PerfTest() = default;
    virtual auto name() const -> std::string = 0;
    // Returns 0 on success, non-zero error code.
    virtual auto run(const TestConfig& cfg, Reporter& rep) -> int = 0;
};

// Measures GPU utilization/memory/power/clock under sustained synthetic-tensor
// inference load with a single engine, identifies CPU-vs-GPU bottleneck,
// optionally compares against the CPU backend.
class GpuEfficiencyTest : public PerfTest {
public:
    auto name() const -> std::string override { return "gpu-efficiency"; }
    auto run(const TestConfig& cfg, Reporter& rep) -> int override;
};

// Stage 2 experiment: raises GPU utilization by running N independent ncnn::Net
// instances (each its own VkCompute + thread), each calling process() on its own
// tile. Two threads each calling submit_and_wait() on their OWN VkCompute submit
// to the shared Vulkan queue; the driver can keep the queue fuller than one
// serialized submitter can. Respects ncnn's single-queue threading model.
class GpuConcurrencyTest : public PerfTest {
public:
    auto name() const -> std::string override { return "gpu-concurrency"; }
    auto run(const TestConfig& cfg, Reporter& rep) -> int override;
};

// Stage 3 experiment: A/B test of VkCompute reuse. Mode A calls the engine's
// process() (fresh VkCompute per tile); Mode B drives ncnn directly and reuses
// one VkCompute per worker thread via cmd.reset(). Verifies output equivalence
// before timing. Isolates whether per-tile VkCompute construction is the
// bottleneck behind GpuConcurrencyTest's 75% overlap efficiency.
class GpuReuseTest : public PerfTest {
public:
    auto name() const -> std::string override { return "gpu-reuse"; }
    auto run(const TestConfig& cfg, Reporter& rep) -> int override;
};

} // namespace upix::perf
