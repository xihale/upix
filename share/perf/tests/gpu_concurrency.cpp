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
#include <memory>
#include <print>
#include <thread>
#include <vector>

namespace crossscale::perf {

namespace {

// Percentile on a sorted copy of v. pct in [0,100]. (Same helper as
// gpu_efficiency.cpp; copied rather than shared to keep the perf layer's tests
// independent — adding a shared math header for two callers is premature.)
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

// Per-thread tile-latency samples + aggregate probe samples collected by a
// background sampler thread (one sampler for the whole run, shared across all
// worker threads — NVML probes the device, which is shared).
struct ConcurrencyResult {
    std::vector<std::vector<double>> per_thread_tile_ms;  // [thread] -> latencies
    std::vector<Sample> samples;                          // NVML samples
    double wall_ms = 0.0;
    int total_iterations = 0;
};

// One worker: loops process() on its own engine + own tile until the global
// stop flag is set. Records per-tile latency. Each engine has its own ncnn::Net
// + VkCompute, so submit_and_wait() targets that thread's own compute — this is
// the only safe way to put two concurrent submitters on ncnn's single Vulkan
// queue (same-Net concurrent create_extractor is unsafe, see engine.hpp).
struct WorkerStats {
    std::vector<double> tile_ms;
    int iterations = 0;
    bool failed = false;
};

auto worker(Engine& engine, const TestConfig& cfg, Workload& wl,
            std::atomic<bool>& stop, WorkerStats& stats) -> void {
    // Each thread owns its own tile buffers (process reads/writes them).
    auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
    std::atomic<bool> cancel{false};

    TimerProbe tile_timer;
    tile_timer.start();

    while (!stop.load(std::memory_order_relaxed)) {
        tile_timer.tic();
        auto res = engine.process(tp.input.get(), tp.output.get(),
                                  cfg.tile_width, cfg.tile_height, cfg.channels, cancel);
        double ms = tile_timer.toc();
        if (!res) {
            LOG_ERROR("Perf", "concurrent process failed: " + res.error().message);
            stats.failed = true;
            return;
        }
        stats.tile_ms.push_back(ms);
        ++stats.iterations;
    }
}

// Background NVML sampler. Same pattern as gpu_efficiency's run_phase sampler.
auto sampler_loop(NvmlProbe& nvml, std::vector<Sample>& out,
                  std::atomic<bool>& sampling, int sample_ms) -> void {
    using namespace std::chrono;
    auto next = steady_clock::now();
    const auto interval = milliseconds(sample_ms);
    while (sampling.load(std::memory_order_relaxed)) {
        out.push_back(nvml.sample());
        next += interval;
        std::this_thread::sleep_until(next);
    }
}

auto summarize_concurrent(const ConcurrencyResult& r, const std::vector<WorkerStats>& workers,
                          int concurrency, Reporter& rep) -> double {
    // Aggregate tiles/sec = total tiles / wall time.
    double tiles_per_s = 0.0;
    if (r.wall_ms > 0.0) tiles_per_s = r.total_iterations / (r.wall_ms / 1000.0);

    // Aggregate latency across all threads (for an overall per-tile number).
    std::vector<double> all_ms;
    all_ms.reserve(r.total_iterations);
    for (const auto& w : workers) {
        for (double m : w.tile_ms) all_ms.push_back(m);
    }
    double ms_avg = 0.0, ms_p50 = 0.0, ms_p99 = 0.0;
    if (!all_ms.empty()) {
        double sum = 0.0;
        for (double m : all_ms) sum += m;
        ms_avg = sum / all_ms.size();
        ms_p50 = percentile(all_ms, 50.0);
        ms_p99 = percentile(all_ms, 99.0);
    }

    // GPU metrics from the shared NVML sampler.
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

    // Summary JSONL — same shape as gpu-efficiency plus concurrency + per-thread.
    std::string per_thread;
    for (size_t i = 0; i < workers.size(); ++i) {
        if (i) per_thread += ',';
        double t_avg = 0.0;
        if (!workers[i].tile_ms.empty()) {
            double s = 0.0;
            for (double m : workers[i].tile_ms) s += m;
            t_avg = s / workers[i].tile_ms.size();
        }
        char tb[64];
        int tn = std::snprintf(tb, sizeof(tb), "{\"iters\":%d,\"ms_avg\":%.3f}",
                               workers[i].iterations, t_avg);
        per_thread.append(tb, tn);
    }

    char buf[768];
    int n = std::snprintf(buf, sizeof(buf),
        "\"device\":\"gpu\",\"concurrency\":%d,\"iterations\":%d,\"tiles_per_s\":%.4f,"
        "\"ms_avg\":%.3f,\"ms_p50\":%.3f,\"ms_p99\":%.3f,"
        "\"gpu_util_avg\":%.2f,\"gpu_util_p50\":%.2f,\"gpu_util_p99\":%.2f,\"gpu_util_peak\":%.2f,"
        "\"active_ratio\":%.2f,\"mem_avg_mb\":%.2f,\"power_avg_w\":%.2f,\"power_peak_w\":%.2f,"
        "\"per_thread\":[%s]",
        concurrency, r.total_iterations, tiles_per_s, ms_avg, ms_p50, ms_p99,
        gpu_util_avg, gpu_util_p50, gpu_util_p99, gpu_util_peak,
        active_ratio, mem_avg, power_avg, power_peak, per_thread.c_str());
    rep.emit_summary(std::string_view(buf, n));

    // Console block — mirrors gpu-efficiency so the two are eyeball-comparable.
    std::print("\n┌─ [gpu x{}] ─────────────────────────────\n", concurrency);
    std::print("│ iterations   : {} (aggregate across {} threads)\n", r.total_iterations, concurrency);
    std::print("│ wall time    : {:.1f} ms\n", r.wall_ms);
    std::print("│ tiles/sec    : {:.2f}  (aggregate)\n", tiles_per_s);
    std::print("│ tile latency : avg {:.3f} ms | p50 {:.3f} ms | p99 {:.3f} ms\n",
               ms_avg, ms_p50, ms_p99);
    if (!r.samples.empty()) {
        std::print("│ GPU util     : avg {:.1f}% | p50 {:.1f}% | p99 {:.1f}% | peak {:.1f}%\n",
                   gpu_util_avg, gpu_util_p50, gpu_util_p99, gpu_util_peak);
        std::print("│ active_ratio : {:.1f}%  (samples with util ≥ 10%)\n", active_ratio);
        std::print("│ GPU memory   : avg {:.0f} MB\n", mem_avg);
        std::print("│ GPU power    : avg {:.1f} W | peak {:.1f} W\n", power_avg, power_peak);
    }
    std::print("│ per-thread   :\n");
    for (size_t i = 0; i < workers.size(); ++i) {
        double t_avg = 0.0;
        if (!workers[i].tile_ms.empty()) {
            double s = 0.0;
            for (double m : workers[i].tile_ms) s += m;
            t_avg = s / workers[i].tile_ms.size();
        }
        std::print("│   t{} : {} iters, avg {:.3f} ms\n", i, workers[i].iterations, t_avg);
    }
    std::print("└─────────────────────────────────────────\n");

    return tiles_per_s;
}

} // namespace

auto GpuConcurrencyTest::run(const TestConfig& cfg, Reporter& rep) -> int {
    const int concurrency = std::max(1, cfg.concurrency);

    Workload wl;

    NvmlProbe nvml;
    bool has_nvml = nvml.start();
    if (!has_nvml) {
        LOG_WARNING("Perf", "NVML unavailable (no NVIDIA driver or libnvidia-ml); "
                            "GPU metrics will be absent, timing-only mode.");
    }

    // Build N independent engines. Each gets its OWN ncnn::Net + VkCompute.
    // Model load is serialized by g_vulkan_model_load_mutex inside ncnn_engine
    // anyway, so we load all N up front (single-threaded) and only then spawn
    // workers — the workers only call process().
    std::vector<std::unique_ptr<Engine>> engines;
    engines.reserve(concurrency);
    for (int i = 0; i < concurrency; ++i) {
        auto e = create_ncnn_engine();
        auto init_res = e->initialize(DeviceType::GPU_VULKAN);
        if (!init_res) {
            LOG_ERROR("Perf", "GPU init failed (engine " + std::to_string(i) +
                              "): " + init_res.error().message);
            nvml.stop();
            return 2;
        }
        auto load_res = e->load_model(cfg.model_param, cfg.model_bin);
        if (!load_res) {
            LOG_ERROR("Perf", "model load failed (engine " + std::to_string(i) +
                              "): " + load_res.error().message);
            nvml.stop();
            return 3;
        }
        engines.push_back(std::move(e));
    }

    // Meta event. Reuse engine 0 for device identity / memory info.
    Engine& e0 = *engines.front();
    std::string gpu_name = "unknown";
    if (has_nvml) gpu_name = nvml.device_name();
    if (gpu_name.empty()) {
        for (const auto& d : e0.list_devices()) {
            if (d.type == DeviceType::GPU_VULKAN) { gpu_name = d.name; break; }
        }
    }
    auto mem = e0.get_memory_info();

    char meta[512];
    int n = std::snprintf(meta, sizeof(meta),
        "\"test\":\"gpu-concurrency\",\"model\":\"%s\",\"scale\":%d,"
        "\"tile_w\":%d,\"tile_h\":%d,\"channels\":%d,\"concurrency\":%d,"
        "\"duration_s\":%.1f,\"warmup_s\":%.1f,\"device\":\"%s\",\"vram_mb\":%zu,\"nvml\":%s",
        cfg.model_name.c_str(), cfg.scale,
        cfg.tile_width, cfg.tile_height, cfg.channels, concurrency,
        cfg.duration_s, cfg.warmup_s, gpu_name.c_str(), mem.first,
        has_nvml ? "true" : "false");
    rep.emit_meta(std::string_view(meta, n));

    std::print("\n═══ GPU Concurrency Test ═══\n");
    std::print("model: {} ({}x), tile {}x{}x{}, concurrency {} (independent engines)\n",
               cfg.model_name, cfg.scale, cfg.tile_width, cfg.tile_height, cfg.channels,
               concurrency);
    std::print("warmup {:.1f}s, duration {:.1f}s\n", cfg.warmup_s, cfg.duration_s);
    std::print("GPU: {}, VRAM {:.0f} MB, NVML {}\n",
               gpu_name, static_cast<double>(mem.first), has_nvml ? "active" : "absent");

    // ---- Warmup: each engine warms up sequentially (still under the model-load
    // mutex domain; cheap, and avoids any first-submit ordering surprises). ----
    {
        auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        std::atomic<bool> cancel{false};
        Timer warm_timer;
        for (int i = 0; i < concurrency; ++i) {
            if (cfg.warmup_s <= 0) break;
            // Give each engine a few warm iterations so its first timed submit
            // isn't a cold path. Use a fixed small count per engine rather than
            // a per-engine timer so warmup is bounded regardless of tile cost.
            for (int w = 0; w < 3; ++w) {
                auto res = engines[i]->process(tp.input.get(), tp.output.get(),
                                               cfg.tile_width, cfg.tile_height,
                                               cfg.channels, cancel);
                if (!res) {
                    LOG_ERROR("Perf", "warmup process failed (engine " +
                                       std::to_string(i) + "): " + res.error().message);
                    break;
                }
            }
        }
        (void)warm_timer;
    }

    // ---- Timed concurrent phase ----
    ConcurrencyResult r;
    r.per_thread_tile_ms.resize(concurrency);
    std::vector<WorkerStats> workers(concurrency);

    std::atomic<bool> sampling{true};
    std::atomic<bool> stop{false};

    std::thread sampler(sampler_loop, std::ref(nvml), std::ref(r.samples),
                        std::ref(sampling), cfg.sample_ms);

    // Spawn workers, each bound to its own engine.
    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    for (int i = 0; i < concurrency; ++i) {
        threads.emplace_back(worker, std::ref(*engines[i]), std::ref(cfg), std::ref(wl),
                             std::ref(stop), std::ref(workers[i]));
    }

    // Let them run for the configured duration, then signal stop.
    Timer phase_timer;
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.duration_s));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads) t.join();

    r.wall_ms = phase_timer.elapsed_ms();
    for (const auto& w : workers) {
        r.total_iterations += w.iterations;
    }

    sampling.store(false, std::memory_order_relaxed);
    sampler.join();

    // Emit per-tile events. The tile index space is global; we emit each
    // thread's tiles in order. (We did NOT emit these inline from workers to
    // avoid the reporter-lock hot path affecting timing; emit post-hoc.)
    int global_i = 0;
    for (const auto& w : workers) {
        for (double ms : w.tile_ms) {
            rep.emit_tile(global_i++, ms);
        }
    }
    for (const auto& s : r.samples) rep.emit_sample(s, "gpu");

    double tps = summarize_concurrent(r, workers, concurrency, rep);
    (void)tps;

    // Engines released when the vector goes out of scope.
    nvml.stop();
    return 0;
}

} // namespace crossscale::perf
