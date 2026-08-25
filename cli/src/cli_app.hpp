// CLI11 schema + per-subcommand entry points.
//
// cli_app.cpp builds the CLI11 `App` tree (subcommands, options, validators,
// help text) and wires each subcommand's callback to the corresponding cmd_*
// function defined in cmd_*.cpp. main.cpp just calls build_app() + CLI11_PARSE.
#pragma once

#include <memory>

namespace CLI { class App; }

// Build the fully-wired CLI11 App. Returns a unique_ptr so main owns it.
auto build_app() -> std::unique_ptr<CLI::App>;

// ── Subcommand entry points (one per cmd_*.cpp) ─────────────────────────────
// Each takes its parsed args and returns a process exit code.
// Forward-declared here; defined in their respective cmd_*.cpp.
namespace args { struct Process;   }
namespace args { struct Model;     }
namespace args { struct Perf;      }
namespace args { struct Host;      }
namespace args { struct Daemon;    }
namespace args { struct Completions; }

int cmd_process(const args::Process& a);
int cmd_model(const args::Model& a);
int cmd_host(const args::Host& a);
int cmd_daemon(const args::Daemon& a);
int cmd_completions(const args::Completions& a);

// `__complete WORDS...` — dynamic completion engine (see cmd_complete.cpp).
// Dispatched from main() before CLI11 parsing; not part of the CLI11 tree.
int cmd_complete(int argc, char* argv[]);
