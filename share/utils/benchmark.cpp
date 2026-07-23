#include "benchmark.hpp"
#include <numeric>
#include <vector>

namespace crossscale {

BenchmarkResult benchmark(const std::string& name,
                          std::function<void()> func,
                          int iterations,
                          int warmup) {
    for (int i = 0; i < warmup; ++i) {
        func();
    }

    std::vector<double> times;
    times.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        Timer timer;
        func();
        times.push_back(timer.elapsed_ms());
    }

    const double total = std::accumulate(times.begin(), times.end(), 0.0);

    BenchmarkResult result;
    result.name = name;
    result.iterations = iterations;
    result.avg_time_ms = total / iterations;
    result.ops_per_second = (iterations / total) * 1000.0;

    return result;
}

} // namespace crossscale
