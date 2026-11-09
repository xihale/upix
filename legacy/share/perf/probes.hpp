// Performance probes: pluggable samplers that collect metrics during a test run.
// A Probe samples one category of metric (GPU util, memory, power, timing...).
// New metric → new probe; new test scenario → new PerfTest. The two axes compose.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace upix::perf {

// A single point-in-time observation. Only fields the probe actually populates
// are set; the rest are std::nullopt and serialized as null.
struct Sample {
    double t_ms = 0.0;  // milliseconds since probe start

    // GPU metrics (populated by NvmlProbe; nullopt otherwise)
    std::optional<unsigned> gpu_util_pct;        // SM activity 0-100
    std::optional<unsigned> mem_used_mb;
    std::optional<unsigned> mem_total_mb;
    std::optional<unsigned> power_w;             // draw in watts
    std::optional<unsigned> sm_clock_mhz;        // SM clock
    std::optional<unsigned> graphics_clock_mhz;  // graphics clock

    // CPU metrics (populated by CpuProbe; nullopt otherwise)
    std::optional<unsigned> cpu_util_pct;        // aggregate 0-100
};

// A Probe samples one metric category during a test run.
class Probe {
public:
    virtual ~Probe() = default;
    virtual auto name() const -> std::string = 0;
    // Returns false if the metric source is unavailable (e.g. no NVML).
    // A false return lets the test degrade gracefully and still emit timing.
    virtual auto start() -> bool = 0;
    virtual auto sample() -> Sample = 0;  // synchronous; caller drives cadence
    virtual void stop() = 0;
};

// Wall-clock probe for per-iteration timing. Not background-sampled; instead the
// test calls tic()/toc() around the unit of work.
class TimerProbe : public Probe {
public:
    auto name() const -> std::string override { return "timer"; }
    auto start() -> bool override;
    auto sample() -> Sample override;  // returns elapsed since last reset
    void stop() override {}

    void tic();              // mark start of a unit
    auto toc() -> double;    // ms since tic(); does not reset
    auto toc_reset() -> double;  // ms since tic(), then resets for next unit

private:
    std::chrono::high_resolution_clock::time_point start_time_{};
    std::chrono::high_resolution_clock::time_point tic_time_{};
    bool started_ = false;
};

// NVML probe via dlopen. Zero compile-time dependency on nvidia-ml dev headers.
// Loads libnvidia-ml.so.1 at runtime; start() returns false if unavailable.
class NvmlProbe : public Probe {
public:
    auto name() const -> std::string override { return "nvml"; }
    auto start() -> bool override;
    auto sample() -> Sample override;
    void stop() override;

    // Returns the GPU's marketing name (e.g. "NVIDIA GeForce RTX 4060 Laptop GPU").
    // Empty if NVML is not active.
    auto device_name() const -> std::string;

private:
    void* lib_handle_ = nullptr;        // dlopen handle
    void* nvml_handle_ = nullptr;       // nvmlDevice_t (opaque)
    void* api_handle_ = nullptr;        // points to NvmlApi (defined in .cpp), opaque here
    std::chrono::high_resolution_clock::time_point start_time_{};
    bool initialized_ = false;
};

} // namespace upix::perf
