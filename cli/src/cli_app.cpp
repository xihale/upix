// CLI11 schema. Wires every subcommand's options, validators, and callbacks.
//
// Callbacks are std::function<void()> in CLI11, so the cmd_* exit code is
// stashed in g_exit_code and main() reads it after CLI11_PARSE. CLI11 itself
// returns 0/non-zero from parse() for usage errors; g_exit_code covers the
// subcommand-business-logic exit code on successful parse.
#include "cli_app.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <optional>
#include <string>

#include "args.hpp"
#include "common.hpp"  // default_models_dir

// Injected by the build (-DCROSSSCALE_VERSION=...); xmake.lua's set_version()
// is the single source of truth. "dev" covers direct compiler invocations.
#ifndef CROSSSCALE_VERSION
#define CROSSSCALE_VERSION "dev"
#endif

// Set by a subcommand callback; read by main(). std::nullopt means "no
// subcommand ran" (e.g. bare --help, or no verb given).
std::optional<int> g_exit_code;

auto build_app() -> std::unique_ptr<CLI::App> {
    auto app = std::make_unique<CLI::App>(
        "CrossScale - Distributed Image Super-Resolution", "crossscale");

    app->set_version_flag("--version,-V", CROSSSCALE_VERSION);

    // Reusable validators.
    auto tile_range = CLI::Range(16, 8192);

    // ── upscale (implicit root verb) ─────────────────────────────────────────
    // Bare `crossscale img.png [out.png]` is the upscale path; a first token
    // matching a subcommand name still wins, so only a file literally named
    // "model"/"daemon"/... is shadowed. The old explicit `process` subcommand
    // is gone — its options live on the root, so `crossscale in.png -m x
    // --tile-size 256` works anywhere.
    static args::Process proc_args;
    proc_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    // Each option mixes a positional name with flags, so `crossscale img.png`
    // and `crossscale -i img.png` are interchangeable. Output is optional —
    // omitted, it derives `img_<model>.png` (or `dir_<model>/` in batch mode).
    auto* input_opt = app->add_option("input,-i,--input", proc_args.input_path,
                                      "Input image or directory")
                           ->expected(0, 1);
    app->add_option("output,-o,--output", proc_args.output_path,
                    "Output path (default: derived from input + model)")
        ->expected(0, 1);
    app->add_option("-m,--model", proc_args.model_name, "Model name (default: first available)");
    app->add_option("--models", proc_args.models_dir, "Models directory")
        ->capture_default_str();
    auto* tile_size_opt = app->add_option("--tile-size", proc_args.tile_size, "Tile size")
        ->check(tile_range)->capture_default_str();
    app->add_option("--tile-overlap", proc_args.tile_overlap, "Tile overlap")
        ->check(CLI::Range(0, 1024))->capture_default_str();
    app->add_option("--batch-size", proc_args.batch_size, "Batch size")
        ->check(CLI::Range(1, 64))->capture_default_str();
    app->add_flag("--auto-tile", proc_args.auto_tile, "Auto-select tile size from GPU VRAM");
    app->add_option("--min-tile", proc_args.min_tile, "Minimum auto-tile size")
        ->check(tile_range)->capture_default_str();
    app->add_option("--max-tile", proc_args.max_tile, "Maximum auto-tile size")
        ->check(tile_range)->capture_default_str();
    auto* jobs_opt = app->add_option("-j,--jobs", proc_args.jobs,
                                     "Parallel workers for directory mode")
        ->check(CLI::Range(1, 1024))->capture_default_str();
    app->add_flag("--gpu", proc_args.use_gpu, "Force GPU for directory mode");
    app->add_flag("--no-daemon", proc_args.no_daemon,
                  "Never use the warm-engine daemon (process inline)");
    app->add_option("--output-quality", proc_args.output_quality,
                    "Output quality 0-100 (default: 95)")
        ->check(CLI::Range(0, 100));
    app->add_option("--output-format", proc_args.output_format,
                    "Output format (default: auto from extension)")
        ->check(CLI::IsMember({"png", "jpg", "jpeg", "webp", "avif"}));
    app->add_option("--config", proc_args.config_file, "Config file path");
    app->callback([input_opt, tile_size_opt, jobs_opt] {
        proc_args.tile_size_explicit = tile_size_opt->count() > 0;
        proc_args.jobs_explicit = jobs_opt->count() > 0;
        if (input_opt->count() > 0) {
            g_exit_code = cmd_process(proc_args);
        }
    });

    // ── model ───────────────────────────────────────────────────────────────
    // One Model instance per nested subcommand (like perf) so each carries its
    // own models_dir/repo. `repo` is bound on the subcommands that hit the
    // network (list-remote/download); list/import are purely local.
    auto* model = app->add_subcommand("model", "Manage models (list, download, import)");

    static args::Model model_list_args;
    model_list_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    auto* mlist = model->add_subcommand("list", "List installed models");
    mlist->add_option("--models-dir", model_list_args.models_dir,
                      "Models directory (default: ./models)");
    mlist->add_flag("--names-only", model_list_args.names_only,
                    "Print model names only, one per line (for scripts/completions)");
    mlist->callback([] { model_list_args.subcommand = "list";
                         g_exit_code = cmd_model(model_list_args); });

    static args::Model model_remote_args;
    model_remote_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    auto* mremote = model->add_subcommand("list-remote", "List models in the GitHub release");
    mremote->add_option("--repo", model_remote_args.repo,
                        "GitHub repo owner/name (default: upstream release)");
    mremote->callback([] { model_remote_args.subcommand = "list-remote";
                           g_exit_code = cmd_model(model_remote_args); });

    static args::Model model_dl_args;
    model_dl_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    auto* mdl = model->add_subcommand("download", "Download model(s) from the release");
    mdl->add_option("names", model_dl_args.names, "Model name(s), see list-remote")
        ->required();
    mdl->add_option("--models-dir", model_dl_args.models_dir,
                    "Models directory (default: ./models)");
    mdl->add_option("--repo", model_dl_args.repo,
                    "GitHub repo owner/name (default: upstream release)");
    mdl->callback([] { model_dl_args.subcommand = "download";
                       g_exit_code = cmd_model(model_dl_args); });

    static args::Model model_imp_args;
    model_imp_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    auto* mimp = model->add_subcommand("import", "Import local .param/.bin files");
    mimp->add_option("files", model_imp_args.files, "Local .param/.bin file path(s)")
        ->required();
    mimp->add_option("--models-dir", model_imp_args.models_dir,
                     "Models directory (default: ./models)");
    mimp->callback([] { model_imp_args.subcommand = "import";
                        g_exit_code = cmd_model(model_imp_args); });

    // ── perf ────────────────────────────────────────────────────────────────
    // One Perf instance per nested subcommand so each carries its own defaults
    // (gpu-efficiency: dur=5s/warm=1s/no-concurrency; gpu-concurrency:
    // conc=2/dur=6s/warm=2s; gpu-reuse: conc=1/dur=6s/warm=2s).
    auto* perf = app->add_subcommand("perf", "GPU/CPU performance probes");

    static args::Perf perf_list_args;
    perf_list_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    perf->add_subcommand("list-probes", "List available perf probes")
        ->callback([] { perf_list_args.subcommand = "list-probes";
                        g_exit_code = cmd_perf(perf_list_args); });

    static args::Perf eff_args;
    eff_args.models_dir = default_models_dir();  // env-aware (common.cpp)
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
    conc_args.models_dir = default_models_dir();  // env-aware (common.cpp)
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
    reuse_args.models_dir = default_models_dir();  // env-aware (common.cpp)
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

    // ── host ──────────────────────────────────────────────────────────────────
    // The coordination server phones pair with (TCP + QR invite). Distinct from
    // `daemon`, which is the local warm-engine process `process` delegates to.
    static args::Host host_args;
    host_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    auto* host = app->add_subcommand("host", "Start or query the coordinator for phones");
    auto add_host_start_flags = [&](CLI::App* s) {
        s->add_option("-p,--port", host_args.port, "Port")->capture_default_str();
        s->add_option("--models-dir", host_args.models_dir, "Models directory")
            ->capture_default_str();
        s->add_flag("--auto-approve", host_args.auto_approve,
                    "Auto-approve all pairing requests without prompting (for automated testing/CI)");
    };
    auto* hstart = host->add_subcommand("start", "Start the coordinator (accepts one phone, upscales submitted images)");
    add_host_start_flags(hstart);
    hstart->callback([] { host_args.subcommand = "start";
                          g_exit_code = cmd_host(host_args); });
    auto* hinfo = host->add_subcommand("info", "Print the connection card (IP/QR/key)");
    hinfo->add_option("-p,--port", host_args.port, "Port")->capture_default_str();
    hinfo->callback([] { host_args.subcommand = "info";
                         g_exit_code = cmd_host(host_args); });

    // Bare `host` (no nested subcommand) → default to start (same as daemon).
    add_host_start_flags(host);
    host->callback([] {
        if (host_args.subcommand.empty()) {
            host_args.subcommand = "start";
            g_exit_code = cmd_host(host_args);
        }
    });

    // ── daemon ────────────────────────────────────────────────────────────────
    // Flags are shared by start/stop. We register them on each nested
    // subcommand (binding the same field twice is fine) so `daemon start --cpu`
    // and `daemon stop --socket X` both parse correctly. A bare `daemon` with no
    // nested subcommand defaults to `start` (matches the legacy behavior).
    static args::Daemon daemon_args;
    daemon_args.models_dir = default_models_dir();  // env-aware (common.cpp)
    auto* daemon = app->add_subcommand("daemon", "Manage the warm-engine daemon");

    auto add_daemon_flags = [&](CLI::App* s) {
        s->add_option("--model", daemon_args.model_name, "Model name");
        s->add_option("--models-dir", daemon_args.models_dir, "Models directory")
            ->capture_default_str();
        s->add_option("--socket", daemon_args.socket_path, "Unix socket path");
        s->add_flag("--cpu", daemon_args.force_cpu, "Force CPU");
        s->add_flag("--fg", daemon_args.foreground, "Run in foreground");
        s->add_option("--idle-timeout", daemon_args.idle_timeout_s,
                      "Exit after N idle seconds (0 = never)")
            ->check(CLI::Range(0, 86400))->capture_default_str();
    };

    auto* daemon_start = daemon->add_subcommand("start", "Start the daemon (default)");
    add_daemon_flags(daemon_start);
    daemon_start->callback([] { daemon_args.subcommand = "start";
                                g_exit_code = cmd_daemon(daemon_args); });

    auto* daemon_stop = daemon->add_subcommand("stop", "Stop the daemon");
    daemon_stop->add_option("--socket", daemon_args.socket_path, "Unix socket path");
    daemon_stop->callback([] { daemon_args.subcommand = "stop";
                               g_exit_code = cmd_daemon(daemon_args); });

    auto* daemon_status = daemon->add_subcommand("status", "Report whether the daemon is running");
    daemon_status->add_option("--socket", daemon_args.socket_path, "Unix socket path");
    daemon_status->callback([] { daemon_args.subcommand = "status";
                                 g_exit_code = cmd_daemon(daemon_args); });

    // Bare `daemon` (no nested subcommand) → default to start. Also accept the
    // shared flags directly on `daemon` so `daemon --cpu --fg` works without a verb.
    add_daemon_flags(daemon);
    daemon->callback([] {
        if (daemon_args.subcommand.empty()) {
            daemon_args.subcommand = "start";
            g_exit_code = cmd_daemon(daemon_args);
        }
    });

    // ── completions ──────────────────────────────────────────────────────────
    static args::Completions completions_args;
    auto* comp = app->add_subcommand(
        "completions", "Print or install a shell completion script (bash/zsh/fish)");
    comp->add_option("shell", completions_args.shell, "Target shell")
        ->required()
        ->check(CLI::IsMember({"bash", "zsh", "fish"}));
    comp->add_flag("-i,--install", completions_args.install,
                   "Write the script into this shell's user completion directory "
                   "instead of printing it");
    comp->callback([] { g_exit_code = cmd_completions(completions_args); });

    return app;
}
