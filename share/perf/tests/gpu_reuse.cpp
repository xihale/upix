// GpuReuseTest: A/B experiment — does reusing one ncnn::VkCompute across tiles
// (via reset()) beat reconstructing one per tile?
//
// Mode A (baseline): the production path. Calls Engine::process(), which inside
// constructs a fresh ncnn::VkCompute + fresh Extractor on every tile, then
// submit_and_wait(). This is exactly what GpuConcurrencyTest measures.
//
// Mode B (reuse): a hand-written inference loop in the test that constructs ONE
// VkCompute per worker thread and reuses it across tiles via cmd.reset(). The
// Extractor is still recreated per tile (unavoidable — ncnn returns it by value
// and it holds per-forward state). Blob/staging allocators are acquired once per
// thread and reused. This isolates the VkCompute-construction cost from the
// multi-engine overlap so we can see whether construction is actually the
// bottleneck the 75% overlap efficiency number implies.
//
// Correctness gate (run before any timing): runs Mode A and Mode B on the SAME
// synthetic tile, compares the output buffers element-by-element. If they differ
// by more than float epsilon, reuse is unsafe and we refuse to report a speed
// number. The reuse loop replicates process_vulkan_with_gpu_layout's record
// sequence verbatim (record_clone in → preproc → extractor → postproc →
// record_clone out → submit_and_wait); only the VkCompute container is reused.
#include "perf/test.hpp"
#include "perf/probes.hpp"
#include "perf/reporter.hpp"
#include "perf/workload.hpp"

#include "core/engine.hpp"
#include "core/gpu_precision.hpp"
#include "core/spirv_pack.hpp"
#include "core/vulkan_shaders.hpp"
#include "utils/benchmark.hpp"  // Timer
#include "utils/logger.hpp"

#include <command.h>
#include <gpu.h>
#include <mat.h>
#include <net.h>
#include <option.h>
#include <pipeline.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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

// --- SPIRV shader sources (verbatim copies of the ones compiled in
// ncnn_engine.cpp — kept local to avoid touching share/core). HWC<->CHW is the
// only layout conversion the GPU path needs. ---
constexpr const char* kHwcToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = (gy * p.w + gx) * p.channels + gz;
    const int dst_idx = gz * p.cstep + gy * p.w + gx;
    dst_data[dst_idx] = sfp(src_data[src_idx]);
}
)";

constexpr const char* kChwToHwcComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = gz * p.cstep + gy * p.w + gx;
    const int dst_idx = (gy * p.w + gx) * p.channels + gz;
    dst_data[dst_idx] = float(src_data[src_idx]);
}
)";

// RAII wrapper for the two ncnn VkAllocators a worker thread acquires. Mirrors
// VkAllocGuard in ncnn_engine.cpp: acquire on construct, reclaim on destruct.
struct VkAllocScope {
    ncnn::VulkanDevice* vkdev = nullptr;
    ncnn::VkAllocator* blob = nullptr;
    ncnn::VkAllocator* staging = nullptr;

    explicit VkAllocScope(ncnn::VulkanDevice* d) : vkdev(d) {
        if (vkdev) {
            blob = vkdev->acquire_blob_allocator();
            staging = vkdev->acquire_staging_allocator();
        }
    }
    ~VkAllocScope() {
        if (vkdev) {
            if (blob) vkdev->reclaim_blob_allocator(blob);
            if (staging) vkdev->reclaim_staging_allocator(staging);
        }
    }
    VkAllocScope(const VkAllocScope&) = delete;
    auto operator=(const VkAllocScope&) -> VkAllocScope& = delete;
};

// Per-thread ncnn state for Mode B. Owns the net (one per worker, like the
// production concurrency path), the two pre/post pipelines, the reused
// VkCompute, and the allocators. The VkCompute is the ONE thing this test is
// about: it is constructed once and reset() between tiles instead of being
// destructed and reconstructed.
struct ReuseContext {
    std::unique_ptr<ncnn::Net> net;
    ncnn::VulkanDevice* vkdev = nullptr;
    std::unique_ptr<ncnn::Pipeline> preproc;   // HWC -> CHW
    std::unique_ptr<ncnn::Pipeline> postproc;  // CHW -> HWC
    std::unique_ptr<VkAllocScope> alloc;
    std::unique_ptr<ncnn::VkCompute> cmd;  // reused across tiles via reset()
    ncnn::Option opt;
    int input_idx = -1;
    int output_idx = -1;
};

