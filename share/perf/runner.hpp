// Runner: top-level entry. Resolves the test by name and executes it with the
// given config. Keeps main.cpp thin (one call per subcommand).
#pragma once

#include "perf/test.hpp"

namespace crossscale::perf {

// Run a named test. Returns process exit code.
auto run_test(const std::string& test_name, const TestConfig& cfg, const std::string& out_path) -> int;

// List available tests and probes (for `perf list-probes`).
auto list_available() -> void;

} // namespace crossscale::perf
