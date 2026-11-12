// Serve-daemon lifecycle control shared by the CLI (`upix daemon`) and the
// desktop (docs/SPEC-frontends.md stage 2: supervision without a CLI).
// The pid file is untrusted input everywhere: every consumer verifies the
// pid names a live upix daemon before acting on it.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#ifdef _WIN32
// pid_t below
#else
#include <sys/types.h>
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
// daemon "process body": `upix daemon` and `--upix-internal-daemon` call it,
// and the desktop's `--serve-daemon` mode calls it as its main().
// Returns the process exit code.
auto run_serve_daemon(const ServeDaemonOptions& options) -> int;

// Absolute path of the currently running executable (/proc/self/exe on Linux,
// _NSGetExecutablePath on macOS, GetModuleFileNameW on Windows).
auto current_executable_path() -> std::optional<std::filesystem::path>;

// True when the local serve socket / named pipe currently exists.
auto daemon_endpoint_present(const std::string& socket_path = {}) -> bool;

// Spawn a detached background serve daemon child (`current_executable_path`
// with `--upix-internal-daemon`) without waiting for endpoint readiness.
// On POSIX uses double-fork + setsid (zero zombies); on Windows uses
// CreateProcessW with DETACHED_PROCESS | CREATE_NO_WINDOW.
auto spawn_serve_daemon(const ServeDaemonOptions& options,
                        std::string& err) -> bool;

// If argv[1] is `--upix-internal-daemon`, parse the remaining daemon arguments
// and execute run_serve_daemon(), returning its exit code. Returns nullopt for
// normal CLI/GUI invocations.
auto maybe_run_internal_serve_daemon(int argc, char* argv[]) -> std::optional<int>;

// Ensure a warm serve daemon is running and accepting connections at
// `socket_path` (empty = default_serve_socket_path()). If none is running,
// spawns a detached daemon and waits up to 30 s for readiness.
auto ensure_serve_daemon_running(
    const std::string& models_dir,
    int idle_secs,
    std::string& err,
    const std::string& socket_path = {},
    const std::string& model_name = {},
    bool force_cpu = false) -> bool;

// Client-side single-image RPC parameters.
struct ServeRequestArgs {
    std::string input_path;
    std::string output_path;
    std::string model_name;
    std::string output_format;
    int quality = 90;
    int png_level = -1;
    int webp_method = -1;
    int avif_speed = 6;
    int chroma = 420;
    int avif_subsampling = 0;  // 0 = follow chroma (444 -> 444, else 420)
    int avif_threads = 0;
    bool webp_lossless = false;
    bool use_gpu = true;
    bool force = false;
    uint32_t tile_size = 0;
    int32_t tile_overlap = 16;
};

struct ServeOutcome {
    int exit_code = 1;
    bool ok = false;
    bool skipped = false;
    bool cancelled = false;
    bool transport_fallback = false;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::string out_path;
    std::string model_name;
    std::string message;
};

using ServeProgressCallback = std::function<void(
    uint32_t completed_tiles, uint32_t total_tiles, uint32_t stage)>;

// Send one single-image upscale request over the local serve endpoint and
// drain progress frames until ServeResponse (or cancellation).
auto serve_request(
    const ServeRequestArgs& args,
    const std::string& socket_path = {},
    const std::atomic<bool>* cancel = nullptr,
    ServeProgressCallback on_progress = nullptr) -> ServeOutcome;

}  // namespace upix