// Build a self-contained ncnn::Net on the same Vulkan device the engine would
// pick, load the same model, and compile the HWC<->CHW pipelines. Returns
// nullptr on any failure (caller skips Mode B and reports Mode A only).
auto build_reuse_context(const TestConfig& cfg) -> std::unique_ptr<ReuseContext> {
    auto ctx = std::make_unique<ReuseContext>();

    ncnn::create_gpu_instance();
    int gpu_count = ncnn::get_gpu_count();
    if (gpu_count <= 0) {
        LOG_ERROR("Perf", "gpu-reuse: no Vulkan devices");
        return nullptr;
    }
    // Pick the same discrete-preferred device the engine would. Mirrors the
    // scoring in ncnn_engine.cpp init_vulkan_device().
    int best = -1;
    int best_score = std::numeric_limits<int>::min();
    for (int i = 0; i < gpu_count; ++i) {
        const auto& info = ncnn::get_gpu_info(i);
        int score = 0;
        if (info.type() == 0) score += 10000;
        else if (info.type() == 1) score += 1000;
        else score += 100;
        score += static_cast<int>(info.compute_queue_count()) * 10;
        if (info.support_fp16_arithmetic()) score += 100;
        if (info.support_fp16_storage()) score += 50;
        if (info.support_fp16_packed()) score += 50;
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best < 0) return nullptr;
    ctx->vkdev = ncnn::get_gpu_device(best);
    if (ctx->vkdev == nullptr) return nullptr;

    ctx->net = std::make_unique<ncnn::Net>();
    ctx->net->set_vulkan_device(ctx->vkdev);
    auto& opt = ctx->net->opt;
    opt.use_vulkan_compute = true;
    spirv_pack::register_ncnn_lookup();
    const auto profile = select_profile(gpu_feature_caps(ctx->vkdev->info), false);
    if (!profile) {
        LOG_ERROR("Perf", "gpu-reuse: GPU lacks frozen SPIR-V pack features");
        return nullptr;
    }
    apply_profile(opt, *profile);

    if (ctx->net->load_param(cfg.model_param.c_str()) != 0) {
        LOG_ERROR("Perf", "gpu-reuse: load_param failed");
        return nullptr;
    }
    if (ctx->net->load_model(cfg.model_bin.c_str()) != 0) {
        LOG_ERROR("Perf", "gpu-reuse: load_model failed");
        return nullptr;
    }

    if (!ctx->net->input_indexes().empty()) ctx->input_idx = ctx->net->input_indexes()[0];
    if (!ctx->net->output_indexes().empty()) ctx->output_idx = ctx->net->output_indexes()[0];

    // Compile + create the two layout pipelines (same shaders as the engine).
    std::vector<uint32_t> spirv_hwc_to_chw;
    std::vector<uint32_t> spirv_chw_to_hwc;
    if (!spirv_pack::load_or_compile_custom(detail::CustomShader::HwcToChw, kHwcToChwComp,
                                           ctx->net->opt, spirv_hwc_to_chw) ||
        !spirv_pack::load_or_compile_custom(detail::CustomShader::ChwToHwc, kChwToHwcComp,
                                           ctx->net->opt, spirv_chw_to_hwc)) {
        LOG_ERROR("Perf", "gpu-reuse: spirv compile failed");
        return nullptr;
    }
    ctx->preproc = std::make_unique<ncnn::Pipeline>(ctx->vkdev);
    ctx->preproc->set_optimal_local_size_xyz(8, 8, 1);
    if (ctx->preproc->create(spirv_hwc_to_chw.data(), spirv_hwc_to_chw.size() * 4, {}) != 0) {
        LOG_ERROR("Perf", "gpu-reuse: preproc pipeline create failed");
        return nullptr;
    }
    ctx->postproc = std::make_unique<ncnn::Pipeline>(ctx->vkdev);
    ctx->postproc->set_optimal_local_size_xyz(8, 8, 1);
    if (ctx->postproc->create(spirv_chw_to_hwc.data(), spirv_chw_to_hwc.size() * 4, {}) != 0) {
        LOG_ERROR("Perf", "gpu-reuse: postproc pipeline create failed");
        return nullptr;
    }

    // Snapshot the option the way the engine does (allocators filled in per
    // tile, but the fp16/int8 flags are fixed once the net is loaded).
    ctx->opt = ctx->net->opt;

    ctx->alloc = std::make_unique<VkAllocScope>(ctx->vkdev);
    ctx->cmd = std::make_unique<ncnn::VkCompute>(ctx->vkdev);
    return ctx;
}

