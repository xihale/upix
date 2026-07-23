#include "perf/test.hpp"
#include "perf/probes.hpp"
#include "perf/reporter.hpp"
#include "perf/workload.hpp"

#include "core/engine.hpp"
#include "utils/benchmark.hpp"  // Timer
#include "utils/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <print>
#include <thread>
#include <vector>

namespace crossscale::perf {

namespace {

// Percentile on a sorted copy of v. pct in [0,100].
auto percentile(std::vector<double> v, double pct) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::floor((pct / 100.0) * (v.size() - 1)));
    return v[idx];
}

auto percentile_u(std::vector<unsigned> v, double pct) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::floor((pct / 100.0) * (v.size() - 1)));
    return static_cast<double>(v[idx]);
}

// Run one backend (CPU or GPU) under load, sampling the probe in a background
// thread. Returns the per-tile timing samples and the probe samples collected.
struct PhaseResult {
    std::vector<double> tile_ms;          // per-iteration elapsed
    std::vector<Sample> samples;          // probe samples over the run
    double wall_ms = 0.0;
    int iterations = 0;
};

auto run_phase(Engine& engine, const TestConfig& cfg, DeviceType dev,
               NvmlProbe& nvml, Workload& wl, Reporter& rep,
               std::string_view device_label) -> PhaseResult {
    PhaseResult r;
    std::atomic<bool> sampling{true};
    std::vector<Sample>& samples = r.samples;

    // Background sampler. Independent of timing so it never perturbs it.
    std::thread sampler([&] {
        using namespace std::chrono;
        auto next = steady_clock::now();
        const auto interval = milliseconds(cfg.sample_ms);
        while (sampling.load(std::memory_order_relaxed)) {
            samples.push_back(nvml.sample());
            next += interval;
            std::this_thread::sleep_until(next);
        }
    });

    Timer wall;
    TimerProbe tile_timer;
    tile_timer.start();

    // Warmup: run until warmup window elapses (discard timing).
    Timer warm_timer;
    while (cfg.warmup_s > 0 && warm_timer.elapsed_seconds() < cfg.warmup_s) {
        auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        std::atomic<bool> cancel{false};
        auto res = engine.process(tp.input.get(), tp.output.get(),
                                  cfg.tile_width, cfg.tile_height, cfg.channels, cancel);
        if (!res) {
            LOG_ERROR("Perf", "warmup process failed: " + res.error().message);
            break;
        }
    }

    // Timed phase.
    Timer phase_timer;
    int i = 0;
    auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
    std::atomic<bool> cancel{false};
    while (true) {
        tile_timer.tic();
        auto res = engine.process(tp.input.get(), tp.output.get(),
                                  cfg.tile_width, cfg.tile_height, cfg.channels, cancel);
        double ms = tile_timer.toc();
        if (!res) {
            LOG_ERROR("Perf", "process failed: " + res.error().message);
            break;
        }
        r.tile_ms.push_back(ms);
        rep.emit_tile(i, ms);
        ++i;
        if (cfg.iterations > 0) {
            if (i >= cfg.iterations) break;
        } else {
            if (phase_timer.elapsed_seconds() >= cfg.duration_s) break;
        }
    }
    r.wall_ms = phase_timer.elapsed_ms();
    r.iterations = i;

    sampling.store(false, std::memory_order_relaxed);
    sampler.join();

    // Emit samples for this phase.
    for (const auto& s : samples) {
        rep.emit_sample(s, device_label);
    }
    (void)wall;
    (void)dev;
    return r;
}

