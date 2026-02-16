#pragma once

#include <string>
#include <chrono>
#include <functional>
#include <vector>

namespace crossscale {

// Simple timing utility
class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    
    void reset() {
        start_ = std::chrono::high_resolution_clock::now();
    }
    
    double elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
    
    double elapsed_seconds() const {
        return elapsed_ms() / 1000.0;
    }
    
private:
    std::chrono::high_resolution_clock::time_point start_;
};

// Benchmark result
struct BenchmarkResult {
    std::string name;
    int iterations;
    double total_time_ms;
    double avg_time_ms;
    double min_time_ms;
    double max_time_ms;
    double ops_per_second;
};

// Run a benchmark
BenchmarkResult benchmark(const std::string& name, 
                          std::function<void()> func,
                          int iterations = 10,
                          int warmup = 2);

// Performance statistics
struct PerfStats {
    double mean;
    double stddev;
    double min;
    double max;
    double median;
    double p95;  // 95th percentile
    double p99;  // 99th percentile
};

PerfStats compute_stats(const std::vector<double>& samples);

} // namespace crossscale
