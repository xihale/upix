// `crossscale perf {list-probes, gpu-efficiency, gpu-concurrency, gpu-reuse}`.
//
// Three probe subcommands share the same model-resolution + TestConfig-shape
// logic; only defaults and the cpu_baseline/concurrency fields differ. The
// duration/warmup strings ("5s", "1.5") are parsed to seconds here.
#include <algorithm>
#include <string>

#include "args.hpp"
#include "cli_app.hpp"
#include "utils/logger.hpp"
#include "core/model_manager.hpp"
#include "perf/runner.hpp"

namespace {

// Parse a "<N>s" / "<N>" duration string to seconds, matching the legacy
// "strip trailing s then stod" rule.
auto parse_seconds(const std::string& s) -> double {
    std::string t = s;
    if (!t.empty() && t.back() == 's') t.pop_back();
    return std::stod(t);
}

// Resolve a model by name (or fall back to the first available). Returns a
// non-owning pointer into `models` or nullptr on failure (after logging).
auto resolve_model(std::vector<crossscale::ModelInfo>& models, const std::string& name)
    -> const crossscale::ModelInfo* {
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
auto run_probe(const args::Perf& a) -> int {
    crossscale::ModelManager mgr;
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

    crossscale::perf::TestConfig cfg;
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

    return crossscale::perf::run_test(a.subcommand, cfg, a.out_path);
}

}  // namespace

int cmd_perf(const args::Perf& a) {
    if (a.subcommand == "list-probes") {
        crossscale::perf::list_available();
        return 0;
    }
    if (a.subcommand == "gpu-efficiency"
        || a.subcommand == "gpu-concurrency"
        || a.subcommand == "gpu-reuse") {
        return run_probe(a);
    }
    // Unreachable: cli_app.cpp only wires these four subcommands. Defensive.
    LOG_ERROR("Perf", "Unknown perf subcommand: " + a.subcommand);
    return 1;
}
