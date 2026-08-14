// POD parameter structs, one per subcommand.
//
// CLI11 options are bound to these fields by reference in cli_app.cpp. Keeping
// the data separate from the schema lets cmd_*.cpp take a plain struct with no
// dependency on CLI11.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace args {

// `process`. The largest surface.
struct Process {
    std::string input_path;
    std::string output_path;
    std::string model_name;
    std::string models_dir = "./models";
    std::string config_file;        // empty = none
    int tile_size = 256;
    int tile_overlap = 16;
    int min_tile = 64;
    int max_tile = 512;
    int batch_size = 1;
    int jobs = 1;
    // -1 = "not set"; downstream treats unset as quality 95. CLI11's
    // capture_default_str would print "-1" in help, which is misleading, so the
    // option is registered without capture_default_str and help text spells it out.
    int output_quality = -1;
    std::string output_format;      // empty = auto from extension
    bool auto_tile = false;
    bool tile_size_explicit = false;  // set by cli_app.cpp when --tile-size is given
    bool use_gpu = false;
    bool no_daemon = false;
};

// `model list` — no flags.
struct Model {};

// `perf {list-probes, gpu-efficiency, gpu-concurrency, gpu-reuse}`.
// One instance per nested subcommand (see cli_app.cpp) so each gets its own
// default values. duration/warmup are kept as raw strings ("5s", "1.5") and
// parsed to seconds in cmd_perf, matching the legacy "strip trailing s" rule.
struct Perf {
    std::string subcommand;         // list-probes / gpu-efficiency / ...
    std::string model_name;
    std::string models_dir = "./models";
    std::string out_path;
    int tile_size = 256;
    int concurrency = 2;            // gpu-concurrency default; gpu-reuse overrides to 1
    std::string duration_s = "5s";  // gpu-efficiency default; others override
    std::string warmup_s = "1s";
    int sample_ms = 100;
    bool no_cpu_baseline = false;   // gpu-efficiency only
};

// `server {start, info}`.
struct Server {
    std::string subcommand;         // start / info
    uint16_t port = 9527;
    bool auto_approve = false;      // start: auto-approve pairing requests (testing/CI)
};

// `serve {start, stop}`.
struct Serve {
    std::string subcommand;         // start / stop (default start)
    std::string model_name;
    std::string models_dir = "./models";
    std::string socket_path;        // empty = default_serve_socket_path()
    bool force_cpu = false;
    bool foreground = false;
};

// `completion <shell>`.
struct Completion {
    std::string shell;              // bash / zsh / fish
};

}  // namespace args