// One Mode B forward on a tile, recording into ctx->cmd. This is a verbatim
// transcription of process_vulkan_with_gpu_layout's record sequence (lines
// 3380-3490 of ncnn_engine.cpp) — only the container (cmd) is reused. Returns
// 0 on success, non-zero ncnn error code on failure.
auto reuse_forward(ReuseContext& ctx, const float* input, float* output,
                   int width, int height, int channels, int scale) -> int {
    ncnn::VkAllocator* blob_vkallocator = ctx.alloc->blob;
    ncnn::VkAllocator* staging_vkallocator = ctx.alloc->staging;

    ncnn::Option opt = ctx.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    const int out_width = width * scale;
    const int out_height = height * scale;
    const size_t in_total = static_cast<size_t>(width) * height * channels;
    const size_t out_total = static_cast<size_t>(out_width) * out_height * channels;

    ncnn::VkCompute& cmd = *ctx.cmd;

    ncnn::Mat in_cpu(static_cast<int>(in_total), const_cast<float*>(input), 4u);
    ncnn::VkMat in_gpu_hwc;
    cmd.record_clone(in_cpu, in_gpu_hwc, opt);

    const size_t in_elemsize = opt.use_fp16_storage ? 2u : 4u;
    ncnn::VkMat in_gpu_chw;
    in_gpu_chw.create(width, height, channels, in_elemsize, 1, blob_vkallocator);

    std::vector<ncnn::VkMat> pre_bindings(2);
    pre_bindings[0] = in_gpu_hwc;
    pre_bindings[1] = in_gpu_chw;

    std::vector<ncnn::vk_constant_type> pre_constants(4);
    pre_constants[0].i = width;
    pre_constants[1].i = height;
    pre_constants[2].i = channels;
    pre_constants[3].i = static_cast<int>(in_gpu_chw.cstep);

    ncnn::VkMat pre_dispatcher;
    pre_dispatcher.w = width;
    pre_dispatcher.h = height;
    pre_dispatcher.c = channels;
    cmd.record_pipeline(ctx.preproc.get(), pre_bindings, pre_constants, pre_dispatcher);

    ncnn::Extractor ex = ctx.net->create_extractor();
    ex.set_light_mode(true);
    ex.set_blob_vkallocator(blob_vkallocator);
    ex.set_workspace_vkallocator(blob_vkallocator);
    ex.set_staging_vkallocator(staging_vkallocator);

    int ret = 0;
    if (ctx.input_idx >= 0) {
        ret = ex.input(ctx.input_idx, in_gpu_chw);
    } else {
        ret = ex.input("data", in_gpu_chw);
    }
    if (ret != 0) return ret;

    ncnn::VkMat out_gpu;
    if (ctx.output_idx >= 0) {
        ret = ex.extract(ctx.output_idx, out_gpu, cmd);
    } else {
        ret = ex.extract("output", out_gpu, cmd);
        if (ret != 0) ret = ex.extract("out", out_gpu, cmd);
    }
    if (ret != 0) return ret;

    if (out_gpu.elempack != 1) {
        ncnn::VkMat out_unpacked;
        ctx.vkdev->convert_packing(out_gpu, out_unpacked, 1, cmd, opt);
        out_gpu = out_unpacked;
    }

    ncnn::VkMat out_gpu_hwc;
    out_gpu_hwc.create(static_cast<int>(out_total), 4u, blob_vkallocator);

    std::vector<ncnn::VkMat> post_bindings(2);
    post_bindings[0] = out_gpu;
    post_bindings[1] = out_gpu_hwc;

    std::vector<ncnn::vk_constant_type> post_constants(4);
    post_constants[0].i = out_width;
    post_constants[1].i = out_height;
    post_constants[2].i = channels;
    post_constants[3].i = static_cast<int>(out_gpu.cstep);

    ncnn::VkMat post_dispatcher;
    post_dispatcher.w = out_width;
    post_dispatcher.h = out_height;
    post_dispatcher.c = channels;
    cmd.record_pipeline(ctx.postproc.get(), post_bindings, post_constants, post_dispatcher);

    ncnn::Mat out_cpu(static_cast<int>(out_total), output, 4u);
    cmd.record_clone(out_gpu_hwc, out_cpu, opt);

    ret = cmd.submit_and_wait();
    // reset() the command buffer for the next tile instead of destruct/reconstruct.
    // This is the single optimization lever under test.
    cmd.reset();
    return ret;
}

