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
#include <vector>

namespace args {

// `process`. The largest surface.
struct Process {
    std::string input_path;
    std::string output_path;
    std::string model_name;
    std::string models_dir;         // pre-filled by cli_app.cpp from default_models_dir()
    std::string config_file;        // empty = none
    int tile_size = 256;
    int tile_overlap = 16;
    int min_tile = 64;
    int max_tile = 512;
    int batch_size = 1;
    int jobs = 1;
    bool jobs_explicit = false;     // set by cli_app.cpp when -j/--jobs is given
                                    // (unset: GPU dir mode sizes workers by VRAM)
    // -1 = "not set"; downstream treats unset as quality 95. CLI11's
    // capture_default_str would print "-1" in help, which is misleading, so the
    // option is registered without capture_default_str and help text spells it out.
    int output_quality = -1;
    std::string output_format;      // empty = auto from extension
    bool auto_tile = false;
    bool tile_size_explicit = false;  // set by cli_app.cpp when --tile-size is given
    bool use_gpu = false;
    bool no_daemon = false;
    bool force = false;               // re-process inputs carrying an SR marker
                                      // (default: such inputs are skipped)
};

// `model {list, list-remote, download, import}`. One instance per nested
// subcommand (see cli_app.cpp) so each carries its own models_dir/repo.
struct Model {
    std::string subcommand;         // list / list-remote / download / import
    std::string models_dir;         // pre-filled by cli_app.cpp from default_models_dir()
    std::string repo;               // "owner/name", empty = default upstream
    std::vector<std::string> names; // download: model names from the manifest
    std::vector<std::string> files; // import: local .param/.bin paths
    bool names_only = false;        // list: one name per line, no header/sizes
                                    // (machine-readable; used by completions)
};

// `host {start, info}` — the coordination server phones pair with.
struct Host {
    std::string subcommand;         // start / info (bare `host` = start)
    uint16_t port = 9527;
    bool auto_approve = false;      // start: auto-approve pairing requests (testing/CI)
    std::string models_dir;         // pre-filled by cli_app.cpp from default_models_dir()
};

// `daemon {start, stop, status}` — the local warm-engine daemon.
struct Daemon {
    std::string subcommand;         // start / stop / status (bare `daemon` = start)
    std::string model_name;
    std::string models_dir;         // pre-filled by cli_app.cpp from default_models_dir()
    std::string socket_path;        // empty = default_serve_socket_path()
    bool force_cpu = false;
    bool foreground = false;
    int idle_timeout_s = 600;       // exit after this many idle seconds (0 = never)
};

// `completions <bash|zsh|fish>` — emit a shell completion script to stdout,
// or with -i/--install write it into the shell's user completion directory.
struct Completions {
    std::string shell;              // bash / zsh / fish
    bool install = false;           // write to the standard dir instead of printing
};

}  // namespace args
