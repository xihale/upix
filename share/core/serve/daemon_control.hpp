// Serve-daemon lifecycle control shared by the CLI (`upix daemon`) and the
// desktop (docs/SPEC-frontends.md stage 2: supervision without a CLI).
// The pid file is untrusted input everywhere: every consumer verifies the
// pid names a live upix daemon before acting on it.
#pragma once

#include <optional>
#include <string>

#ifdef _WIN32
#include <cstdint>
#endif

namespace upix {

#ifdef _WIN32
// DWORD-width process id on Windows; POSIX pulls the real pid_t from
// <sys/types.h> instead.
using pid_t = unsigned long;
#endif

struct ServeDaemonStatus {
    bool running = false;
    // A pid file exists but names no live upix daemon (dead pid, corrupt
    // file) — "the file lies".
    bool stale = false;
    pid_t pid = 0;
    // Liveness can race with shutdown; the endpoint is the real contract.
    bool socket_missing = false;
    std::string socket_path;
};

auto serve_daemon_status(const std::string& socket_path) -> ServeDaemonStatus;

// Stop a live daemon (verified through its pid file) and wait for it to exit
// (up to ~3 s). SIGTERM on POSIX (graceful), TerminateProcess on Windows
// (hard — the pipe namespace self-cleans, so no unlink is needed). Returns
// false when no daemon was running.
auto serve_daemon_stop(const std::string& socket_path) -> bool;

// Is `pid` a live process that is actually one of our serve daemons?
// kill(pid, 0) alone also succeeds for zombies and for an unrelated process
// that recycled the pid, and both make every caller pay the full
// daemon-spawn wait before falling back inline. Verifies the executable
// (and on Linux the command line) so pid files can be trusted.
auto pid_is_upix_daemon(pid_t pid) -> bool;

// Options for run_serve_daemon (the foreground daemon body).
struct ServeDaemonOptions {
    std::string socket_path;    // empty → default_serve_socket_path()
    std::string models_dir;
    std::string model_name;     // empty → first scanned model
    bool force_cpu = false;
    int idle_timeout_s = 600;   // 0 = never exit
};

// Start, write the pid file, run the serve loop until stop()/idle timeout,
// clean up. Runs the shutdown signal handling itself (SIGINT/SIGTERM on
// POSIX, CTRL_C/CTRL_CLOSE on Windows → graceful stop), so it is the whole
// daemon "process body": `upix daemon` calls it after its own fork, the
// desktop's `--serve-daemon` mode calls it as its main().
// Returns the process exit code.
auto run_serve_daemon(const ServeDaemonOptions& options) -> int;

}  // namespace upix
