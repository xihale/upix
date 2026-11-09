// Upix CLI — thin entry point.
//
// The command tree (subcommands, options, validators, help text) is built in
// cli_app.cpp via CLI11. main() just parses argv and dispatches to the
// cmd_*.cpp entry point that CLI11 selected.
#include <optional>
#include <print>
#include <string>

#include <CLI/CLI.hpp>

#include "cli_app.hpp"
#include "common.hpp"  // g_running, g_cancel_requested

// Defined in cli_app.cpp; carries the selected subcommand's exit code. std::nullopt
// means no subcommand's callback has run yet (bare --help, or no verb given).
extern std::optional<int> g_exit_code;

auto main(int argc, char* argv[]) -> int {
    g_running = true;
    g_cancel_requested = false;

    // Hidden dynamic-completion entry (cobra-style): the emitted shell scripts
    // replay the line being completed into this and print its candidates.
    // Handled before CLI11 so arbitrary words are never parsed as flags.
    if (argc >= 2 && std::string(argv[1]) == "__complete") {
        return cmd_complete(argc - 2, argv + 2);
    }

    auto app = build_app();

    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // app->exit() centralizes: help → stdout, version → stdout, errors → stderr.
        return app->exit(e, std::cout, std::cerr);
    }

    // A subcommand ran and set g_exit_code → honor it.
    if (g_exit_code.has_value()) {
        return g_exit_code.value();
    }

    // No subcommand ran (e.g. bare `upix`). Show top-level help.
    std::print("{}", app->help());
    return 0;
}