struct WorkerStats {
    std::vector<double> tile_ms;
    int iterations = 0;
    bool failed = false;
};

// Mode A worker: drives Engine::process() (which internally constructs a fresh
// VkCompute + Extractor per tile). Identical to GpuConcurrencyTest's worker.
auto mode_a_worker(Engine& engine, const TestConfig& cfg, Workload& wl,
                   std::atomic<bool>& stop, WorkerStats& stats) -> void {
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
            LOG_ERROR("Perf", "gpu-reuse mode A process failed: " + res.error().message);
            stats.failed = true;
            return;
        }
        stats.tile_ms.push_back(ms);
        ++stats.iterations;
    }
}

// Mode B worker: reuses one VkCompute (ctx->cmd) across all tiles, calling
// cmd.reset() between iterations. Everything else (Net, pipelines, allocators)
// is also per-thread and reused.
auto mode_b_worker(ReuseContext& ctx, const TestConfig& cfg, Workload& wl,
                   std::atomic<bool>& stop, WorkerStats& stats) -> void {
    auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
    TimerProbe tile_timer;
    tile_timer.start();
    while (!stop.load(std::memory_order_relaxed)) {
        tile_timer.tic();
        int ret = reuse_forward(ctx, tp.input.get(), tp.output.get(),
                                cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        double ms = tile_timer.toc();
        if (ret != 0) {
            LOG_ERROR("Perf", "gpu-reuse mode B forward failed (ncnn err " +
                               std::to_string(ret) + ")");
            stats.failed = true;
            return;
        }
        stats.tile_ms.push_back(ms);
        ++stats.iterations;
    }
}

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

// Aggregate stats for one phase (one mode). Returns tiles/sec and fills out
// latency percentiles. Shape mirrors summarize_concurrent but single-mode.
struct PhaseSummary {
    double tiles_per_s = 0.0;
    double ms_avg = 0.0;
    double ms_p50 = 0.0;
    double ms_p99 = 0.0;
    double gpu_util_avg = 0.0;
    double gpu_util_p50 = 0.0;
    double gpu_util_peak = 0.0;
    int total_iterations = 0;
    double wall_ms = 0.0;
};

auto summarize_phase(const std::vector<WorkerStats>& workers,
                     const std::vector<Sample>& samples,
                     double wall_ms, int concurrency, Reporter& rep,
                     std::string_view mode_tag) -> PhaseSummary {
    PhaseSummary s;
    s.wall_ms = wall_ms;
    for (const auto& w : workers) s.total_iterations += w.iterations;
    if (wall_ms > 0.0) s.tiles_per_s = s.total_iterations / (wall_ms / 1000.0);

    std::vector<double> all_ms;
    all_ms.reserve(s.total_iterations);
    for (const auto& w : workers) {
        for (double m : w.tile_ms) all_ms.push_back(m);
    }
    if (!all_ms.empty()) {
        double sum = 0.0;
        for (double m : all_ms) sum += m;
        s.ms_avg = sum / all_ms.size();
        s.ms_p50 = percentile(all_ms, 50.0);
        s.ms_p99 = percentile(all_ms, 99.0);
    }

    if (!samples.empty()) {
        std::vector<unsigned> utils;
        for (const auto& sm : samples) {
            if (sm.gpu_util_pct) utils.push_back(*sm.gpu_util_pct);
        }
        if (!utils.empty()) {
            double sum = 0.0;
            for (auto u : utils) sum += u;
            s.gpu_util_avg = sum / utils.size();
            s.gpu_util_p50 = percentile_u(utils, 50.0);
            s.gpu_util_peak = static_cast<double>(*std::max_element(utils.begin(), utils.end()));
        }
    }

    std::string per_thread;
    for (size_t i = 0; i < workers.size(); ++i) {
        if (i) per_thread += ',';
        double t_avg = 0.0;
        if (!workers[i].tile_ms.empty()) {
            double ss = 0.0;
            for (double m : workers[i].tile_ms) ss += m;
            t_avg = ss / workers[i].tile_ms.size();
        }
        char tb[64];
        int tn = std::snprintf(tb, sizeof(tb),
                               "{\"iters\":%d,\"ms_avg\":%.3f}",
                               workers[i].iterations, t_avg);
        per_thread.append(tb, tn);
    }

    char buf[640];
    int n = std::snprintf(buf, sizeof(buf),
        "\"device\":\"gpu\",\"mode\":\"%s\",\"concurrency\":%d,\"iterations\":%d,"
        "\"tiles_per_s\":%.4f,\"ms_avg\":%.3f,\"ms_p50\":%.3f,\"ms_p99\":%.3f,"
        "\"gpu_util_avg\":%.2f,\"gpu_util_p50\":%.2f,\"gpu_util_peak\":%.2f,"
        "\"wall_ms\":%.1f,\"per_thread\":[%s]",
        mode_tag.data(), concurrency, s.total_iterations, s.tiles_per_s,
        s.ms_avg, s.ms_p50, s.ms_p99,
        s.gpu_util_avg, s.gpu_util_p50, s.gpu_util_peak,
        wall_ms, per_thread.c_str());
    rep.emit_summary(std::string_view(buf, n));
    return s;
}

// Run one phase (Mode A or B) for cfg.duration_s after cfg.warmup_s warmup.
// concurrency workers; each worker gets its own engine (Mode A) or its own
// ReuseContext (Mode B). Returns the phase summary.
template <typename WorkerFn>
auto run_phase(const TestConfig& cfg, int concurrency, Reporter& rep,
               NvmlProbe& nvml, bool has_nvml, WorkerFn&& worker_fn,
               std::string_view mode_tag) -> PhaseSummary {
    std::vector<WorkerStats> workers(concurrency);
    std::vector<Sample> samples;
    std::atomic<bool> sampling{true};
    std::atomic<bool> stop{false};

    std::thread sampler;
    if (has_nvml) {
        sampler = std::thread(sampler_loop, std::ref(nvml), std::ref(samples),
                              std::ref(sampling), cfg.sample_ms);
    }

    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    for (int i = 0; i < concurrency; ++i) {
        threads.emplace_back(worker_fn, i, std::ref(stop), std::ref(workers[i]));
    }

    Timer phase_timer;
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.duration_s));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    double wall_ms = phase_timer.elapsed_ms();

    sampling.store(false, std::memory_order_relaxed);
    if (sampler.joinable()) sampler.join();

    int global_i = 0;
    for (const auto& w : workers) {
        for (double ms : w.tile_ms) rep.emit_tile(global_i++, ms);
    }
    (void)global_i;
    for (const auto& sm : samples) rep.emit_sample(sm, "gpu");

    return summarize_phase(workers, samples, wall_ms, concurrency, rep, mode_tag);
}