// Summarize one phase and emit a summary event + console block.
auto summarize(const PhaseResult& r, std::string_view device_label, Reporter& rep) -> double {
    double tiles_per_s = 0.0;
    if (r.wall_ms > 0.0) tiles_per_s = r.iterations / (r.wall_ms / 1000.0);

    double ms_avg = 0.0, ms_p50 = 0.0, ms_p99 = 0.0;
    if (!r.tile_ms.empty()) {
        double sum = 0.0;
        for (double m : r.tile_ms) sum += m;
        ms_avg = sum / r.tile_ms.size();
        ms_p50 = percentile(r.tile_ms, 50.0);
        ms_p99 = percentile(r.tile_ms, 99.0);
    }

    // GPU metrics (only present if NVML was sampling — i.e. GPU phase).
    double gpu_util_avg = 0.0, gpu_util_p50 = 0.0, gpu_util_p99 = 0.0, gpu_util_peak = 0.0;
    double active_ratio = 0.0;
    double mem_avg = 0.0, power_avg = 0.0, power_peak = 0.0;
    if (!r.samples.empty()) {
        std::vector<unsigned> utils, mems, powers;
        for (const auto& s : r.samples) {
            if (s.gpu_util_pct) utils.push_back(*s.gpu_util_pct);
            if (s.mem_used_mb) mems.push_back(*s.mem_used_mb);
            if (s.power_w) powers.push_back(*s.power_w);
        }
        if (!utils.empty()) {
            double sum = 0.0;
            for (auto u : utils) sum += u;
            gpu_util_avg = sum / utils.size();
            gpu_util_p50 = percentile_u(utils, 50.0);
            gpu_util_p99 = percentile_u(utils, 99.0);
            gpu_util_peak = static_cast<double>(*std::max_element(utils.begin(), utils.end()));
            // active_ratio = fraction of samples where util >= 10% (GPU awake).
            int active = 0;
            for (auto u : utils) if (u >= 10) ++active;
            active_ratio = static_cast<double>(active) / utils.size() * 100.0;
        }
        if (!mems.empty()) {
            double sum = 0.0;
            for (auto m : mems) sum += m;
            mem_avg = sum / mems.size();
        }
        if (!powers.empty()) {
            double sum = 0.0;
            for (auto p : powers) sum += p;
            power_avg = sum / powers.size();
            power_peak = static_cast<double>(*std::max_element(powers.begin(), powers.end()));
        }
    }

    // Summary JSONL.
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf),
        "\"device\":\"%.*s\",\"iterations\":%d,\"tiles_per_s\":%.4f,"
        "\"ms_avg\":%.3f,\"ms_p50\":%.3f,\"ms_p99\":%.3f,"
        "\"gpu_util_avg\":%.2f,\"gpu_util_p50\":%.2f,\"gpu_util_p99\":%.2f,\"gpu_util_peak\":%.2f,"
        "\"active_ratio\":%.2f,\"mem_avg_mb\":%.2f,\"power_avg_w\":%.2f,\"power_peak_w\":%.2f",
        static_cast<int>(device_label.size()), device_label.data(),
        r.iterations, tiles_per_s, ms_avg, ms_p50, ms_p99,
        gpu_util_avg, gpu_util_p50, gpu_util_p99, gpu_util_peak,
        active_ratio, mem_avg, power_avg, power_peak);
    rep.emit_summary(std::string_view(buf, n));

    // Console block.
    std::print("\n┌─ [{}] ─────────────────────────────────\n", std::string(device_label));
    std::print("│ iterations   : {}\n", r.iterations);
    std::print("│ wall time    : {:.1f} ms\n", r.wall_ms);
    std::print("│ tiles/sec    : {:.2f}\n", tiles_per_s);
    std::print("│ tile latency : avg {:.3f} ms | p50 {:.3f} ms | p99 {:.3f} ms\n",
               ms_avg, ms_p50, ms_p99);
    if (!r.samples.empty()) {
        std::print("│ GPU util     : avg {:.1f}% | p50 {:.1f}% | p99 {:.1f}% | peak {:.1f}%\n",
                   gpu_util_avg, gpu_util_p50, gpu_util_p99, gpu_util_peak);
        std::print("│ active_ratio : {:.1f}%  (samples with util ≥ 10%)\n", active_ratio);
        std::print("│ GPU memory   : avg {:.0f} MB\n", mem_avg);
        std::print("│ GPU power    : avg {:.1f} W | peak {:.1f} W\n", power_avg, power_peak);
    }
    std::print("└─────────────────────────────────────────\n");

    return tiles_per_s;
}

} // namespace

