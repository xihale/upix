// `crossscale benchmark` — a synthetic CPU micro-benchmark for sanity-checking timings.
#include <iostream>

#include "args.hpp"
#include "cli_app.hpp"
#include "utils/benchmark.hpp"
#include "utils/logger.hpp"

int cmd_benchmark(const args::Benchmark&) {
    LOG_INFO("Benchmark", "Running performance benchmark...");

    crossscale::Timer timer;
    auto result = crossscale::benchmark("test_iteration", []() {
        volatile int x = 0;
        for (int i = 0; i < 1000000; ++i) x += i;
        (void)x;
    }, 10, 2);

    std::cout << "Benchmark: " << result.name << "\n"
              << "  Iterations: " << result.iterations << "\n"
              << "  Avg time: " << result.avg_time_ms << " ms\n"
              << "  Ops/sec: " << result.ops_per_second << "\n";
    (void)timer;
    return 0;
}
