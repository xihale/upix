#include "perf/runner.hpp"
#include "perf/test.hpp"
#include "perf/reporter.hpp"
#include "utils/logger.hpp"

#include <print>

namespace crossscale::perf {

auto run_test(const std::string& test_name, const TestConfig& cfg, const std::string& out_path) -> int {
    Reporter rep(out_path);
    if (!rep.open()) {
        LOG_ERROR("Perf", "failed to open output: " + out_path);
        return 1;
    }

    std::unique_ptr<PerfTest> test;
    if (test_name == "gpu-efficiency") {
        test = std::make_unique<GpuEfficiencyTest>();
    } else if (test_name == "gpu-concurrency") {
        test = std::make_unique<GpuConcurrencyTest>();
    } else {
        LOG_ERROR("Perf", "unknown test: " + test_name);
        LOG_INFO("Perf", "available: gpu-efficiency, gpu-concurrency");
        return 1;
    }

    LOG_INFO("Perf", "running test: " + test_name +
             (out_path.empty() ? "" : (" → " + out_path)));
    return test->run(cfg, rep);
}

auto list_available() -> void {
    std::print("Available perf tests:\n");
    std::print("  gpu-efficiency   GPU utilization/memory/power under synthetic-tensor load\n");
    std::print("  gpu-concurrency  N independent GPU engines/threads; measures queue saturation\n");
    std::print("\nProbes:\n");
    std::print("  timer            high-resolution per-tile timing\n");
    std::print("  nvml             GPU util/mem/power/clock via NVML (runtime-loaded)\n");
    std::print("\nReserved interfaces (not yet implemented):\n");
    std::print("  tile-sweep       tile_size × overlap × batch grid scan\n");
    std::print("  precision-sweep  fp16/int8/arith config sweep\n");
    std::print("  distributed      multi-device weight/throughput efficiency\n");
}

} // namespace crossscale::perf