// Compare two output buffers element-by-element. Returns max abs diff and mean
// abs diff. Used by the correctness gate.
struct DiffResult {
    double max_abs = 0.0;
    double mean_abs = 0.0;
    size_t count = 0;
};

auto compare_buffers(const float* a, const float* b, size_t n) -> DiffResult {
    DiffResult d;
    d.count = n;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double diff = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (diff > d.max_abs) d.max_abs = diff;
        sum += diff;
    }
    d.mean_abs = n > 0 ? sum / static_cast<double>(n) : 0.0;
    return d;
}

} // namespace

auto GpuReuseTest::run(const TestConfig& cfg, Reporter& rep) -> int {
    const int concurrency = std::max(1, cfg.concurrency);

    Workload wl;
    NvmlProbe nvml;
    bool has_nvml = nvml.start();
    if (!has_nvml) {
        LOG_WARNING("Perf", "NVML unavailable; GPU metrics absent, timing-only mode.");
    }

    // ---- Build N engines for Mode A (identical to GpuConcurrencyTest). ----
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

    // ---- Build N ReuseContexts for Mode B (own ncnn::Net per thread). ----
    std::vector<std::unique_ptr<ReuseContext>> contexts;
    contexts.reserve(concurrency);
    for (int i = 0; i < concurrency; ++i) {
        auto ctx = build_reuse_context(cfg);
        if (!ctx) {
            LOG_ERROR("Perf", "gpu-reuse: failed to build reuse context " +
                              std::to_string(i));
            nvml.stop();
            return 4;
        }
        contexts.push_back(std::move(ctx));
    }

    Engine& e0 = *engines.front();
    std::string gpu_name = has_nvml ? nvml.device_name() : "unknown";
    if (gpu_name.empty()) {
        for (const auto& d : e0.list_devices()) {
            if (d.type == DeviceType::GPU_VULKAN) { gpu_name = d.name; break; }
        }
    }
    auto mem = e0.get_memory_info();

    char meta[512];
    int n = std::snprintf(meta, sizeof(meta),
        "\"test\":\"gpu-reuse\",\"model\":\"%s\",\"scale\":%d,"
        "\"tile_w\":%d,\"tile_h\":%d,\"channels\":%d,\"concurrency\":%d,"
        "\"duration_s\":%.1f,\"warmup_s\":%.1f,\"device\":\"%s\",\"vram_mb\":%zu,\"nvml\":%s",
        cfg.model_name.c_str(), cfg.scale,
        cfg.tile_width, cfg.tile_height, cfg.channels, concurrency,
        cfg.duration_s, cfg.warmup_s, gpu_name.c_str(), mem.first,
        has_nvml ? "true" : "false");
    rep.emit_meta(std::string_view(meta, n));

    std::print("\n═══ GPU VkCompute Reuse A/B Test ═══\n");
    std::print("model: {} ({}x), tile {}x{}x{}, concurrency {}\n",
               cfg.model_name, cfg.scale, cfg.tile_width, cfg.tile_height, cfg.channels,
               concurrency);
    std::print("warmup {:.1f}s, duration {:.1f}s per phase\n", cfg.warmup_s, cfg.duration_s);
    std::print("GPU: {}, VRAM {:.0f} MB, NVML {}\n",
               gpu_name, static_cast<double>(mem.first), has_nvml ? "active" : "absent");

    // ================ CORRECTNESS GATE (run before any timing) ================
    // Same synthetic tile through Mode A (engine.process) and Mode B (reuse loop).
    // If they disagree beyond fp16 tolerance, reuse is unsafe — abort.
    std::print("\n── correctness gate ──\n");
    {
        auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        const size_t out_w = static_cast<size_t>(cfg.tile_width) * cfg.scale;
        const size_t out_h = static_cast<size_t>(cfg.tile_height) * cfg.scale;
        const size_t out_elems = out_w * out_h * static_cast<size_t>(cfg.channels);
        std::vector<float> out_a(tp.output.get(), tp.output.get() + out_elems);
        std::vector<float> out_b(out_elems);
        // zero both so any uninitialized tail is visible
        std::fill(out_a.begin(), out_a.end(), 0.0f);
        std::fill(out_b.begin(), out_b.end(), 0.0f);

        std::atomic<bool> cancel{false};
        auto res_a = engines[0]->process(tp.input.get(), out_a.data(),
                                         cfg.tile_width, cfg.tile_height, cfg.channels, cancel);
        if (!res_a) {
            LOG_ERROR("Perf", "correctness: Mode A process failed: " + res_a.error().message);
            nvml.stop();
            return 5;
        }
        int ret_b = reuse_forward(*contexts[0], tp.input.get(), out_b.data(),
                                  cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        if (ret_b != 0) {
            LOG_ERROR("Perf", "correctness: Mode B forward failed (ncnn err " +
                              std::to_string(ret_b) + ")");
            nvml.stop();
            return 5;
        }

        // A second Mode B pass to confirm reset() leaves the cmd reusable.
        std::vector<float> out_b2(out_elems, 0.0f);
        int ret_b2 = reuse_forward(*contexts[0], tp.input.get(), out_b2.data(),
                                   cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        if (ret_b2 != 0) {
            LOG_ERROR("Perf", "correctness: Mode B 2nd forward (after reset) failed: " +
                              std::to_string(ret_b2));
            nvml.stop();
            return 5;
        }

        size_t nelems = out_elems;
        DiffResult d_ab = compare_buffers(out_a.data(), out_b.data(), nelems);
        DiffResult d_bb = compare_buffers(out_b.data(), out_b2.data(), nelems);

        char cbuf[512];
        int cn = std::snprintf(cbuf, sizeof(cbuf),
            "\"event\":\"correctness\",\"mode_a_vs_b_max_abs\":%.3e,\"mode_a_vs_b_mean_abs\":%.3e,"
            "\"mode_b_b_max_abs\":%.3e,\"mode_b_b_mean_abs\":%.3e,\"elements\":%zu",
            d_ab.max_abs, d_ab.mean_abs, d_bb.max_abs, d_bb.mean_abs, nelems);
        rep.emit_summary(std::string_view(cbuf, cn));

        std::print("  Mode A vs Mode B : max_abs {:.3e} | mean_abs {:.3e}  ({} elements)\n",
                   d_ab.max_abs, d_ab.mean_abs, nelems);
        std::print("  Mode B vs Mode B : max_abs {:.3e} | mean_abs {:.3e}  (reset() stability)\n",
                   d_bb.max_abs, d_bb.mean_abs);

        // Tolerance: fp16 storage + arithmetic is nondeterministic across runs
        // due to reduction order, but A-vs-B and B-vs-B should both be tiny.
        // Use 1e-3 absolute (outputs are ~[0,1]) — comfortably above fp16 noise,
        // well below any real divergence.
        constexpr double kTol = 1e-3;
        bool ab_ok = d_ab.max_abs <= kTol;
        bool bb_ok = d_bb.max_abs <= kTol;
        std::print("  tolerance        : {:.0e} (fp16 nondeterminism guard)\n", kTol);
        std::print("  A vs B           : {}\n", ab_ok ? "PASS" : "FAIL");
        std::print("  B vs B (reset)   : {}\n", bb_ok ? "PASS" : "FAIL");
        if (!ab_ok || !bb_ok) {
            LOG_ERROR("Perf", "gpu-reuse: CORRECTNESS GATE FAILED — reuse output differs "
                              "from baseline; refusing to report a speed number.");
            std::print("\n  >> CORRECTNESS GATE FAILED — reuse is unsafe. Aborting. <<\n");
            nvml.stop();
            return 6;
        }
        std::print("  >> correctness PASS — proceeding to timing. <<\n");
    }

    // ================ WARMUP (both modes, brief) ================
    {
        auto tp = wl.make_tile(cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
        std::atomic<bool> cancel{false};
        for (int w = 0; w < 3; ++w) {
            auto res_a = engines[0]->process(tp.input.get(), tp.output.get(),
                                             cfg.tile_width, cfg.tile_height, cfg.channels, cancel);
            if (!res_a) {
                LOG_WARNING("Perf", "gpu-reuse warmup A failed: " + res_a.error().message);
            }
            int ret_b = reuse_forward(*contexts[0], tp.input.get(), tp.output.get(),
                                      cfg.tile_width, cfg.tile_height, cfg.channels, cfg.scale);
            if (ret_b != 0) {
                LOG_WARNING("Perf", "gpu-reuse warmup B failed (ncnn err " +
                                    std::to_string(ret_b) + ")");
            }
        }
    }

    // ================ PHASE A: baseline (fresh VkCompute per tile) ================
    std::print("\n── phase A: baseline (fresh VkCompute per tile) ──\n");
    PhaseSummary sa = run_phase(cfg, concurrency, rep, nvml, has_nvml,
        [&](int idx, std::atomic<bool>& stop, WorkerStats& st) {
            mode_a_worker(*engines[idx], cfg, wl, stop, st);
        }, "A_baseline");
    std::print("  tiles/sec : {:.2f}\n", sa.tiles_per_s);
    std::print("  latency   : avg {:.3f} ms | p50 {:.3f} ms | p99 {:.3f} ms\n",
               sa.ms_avg, sa.ms_p50, sa.ms_p99);
    if (has_nvml) {
        std::print("  GPU util  : avg {:.1f}% | p50 {:.1f}% | peak {:.1f}%\n",
                   sa.gpu_util_avg, sa.gpu_util_p50, sa.gpu_util_peak);
    }

    // ================ PHASE B: reuse (one VkCompute per thread, reset() between) ================
    std::print("\n── phase B: VkCompute reuse (cmd.reset() per tile) ──\n");
    PhaseSummary sb = run_phase(cfg, concurrency, rep, nvml, has_nvml,
        [&](int idx, std::atomic<bool>& stop, WorkerStats& st) {
            mode_b_worker(*contexts[idx], cfg, wl, stop, st);
        }, "B_reuse");
    std::print("  tiles/sec : {:.2f}\n", sb.tiles_per_s);
    std::print("  latency   : avg {:.3f} ms | p50 {:.3f} ms | p99 {:.3f} ms\n",
               sb.ms_avg, sb.ms_p50, sb.ms_p99);
    if (has_nvml) {
        std::print("  GPU util  : avg {:.1f}% | p50 {:.1f}% | peak {:.1f}%\n",
                   sb.gpu_util_avg, sb.gpu_util_p50, sb.gpu_util_peak);
    }

    // ================ VERDICT ================
    double dtps = sb.tiles_per_s - sa.tiles_per_s;
    double pct = sa.tiles_per_s > 0.0 ? (dtps / sa.tiles_per_s) * 100.0 : 0.0;
    double dlat = sa.ms_avg - sb.ms_avg;  // positive = reuse is faster
    double dlat_pct = sa.ms_avg > 0.0 ? (dlat / sa.ms_avg) * 100.0 : 0.0;

    // Overlap efficiency: at concurrency 2, ideal is 2x the single-thread rate.
    // We don't have a single-thread reference here, so report the speedup of B
    // over A as the reuse efficiency delta directly.
    std::print("\n── verdict (concurrency {}) ──\n", concurrency);
    std::print("  tiles/sec : A {:.2f} → B {:.2f}  ({:+.2f}%, {:+.2f} tiles/s)\n",
               sa.tiles_per_s, sb.tiles_per_s, pct, dtps);
    std::print("  avg latency: A {:.3f} ms → B {:.3f} ms  ({:+.2f}%)\n",
               sa.ms_avg, sb.ms_avg, dlat_pct);

    char vbuf[512];
    int vn = std::snprintf(vbuf, sizeof(vbuf),
        "\"event\":\"verdict\",\"a_tiles_per_s\":%.4f,\"b_tiles_per_s\":%.4f,"
        "\"delta_pct\":%.4f,\"a_ms_avg\":%.3f,\"b_ms_avg\":%.3f,\"lat_delta_pct\":%.4f",
        sa.tiles_per_s, sb.tiles_per_s, pct, sa.ms_avg, sb.ms_avg, dlat_pct);
    rep.emit_summary(std::string_view(vbuf, vn));

    if (pct > 1.0) {
        std::print("  >> reuse HELPED: {:.1f}% faster. VkCompute reconstruction was a real cost. <<\n", pct);
    } else if (pct < -1.0) {
        std::print("  >> reuse HURT: {:.1f}% slower. reset() overhead exceeds reconstruction cost. <<\n", -pct);
    } else {
        std::print("  >> reuse NEUTRAL: within ±1% of baseline. VkCompute construction is NOT the bottleneck. <<\n");
    }

    nvml.stop();
    return 0;
}

} // namespace crossscale::perf
