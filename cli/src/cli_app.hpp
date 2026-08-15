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
namespace args { struct Server;    }
namespace args { struct Serve;     }

int cmd_process(const args::Process& a);
int cmd_model(const args::Model& a);
int cmd_perf(const args::Perf& a);
int cmd_server(const args::Server& a);
int cmd_serve(const args::Serve& a);
