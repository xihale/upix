#include "benchmark.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

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

    double total = std::accumulate(times.begin(), times.end(), 0.0);
    double min_t = *std::min_element(times.begin(), times.end());
    double max_t = *std::max_element(times.begin(), times.end());

    BenchmarkResult result;
    result.name = name;
    result.iterations = iterations;
    result.total_time_ms = total;
    result.avg_time_ms = total / iterations;
    result.min_time_ms = min_t;
    result.max_time_ms = max_t;
    result.ops_per_second = (iterations / total) * 1000.0;

    return result;
}

PerfStats compute_stats(const std::vector<double>& samples) {
    PerfStats stats{};
    
    if (samples.empty()) return stats;

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    stats.mean = sum / sorted.size();
    stats.min = sorted.front();
    stats.max = sorted.back();

    size_t n = sorted.size();
    stats.median = (n % 2 == 0) 
        ? (sorted[n/2 - 1] + sorted[n/2]) / 2.0 
        : sorted[n/2];

    stats.p95 = sorted[static_cast<size_t>(n * 0.95)];
    stats.p99 = sorted[static_cast<size_t>(n * 0.99)];

    double sq_sum = 0.0;
    for (double s : sorted) {
        sq_sum += (s - stats.mean) * (s - stats.mean);
    }
    stats.stddev = std::sqrt(sq_sum / n);

    return stats;
}

} // namespace crossscale
