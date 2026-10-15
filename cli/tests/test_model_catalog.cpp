// Catalog conformance: every model in the local store must have a scale label
// (the trailing xN in its name) that agrees with what the param graph alone
// computes, and no model may fall back to the silent 2x default. This pins
// the convention that spans scripts/publish_models.py (conversion time),
// ModelManager (scan) and the engine's no-upsampler fallback (load) — the
// three historically drifted by hand. Skips cleanly when no models are
// installed (CI has none); run it on a machine with the catalog downloaded.

#include "core/model_graph.hpp"
#include "core/model_manager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

auto models_dir_from_env() -> fs::path {
    if (const char* env = std::getenv("UPIX_MODELS_DIR"); env && *env) {
        return fs::path(env);
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "upix" / "models";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".local" / "share" / "upix" / "models";
    }
    return {};
}

int check_filename_scale(const std::string& name, int expected) {
    const int got = upix::scale_from_filename(name);
    if (got != expected) {
        std::cerr << "scale_from_filename(\"" << name << "\") = " << got
                  << ", expected " << expected << "\n";
        return 1;
    }
    return 0;
}

// A param file whose upsampler arithmetic lands outside the supported range
// must report "not detected" (the engine then falls back to the filename
// convention) instead of exporting a scale that overflows every caller's
// width*scale arithmetic. Runs without any installed model.
int check_hostile_scale_rejected() {
    std::error_code tmp_ec;
    const fs::path tmp_dir = fs::temp_directory_path(tmp_ec);
    if (tmp_ec) {
        std::cerr << "no temporary directory for the hostile-param check: "
                  << tmp_ec.message() << "\n";
        return 1;
    }
    const fs::path param = tmp_dir / "upix-hostile-scale.param";
    {
        std::ofstream out(param);
        if (!out) {
            std::cerr << "cannot write " << param << "\n";
            return 1;
        }
        out << "7767517\n2 2\n"
               "Input in 0 1 in0 0=4 1=4 2=3\n"
               "Deconvolution deconv 1 1 in0 out0 0=3 3=2000000000\n";
    }
    auto graph = upix::detect_model_graph(param.string());
    std::error_code rm_ec;
    fs::remove(param, rm_ec);
    if (!graph) {
        std::cerr << "detect_model_graph on the hostile param returned nullopt\n";
        return 1;
    }
    if (graph->scale_detected) {
        std::cerr << "hostile param reported scale x" << graph->scale_factor
                  << " (expected: not detected)\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    using upix::detect_model_graph;
    using upix::scale_from_filename;

    // The convention itself, including the names that historically broke
    // hand-rolled parsers: digits embedded before the suffix ("x4plus"),
    // 1x restoration nets, uppercase X, no suffix at all.
    int failures = 0;
    failures += check_filename_scale("Real-ESRGANv3-anime-x2", 2);
    failures += check_filename_scale("Real-ESRGAN-x4plus", 4);
    failures += check_filename_scale("1xDeJPG-RealPLKSR-x1", 1);
    failures += check_filename_scale("NES-Composite-RGB-Compact-x1", 1);
    failures += check_filename_scale("x2", 2);
    failures += check_filename_scale("NomosUni-SPAN-X4", 4);
    failures += check_filename_scale("Nomos8kSC", 0);
    failures += check_filename_scale("x9", 0);
    failures += check_filename_scale("x0", 0);
    failures += check_filename_scale("", 0);
    // Digit runs long enough to overflow a naive accumulator must be rejected,
    // not wrapped into a plausible-looking scale.
    failures += check_filename_scale("x99999999999", 0);
    failures += check_filename_scale("7x2147483647", 0);
    failures += check_hostile_scale_rejected();

    const fs::path dir = models_dir_from_env();
    if (dir.empty() || !fs::exists(dir)) {
        std::cout << "ok " << 13 - failures
                  << " convention checks (no models dir, catalog scan skipped)\n";
        return failures == 0 ? 0 : 1;
    }

    upix::ModelManager manager(dir);
    auto scanned = manager.scan_models();
    if (!scanned) {
        std::cerr << "scan_models failed: " << scanned.error().message << "\n";
        return 1;
    }
    if (scanned->empty()) {
        std::cout << "ok " << 13 - failures
                  << " convention checks (no models installed, scan skipped)\n";
        return failures == 0 ? 0 : 1;
    }

    size_t graph_agree = 0, name_only = 0;
    for (const auto& info : *scanned) {
        auto graph = detect_model_graph(info.param_path.string());
        if (!graph) {
            std::cerr << "FAIL " << info.name << ": param unreadable: "
                      << info.param_path.string() << "\n";
            failures++;
            continue;
        }
        if (!fs::exists(info.bin_path)) {
            std::cerr << "FAIL " << info.name << ": missing .bin: "
                      << info.bin_path.string() << "\n";
            failures++;
            continue;
        }
        if (graph->scale_detected) {
            // Graph truth vs the name label — a mismatch means one of the
            // three surfaces drifted (or the label lies about the net).
            if (graph->scale_factor != info.scale_factor) {
                std::cerr << "FAIL " << info.name << ": graph says x"
                          << graph->scale_factor << ", name says x"
                          << info.scale_factor << "\n";
                failures++;
            } else {
                graph_agree++;
            }
        } else {
            // Undetectable graph: the name MUST carry an explicit xN, else
            // both engine and manager silently ship the 2x default.
            if (scale_from_filename(info.name) < 1) {
                std::cerr << "FAIL " << info.name
                          << ": graph undetectable and name encodes no xN"
                          << " (silent 2x default)\n";
                failures++;
            } else {
                name_only++;
            }
        }
    }

    if (failures != 0) {
        std::cerr << "catalog conformance FAILED: " << failures << " problem(s)\n";
        return 1;
    }
    std::cout << "ok convention checks; " << scanned->size() << " models: "
              << graph_agree << " graph-verified, " << name_only
              << " name-pinned (graph has no upsampler)\n";
    return 0;
}
