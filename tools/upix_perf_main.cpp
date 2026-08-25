// upix-perf — standalone GPU/CPU performance-probe CLI.
//
// Split out of the main upix-cli binary so the shipped CLI stays lean: the
// probes are developer/tuning tooling (GPU vs CPU throughput, engine
// concurrency scaling, VkCompute reuse A/B), not an end-user feature. Probe
// implementations live in share/perf/; this file owns only argument parsing
// and the model resolution shared by every probe.
//
// Unlike the old nested `upix perf <probe>` form, subcommands here are flat:
//
//   upix-perf list-probes
//   upix-perf gpu-efficiency  [options]
//   upix-perf gpu-concurrency [options]
//   upix-perf gpu-reuse       [options]
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "core/model_manager.hpp"
#include "perf/runner.hpp"
#include "utils/logger.hpp"

// Injected by the build (-DUPIX_VERSION=...); mirrors cli/xmake.lua's on_load.
// "dev" covers direct compiler invocations.
#ifndef UPIX_VERSION
#define UPIX_VERSION "dev"
#endif

// Set by a subcommand callback; read by main(). std::nullopt means "no
// subcommand ran" (e.g. bare --help).
std::optional<int> g_exit_code;

namespace {

// One instance per subcommand so each carries its own defaults (same shape as
// the removed cli/src args::Perf). duration/warmup stay raw strings ("5s",
// "1.5") and are parsed to seconds in run_probe, matching the legacy
// "strip trailing s" rule.
struct PerfArgs {
    std::string subcommand;
    std::string model_name;
    std::string models_dir;
    std::string out_path;
    int tile_size = 256;
    int concurrency = 2;            // gpu-concurrency default; gpu-reuse overrides to 1
    std::string duration_s = "5s";  // gpu-efficiency default; others override
    std::string warmup_s = "1s";
    int sample_ms = 100;
    bool no_cpu_baseline = false;   // gpu-efficiency only
};

// Same resolution order as cli/src/common.cpp's default_models_dir():
// $UPIX_MODELS_DIR > $XDG_DATA_HOME/upix/models > ~/.local/share/upix/models.
// Duplicated rather than linked because common.cpp drags in QR/network/daemon
// code this tool does not need.
auto default_models_dir() -> std::string {
    if (const char* env = std::getenv("UPIX_MODELS_DIR"); env && *env) {
        return env;
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::format("{}/upix/models", xdg);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::format("{}/.local/share/upix/models", home);
    }
    return "./models";  // no HOME/XDG at all (degenerate): keep something usable
}

// Parse a "<N>s" / "<N>" duration string to seconds, matching the legacy
// "strip trailing s then stod" rule (moved verbatim from cli/src/cmd_perf.cpp).
auto parse_seconds(const std::string& s) -> double {
    std::string t = s;
    if (!t.empty() && t.back() == 's') t.pop_back();
    return std::stod(t);
}

// Resolve a model by name (or fall back to the first available). Returns a
// non-owning pointer into `models` or nullptr on failure (after logging).
auto resolve_model(std::vector<upix::ModelInfo>& models, const std::string& name)
    -> const upix::ModelInfo* {
    if (models.empty()) return nullptr;
    if (name.empty()) return &models[0];
    auto it = std::find_if(models.begin(), models.end(),
        [&](const auto& m) { return m.name == name; });
    if (it == models.end()) {
        LOG_ERROR("Perf", "model not found: " + name);
        for (const auto& m : models) LOG_INFO("Perf", "  available: " + m.name);
        return nullptr;
    }
    return &*it;
}

// Shared body for gpu-efficiency / gpu-concurrency / gpu-reuse: resolve model,
// build a TestConfig from the parsed args, run the named probe.
auto run_probe(const PerfArgs& a) -> int {
    upix::ModelManager mgr;
    mgr.set_models_directory(a.models_dir);
    auto scan_res = mgr.scan_models();
    if (!scan_res) {
        LOG_ERROR("Perf", "model scan failed: " + scan_res.error().message);
        return 1;
    }
    auto models = std::move(scan_res).value();
    if (models.empty()) {
        LOG_ERROR("Perf", "no models found in: " + a.models_dir);
        return 1;
    }
    const auto* sel = resolve_model(models, a.model_name);
    if (!sel) return 1;

    upix::perf::TestConfig cfg;
    cfg.model_param = sel->param_path.string();
    cfg.model_bin = sel->bin_path.string();
    cfg.model_name = sel->name;
    cfg.tile_width = a.tile_size;
    cfg.tile_height = a.tile_size;
    cfg.scale = sel->scale_factor;
    cfg.duration_s = parse_seconds(a.duration_s);
    cfg.warmup_s = parse_seconds(a.warmup_s);
    cfg.sample_ms = a.sample_ms;

    if (a.subcommand == "gpu-efficiency") {
        cfg.cpu_baseline = !a.no_cpu_baseline;
        // concurrency left at TestConfig default.
    } else {
        // gpu-concurrency / gpu-reuse are GPU-only by design.
        cfg.cpu_baseline = false;
        cfg.concurrency = a.concurrency;
    }

    return upix::perf::run_test(a.subcommand, cfg, a.out_path);
}

}  // namespace

