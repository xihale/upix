#include "core/serve/daemon_control.hpp"

#include <csignal>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>

#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#include "core/serve/daemon.hpp"
#include "utils/logger.hpp"

namespace fs = std::filesystem;

namespace upix {
namespace {

// Signals reach into the running daemon (a handler cannot capture state, so
// the address sits in file-static storage — exactly one daemon per process).
ServeDaemon* g_control_daemon = nullptr;

extern "C" void control_daemon_signal_handler(int) {
    if (g_control_daemon) g_control_daemon->stop();
}

}  // namespace

auto pid_is_upix_daemon(pid_t pid) -> bool {
    if (pid <= 0 || ::kill(pid, 0) != 0) {
        return false;
    }
#if defined(__linux__)
    // A zombie (parent not reaping) and any process that recycled this pid
    // both pass kill(0). The exe symlink is the reliable check: it fails for
    // zombies and names the real binary otherwise.
    std::error_code ec;
    const auto exe = fs::read_symlink(std::format("/proc/{}/exe", pid), ec);
    if (ec || exe.string().find("upix") == std::string::npos) {
        return false;
    }
    // And it must be a daemon ("daemon" subcommand or the desktop's
    // "--serve-daemon" mode), not an unrelated upix process.
    std::ifstream cmd(std::format("/proc/{}/cmdline", pid), std::ios::binary);
    const std::string argv((std::istreambuf_iterator<char>(cmd)),
                           std::istreambuf_iterator<char>());
    return argv.find("daemon") != std::string::npos;
#elif defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (::proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return false;  // zombies fail here too
    }
    return std::string_view(path).find("upix") != std::string_view::npos;
#else
    return true;  // no executable probe on this platform: keep kill(0) semantics
#endif
}

auto serve_daemon_status(const std::string& socket_path) -> ServeDaemonStatus {
    ServeDaemonStatus st;
    st.socket_path = socket_path;
    const std::string pid_path = socket_path + ".pid";
    std::ifstream pf(pid_path);
    if (!pf) {
        return st;  // not running, not stale
    }
    pid_t pid = 0;
    pf >> pid;
    if (pid <= 0 || !pid_is_upix_daemon(pid)) {
        st.stale = true;
        return st;
    }
    st.running = true;
    st.pid = pid;
    std::error_code ec;
    st.socket_missing = !fs::exists(socket_path, ec);
    return st;
}

auto serve_daemon_stop(const std::string& socket_path) -> bool {
    const std::string pid_path = socket_path + ".pid";
    std::ifstream pf(pid_path);
    if (!pf) return false;
    pid_t pid = 0;
    pf >> pid;
    if (pid <= 0) {
        // Corrupt pid file: remove so the next start isn't confused.
        fs::remove(pid_path);
        return false;
    }
    // Same guard the start path applies: the pid file is untrusted input.
    // Signalling whatever pid a stale or corrupted file names would turn
    // a stop into an arbitrary-pid SIGTERM primitive.
    if (!pid_is_upix_daemon(pid)) {
        fs::remove(pid_path);
        return false;
    }
    if (::kill(pid, SIGTERM) < 0) {
        LOG_ERROR("Daemon", "stop: kill pid {} failed: {}", pid, std::strerror(errno));
        // Stale pid file: clean it up so next start isn't confused.
        if (errno == ESRCH) fs::remove(pid_path);
        return false;
    }
    // Wait for the daemon to actually exit: its accept() loop only wakes
    // on the next poll tick (~500 ms), so returning immediately would
    // leave a status check reporting "running" for a moment after stop.
    for (int i = 0; i < 60; ++i) {  // up to 3 s
        if (!pid_is_upix_daemon(pid)) {
            return true;
        }
        ::usleep(50 * 1000);
    }
    LOG_WARNING("Daemon",
                "stop signal sent; pid {} still exiting (processing a request?)", pid);
    return true;
}

auto run_serve_daemon(const ServeDaemonOptions& options) -> int {
    std::string socket_path = options.socket_path;
    if (socket_path.empty()) {
        socket_path = default_serve_socket_path();
    }
    if (socket_path.empty()) {
        LOG_ERROR("Daemon",
                  "no default socket path available (set XDG_CACHE_HOME or HOME)");
        return 1;
    }
    const std::string pid_path = socket_path + ".pid";

    // Guard: if a pid file exists and names a live upix daemon, refuse
    // (already running). kill(0) alone would also bless zombies and recycled
    // pids and wedge every future start.
    {
        std::ifstream pf(pid_path);
        if (pf) {
            pid_t existing = 0;
            pf >> existing;
            if (pid_is_upix_daemon(existing)) {
                LOG_ERROR("Daemon", "daemon already running (pid {})", existing);
                return 1;
            }
            // pid file present but process dead → stale, overwritten below.
        }
    }

    ServeDaemon daemon;
    daemon.set_idle_timeout(options.idle_timeout_s);
    int rc = daemon.start(socket_path, options.models_dir, options.model_name,
                          options.force_cpu ? ServeDevice::CPU : ServeDevice::GPU);
    if (rc != 0) {
        // We never wrote the pid file on a failed start, so whatever sits
        // there belongs to someone else. Remove it only when it describes no
        // live upix daemon: a lost start race (bind refused because another
        // daemon holds the socket) must not unlink the winner's pid file.
        std::ifstream pf(pid_path);
        pid_t owner = 0;
        if (pf) pf >> owner;
        if (!pid_is_upix_daemon(owner)) {
            fs::remove(pid_path);
        }
        return rc;
    }

    // Write pid file after successful start (so stop can find us).
    std::ofstream(pid_path) << ::getpid();

    g_control_daemon = &daemon;
    std::signal(SIGINT, control_daemon_signal_handler);
    std::signal(SIGTERM, control_daemon_signal_handler);

    daemon.run();  // returns when stop_requested_ is set (via signal) or idle timeout

    g_control_daemon = nullptr;

    // Cleanup on shutdown: daemon destructor unlinks the socket; we unlink pid.
    fs::remove(pid_path);
    LOG_INFO("Daemon", "daemon shutting down");
    return 0;
}

}  // namespace upix