auto GpuEfficiencyTest::run(const TestConfig& cfg, Reporter& rep) -> int {
    Workload wl;

    NvmlProbe nvml;
    bool has_nvml = nvml.start();
    if (!has_nvml) {
        LOG_WARNING("Perf", "NVML unavailable (no NVIDIA driver or libnvidia-ml); "
                            "GPU metrics will be absent, timing-only mode.");
    }

    // Meta event: device + config context.
    auto engine = create_ncnn_engine();
    auto init_res = engine->initialize(DeviceType::GPU_VULKAN);
    if (!init_res) {
        LOG_ERROR("Perf", "GPU init failed: " + init_res.error().message);
        nvml.stop();
        return 2;
    }
    auto load_res = engine->load_model(cfg.model_param, cfg.model_bin);
    if (!load_res) {
        LOG_ERROR("Perf", "model load failed: " + load_res.error().message);
        nvml.stop();
        return 3;
    }

    // Capture GPU identity for the meta event. Prefer NVML's marketing name
    // (accurate); fall back to the engine's generic "Vulkan GPU-N" label.
    std::string gpu_name = "unknown";
    if (has_nvml) {
        gpu_name = nvml.device_name();
    }
    if (gpu_name.empty()) {
        for (const auto& d : engine->list_devices()) {
            if (d.type == DeviceType::GPU_VULKAN) { gpu_name = d.name; break; }
        }
    }
    auto mem = engine->get_memory_info();

    char meta[512];
    int n = std::snprintf(meta, sizeof(meta),
        "\"test\":\"gpu-efficiency\",\"model\":\"%s\",\"scale\":%d,"
        "\"tile_w\":%d,\"tile_h\":%d,\"channels\":%d,\"duration_s\":%.1f,\"warmup_s\":%.1f,"
        "\"device\":\"%s\",\"vram_mb\":%zu,\"nvml\":%s",
        cfg.model_name.c_str(), cfg.scale,
        cfg.tile_width, cfg.tile_height, cfg.channels, cfg.duration_s, cfg.warmup_s,
        gpu_name.c_str(), mem.first, has_nvml ? "true" : "false");
    rep.emit_meta(std::string_view(meta, n));

    std::print("\n═══ GPU Efficiency Test ═══\n");
    std::print("model: {} ({}x), tile {}x{}x{}, warmup {:.1f}s, duration {:.1f}s\n",
               cfg.model_name, cfg.scale, cfg.tile_width, cfg.tile_height, cfg.channels,
               cfg.warmup_s, cfg.duration_s);
    std::print("GPU: {}, VRAM {:.0f} MB, NVML {}\n",
               gpu_name, static_cast<double>(mem.first), has_nvml ? "active" : "absent");

    // ---- GPU phase ----
    double gpu_tps = 0.0;
    {
        auto r = run_phase(*engine, cfg, DeviceType::GPU_VULKAN, nvml, wl, rep, "gpu");
        gpu_tps = summarize(r, "gpu", rep);
    }

    // ---- CPU phase (optional baseline) ----
    double cpu_tps = 0.0;
    if (cfg.cpu_baseline) {
        auto cpu_engine = create_ncnn_engine();
        auto ci = cpu_engine->initialize(DeviceType::CPU);
        if (!ci) {
            LOG_WARNING("Perf", "CPU init failed, skipping baseline: " + ci.error().message);
        } else {
            auto cl = cpu_engine->load_model(cfg.model_param, cfg.model_bin);
            if (!cl) {
                LOG_WARNING("Perf", "CPU model load failed, skipping baseline: " + cl.error().message);
            } else {
                auto r = run_phase(*cpu_engine, cfg, DeviceType::CPU, nvml, wl, rep, "cpu");
                cpu_tps = summarize(r, "cpu", rep);
            }
        }
    }

    // ---- Comparison ----
    if (cfg.cpu_baseline && gpu_tps > 0.0) {
        rep.emit_comparison(gpu_tps, cpu_tps);
        double speedup = cpu_tps > 0.0 ? gpu_tps / cpu_tps : 0.0;
        std::print("\n── Comparison ──\n");
        std::print("GPU {:.2f} tiles/s  vs  CPU {:.2f} tiles/s  →  speedup ×{:.2f}\n",
                   gpu_tps, cpu_tps, speedup);
    }

    if (has_nvml) {
        // Bottleneck hint: if GPU util is low, the GPU is starving (CPU-bound).
        // Computed from the GPU phase samples we already summarized.
    }

    nvml.stop();
    return 0;
}

} // namespace crossscale::perf