// CLI11 schema. Flat tree: four top-level subcommands, no nesting.
auto build_app() -> std::unique_ptr<CLI::App> {
    auto app = std::make_unique<CLI::App>(
        "Upix performance probes - GPU/CPU benchmarking for tuning decisions",
        "upix-perf");

    app->set_version_flag("--version,-V", UPIX_VERSION);

    const auto tile_range = CLI::Range(16, 8192);

    // ── list-probes ──────────────────────────────────────────────────────────
    static PerfArgs list_args;
    app->add_subcommand("list-probes", "List available perf probes")
        ->callback([] { list_args.subcommand = "list-probes";
                        upix::perf::list_available();
                        g_exit_code = 0; });

    // Shared option set for the three measurement probes. Defaults differ per
    // probe exactly as they did under `upix perf`: gpu-efficiency runs
    // dur=5s/warm=1s with a CPU baseline; gpu-concurrency defaults to 2
    // engines, dur=6s/warm=2s; gpu-reuse defaults to 1 engine, dur=6s/warm=2s.
    auto add_probe_options = [&](CLI::App* s, PerfArgs& a) {
        s->add_option("--model", a.model_name, "Model name (default: first available)");
        s->add_option("--models-dir", a.models_dir, "Models directory")->capture_default_str();
        s->add_option("--tile", a.tile_size, "Tile size")->check(tile_range)->capture_default_str();
        s->add_option("--duration", a.duration_s, "Duration (e.g. 6s)")->capture_default_str();
        s->add_option("--warmup", a.warmup_s, "Warmup (e.g. 2s)")->capture_default_str();
        s->add_option("--sample-ms", a.sample_ms, "Sample interval ms")
            ->check(CLI::Range(10, 10000))->capture_default_str();
        s->add_option("--out", a.out_path, "Output file (default: stdout)");
    };

    // ── gpu-efficiency ───────────────────────────────────────────────────────
    static PerfArgs eff_args;
    eff_args.models_dir = default_models_dir();
    auto* eff = app->add_subcommand("gpu-efficiency", "Measure GPU vs CPU tile throughput");
    add_probe_options(eff, eff_args);
    eff->add_flag("--no-cpu-baseline", eff_args.no_cpu_baseline, "Skip CPU baseline");
    eff->callback([] { eff_args.subcommand = "gpu-efficiency";
                       g_exit_code = run_probe(eff_args); });

    // ── gpu-concurrency ──────────────────────────────────────────────────────
    static PerfArgs conc_args;
    conc_args.models_dir = default_models_dir();
    conc_args.concurrency = 2; conc_args.duration_s = "6s"; conc_args.warmup_s = "2s";
    auto* conc = app->add_subcommand("gpu-concurrency", "Measure scaling across N concurrent engines");
    add_probe_options(conc, conc_args);
    conc->add_option("--concurrency", conc_args.concurrency, "Number of concurrent engines")
        ->check(CLI::Range(1, 16))->capture_default_str();
    conc->callback([] { conc_args.subcommand = "gpu-concurrency";
                        g_exit_code = run_probe(conc_args); });

    // ── gpu-reuse ────────────────────────────────────────────────────────────
    static PerfArgs reuse_args;
    reuse_args.models_dir = default_models_dir();
    reuse_args.concurrency = 1; reuse_args.duration_s = "6s"; reuse_args.warmup_s = "2s";
    auto* reuse = app->add_subcommand("gpu-reuse", "A/B fresh vs reused VkCompute per tile");
    add_probe_options(reuse, reuse_args);
    reuse->add_option("--concurrency", reuse_args.concurrency, "Number of concurrent engines")
        ->check(CLI::Range(1, 16))->capture_default_str();
    reuse->callback([] { reuse_args.subcommand = "gpu-reuse";
                        g_exit_code = run_probe(reuse_args); });

    return app;
}

auto main(int argc, char* argv[]) -> int {
    auto app = build_app();

    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // app->exit() centralizes: help → stdout, version → stdout, errors → stderr.
        return app->exit(e, std::cout, std::cerr);
    }

    // A subcommand ran → honor its exit code; otherwise show top-level help.
    if (g_exit_code.has_value()) {
        return g_exit_code.value();
    }
    std::print("{}", app->help());
    return 0;
}
