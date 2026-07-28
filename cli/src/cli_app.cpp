// CLI11 schema. Wires every subcommand's options, validators, and callbacks.
//
// Callbacks are std::function<void()> in CLI11, so the cmd_* exit code is
// stashed in g_exit_code and main() reads it after CLI11_PARSE. CLI11 itself
// returns 0/non-zero from parse() for usage errors; g_exit_code covers the
// subcommand-business-logic exit code on successful parse.
#include "cli_app.hpp"

#include <CLI/CLI.hpp>
#include <optional>
#include <string>

#include "args.hpp"

// Set by a subcommand callback; read by main(). std::nullopt means "no
// subcommand ran" (e.g. bare --help, or no verb given).
std::optional<int> g_exit_code;

auto build_app() -> std::unique_ptr<CLI::App> {
    auto app = std::make_unique<CLI::App>(
        "CrossScale - Distributed Image Super-Resolution", "crossscale");

    app->set_version_flag("--version,-V", "0.1.0");

    // Reusable validators.
    auto tile_range = CLI::Range(16, 8192);

    // ── process ─────────────────────────────────────────────────────────────
    static args::Process proc_args;
    auto* proc = app->add_subcommand("process", "Upscale an image or directory");
    proc->add_option("-i,--input", proc_args.input_path, "Input image or directory")
        ->required()->expected(1);
    proc->add_option("-o,--output", proc_args.output_path, "Output image or directory")
        ->required();
    proc->add_option("-m,--model", proc_args.model_name, "Model name (default: first available)");
    proc->add_option("--models", proc_args.models_dir, "Models directory")
        ->capture_default_str();
    auto* tile_size_opt = proc->add_option("--tile-size", proc_args.tile_size, "Tile size")
        ->check(tile_range)->capture_default_str();
    proc->add_option("--tile-overlap", proc_args.tile_overlap, "Tile overlap")
        ->check(CLI::Range(0, 1024))->capture_default_str();
    proc->add_option("--batch-size", proc_args.batch_size, "Batch size")
        ->check(CLI::Range(1, 64))->capture_default_str();
    proc->add_flag("--auto-tile", proc_args.auto_tile, "Auto-select tile size from GPU VRAM");
    proc->add_option("--min-tile", proc_args.min_tile, "Minimum auto-tile size")
        ->check(tile_range)->capture_default_str();
    proc->add_option("--max-tile", proc_args.max_tile, "Maximum auto-tile size")
        ->check(tile_range)->capture_default_str();
    proc->add_option("-j,--jobs", proc_args.jobs, "Parallel workers for directory mode")
        ->check(CLI::Range(1, 1024))->capture_default_str();
    proc->add_flag("--gpu", proc_args.use_gpu, "Force GPU for directory mode");
    proc->add_flag("--no-daemon", proc_args.no_daemon, "Skip serve-daemon delegation");
    proc->add_option("--output-quality", proc_args.output_quality,
                     "Output quality 0-100 (default: 95)")
        ->check(CLI::Range(0, 100));
    proc->add_option("--output-format", proc_args.output_format,
                     "Output format (default: auto from extension)")
        ->check(CLI::IsMember({"png", "jpg", "jpeg", "webp"}));
    proc->add_option("--config", proc_args.config_file, "Config file path");
    proc->callback([tile_size_opt] {
        proc_args.tile_size_explicit = tile_size_opt->count() > 0;
        g_exit_code = cmd_process(proc_args);
    });

    // ── model ───────────────────────────────────────────────────────────────
    static args::Model model_args;
    auto* model = app->add_subcommand("model", "List installed models");
    model->add_subcommand("list", "List available models")
        ->callback([] { g_exit_code = cmd_model(model_args); });

    // ── benchmark ───────────────────────────────────────────────────────────
    static args::Benchmark bench_args;
    app->add_subcommand("benchmark", "Run a CPU micro-benchmark")
        ->callback([] { g_exit_code = cmd_benchmark(bench_args); });

    // ── perf ────────────────────────────────────────────────────────────────
    // One Perf instance per nested subcommand so each carries its own defaults
    // (gpu-efficiency: dur=5s/warm=1s/no-concurrency; gpu-concurrency:
    // conc=2/dur=6s/warm=2s; gpu-reuse: conc=1/dur=6s/warm=2s).
    auto* perf = app->add_subcommand("perf", "GPU/CPU performance probes");

    static args::Perf perf_list_args;
    perf->add_subcommand("list-probes", "List available perf probes")
        ->callback([] { perf_list_args.subcommand = "list-probes";
                        g_exit_code = cmd_perf(perf_list_args); });

    static args::Perf eff_args;
    auto* eff = perf->add_subcommand("gpu-efficiency", "Measure GPU vs CPU tile throughput");
    eff->add_option("--model", eff_args.model_name, "Model name (default: first available)");
    eff->add_option("--models-dir", eff_args.models_dir, "Models directory")->capture_default_str();
    eff->add_option("--tile", eff_args.tile_size, "Tile size")->check(tile_range)->capture_default_str();
    eff->add_option("--duration", eff_args.duration_s, "Duration (e.g. 5s)")->capture_default_str();
    eff->add_option("--warmup", eff_args.warmup_s, "Warmup (e.g. 1s)")->capture_default_str();
    eff->add_option("--sample-ms", eff_args.sample_ms, "Sample interval ms")
        ->check(CLI::Range(10, 10000))->capture_default_str();
    eff->add_flag("--no-cpu-baseline", eff_args.no_cpu_baseline, "Skip CPU baseline");
    eff->add_option("--out", eff_args.out_path, "Output file (default: stdout)");
    eff->callback([] { eff_args.subcommand = "gpu-efficiency";
                       g_exit_code = cmd_perf(eff_args); });

    static args::Perf conc_args;
    conc_args.concurrency = 2; conc_args.duration_s = "6s"; conc_args.warmup_s = "2s";
    auto* conc = perf->add_subcommand("gpu-concurrency", "Measure scaling across N concurrent engines");
    conc->add_option("--model", conc_args.model_name, "Model name");
    conc->add_option("--models-dir", conc_args.models_dir, "Models directory")->capture_default_str();
    conc->add_option("--tile", conc_args.tile_size, "Tile size")->check(tile_range)->capture_default_str();
    conc->add_option("--concurrency", conc_args.concurrency, "Number of concurrent engines")
        ->check(CLI::Range(1, 16))->capture_default_str();
    conc->add_option("--duration", conc_args.duration_s, "Duration (e.g. 6s)")->capture_default_str();
    conc->add_option("--warmup", conc_args.warmup_s, "Warmup (e.g. 2s)")->capture_default_str();
    conc->add_option("--sample-ms", conc_args.sample_ms, "Sample interval ms")
        ->check(CLI::Range(10, 10000))->capture_default_str();
    conc->add_option("--out", conc_args.out_path, "Output file (default: stdout)");
    conc->callback([] { conc_args.subcommand = "gpu-concurrency";
                        g_exit_code = cmd_perf(conc_args); });

    static args::Perf reuse_args;
    reuse_args.concurrency = 1; reuse_args.duration_s = "6s"; reuse_args.warmup_s = "2s";
    auto* reuse = perf->add_subcommand("gpu-reuse", "A/B fresh vs reused VkCompute per tile");
    reuse->add_option("--model", reuse_args.model_name, "Model name");
    reuse->add_option("--models-dir", reuse_args.models_dir, "Models directory")->capture_default_str();
    reuse->add_option("--tile", reuse_args.tile_size, "Tile size")->check(tile_range)->capture_default_str();
    reuse->add_option("--concurrency", reuse_args.concurrency, "Number of concurrent engines")
        ->check(CLI::Range(1, 16))->capture_default_str();
    reuse->add_option("--duration", reuse_args.duration_s, "Duration (e.g. 6s)")->capture_default_str();
    reuse->add_option("--warmup", reuse_args.warmup_s, "Warmup (e.g. 2s)")->capture_default_str();
    reuse->add_option("--sample-ms", reuse_args.sample_ms, "Sample interval ms")
        ->check(CLI::Range(10, 10000))->capture_default_str();
    reuse->add_option("--out", reuse_args.out_path, "Output file (default: stdout)");
    reuse->callback([] { reuse_args.subcommand = "gpu-reuse";
                         g_exit_code = cmd_perf(reuse_args); });

    // ── server ──────────────────────────────────────────────────────────────
    static args::Server server_args;
    auto* server = app->add_subcommand("server", "Start or query the coordination server");
    auto* sstart = server->add_subcommand("start", "Start the coordinator + mDNS broadcast");
    sstart->add_option("-p", server_args.port, "Port")->capture_default_str();
    sstart->add_option("-k", server_args.key, "Auth key (default: auto-generated)");
    sstart->callback([] { g_exit_code = cmd_server(server_args); server_args.subcommand = "start"; });
    auto* sinfo = server->add_subcommand("info", "Print the connection card (IP/QR/key)");
    sinfo->add_option("-p", server_args.port, "Port")->capture_default_str();
    sinfo->add_option("-k", server_args.key, "Auth key")->required();
    sinfo->callback([] { g_exit_code = cmd_server(server_args); server_args.subcommand = "info"; });

    // ── worker ──────────────────────────────────────────────────────────────
    static args::Worker worker_args;
    auto* worker = app->add_subcommand("worker", "Connect as a worker to a coordinator");
    auto* wconnect = worker->add_subcommand("connect", "Connect to <host>:<port>");
    wconnect->add_option("host_port", worker_args.host_port, "<host>:<port>")->required();
    wconnect->add_option("-k", worker_args.key, "Auth key")->required();
    wconnect->add_option("--recommended-weight", worker_args.recommended_weight, "Recommended weight");
    wconnect->add_option("--queue-depth", worker_args.queue_depth, "Queue depth (1-1024)")
        ->check(CLI::Range((size_t)1, (size_t)1024));
    wconnect->add_option("--recommended-tile-size", worker_args.recommended_tile_size,
                         "Recommended tile size (0-8192)")
        ->check(CLI::Range(0, 8192));
    wconnect->callback([] { g_exit_code = cmd_worker(worker_args); });

    // ── serve ───────────────────────────────────────────────────────────────
    // Flags are shared by start/stop. We register them on each nested
    // subcommand (binding the same field twice is fine) so `serve start --cpu`
    // and `serve stop --socket X` both parse correctly. A bare `serve` with no
    // nested subcommand defaults to `start` (matches the legacy behavior).
    static args::Serve serve_args;
    auto* serve = app->add_subcommand("serve", "Manage the warm-engine daemon");

    auto add_serve_flags = [&](CLI::App* s) {
        s->add_option("--model", serve_args.model_name, "Model name");
        s->add_option("--models-dir", serve_args.models_dir, "Models directory")
            ->capture_default_str();
        s->add_option("--socket", serve_args.socket_path, "Unix socket path");
        s->add_flag("--cpu", serve_args.force_cpu, "Force CPU");
        s->add_flag("--fg", serve_args.foreground, "Run in foreground");
    };

    auto* serve_start = serve->add_subcommand("start", "Start the daemon (default)");
    add_serve_flags(serve_start);
    serve_start->callback([] { serve_args.subcommand = "start";
                               g_exit_code = cmd_serve(serve_args); });

    auto* serve_stop = serve->add_subcommand("stop", "Stop the daemon");
    serve_stop->add_option("--socket", serve_args.socket_path, "Unix socket path");
    serve_stop->callback([] { serve_args.subcommand = "stop";
                              g_exit_code = cmd_serve(serve_args); });

    // Bare `serve` (no nested subcommand) → default to start. Also accept the
    // shared flags directly on `serve` so `serve --cpu --fg` works without a verb.
    add_serve_flags(serve);
    serve->callback([] {
        if (serve_args.subcommand.empty()) {
            serve_args.subcommand = "start";
            g_exit_code = cmd_serve(serve_args);
        }
    });

    // ── completion ──────────────────────────────────────────────────────────
    static args::Completion completion_args;
    auto* completion = app->add_subcommand("completion", "Print shell completion script");
    completion->add_option("shell", completion_args.shell, "bash / zsh / fish")
        ->required()->check(CLI::IsMember({"bash", "zsh", "fish"}));
    completion->callback([] { g_exit_code = cmd_completion(completion_args); });

    return app;
}

// Stub implementations — replaced one-by-one in cmd_*.cpp. Keeping them here
// (rather than in separate files yet) lets the skeleton link and lets us verify
// the command tree compiles and --help renders correctly before any real logic
// is wired. Each cmd_*.cpp will supply its own definition, shadowing these.
// All cmd_* implementations now live in their own cmd_*.cpp — no stubs left.
