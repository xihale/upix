// `crossscale daemon {start, stop, status}` — manage the local warm-engine daemon.
//
// start:  fork into background (unless --fg), open the Unix socket, load a warm
//         ncnn engine, and serve single-image upscale requests so that
//         `crossscale process` skips the ~490ms init+load fixed cost. Exits by
//         itself after --idle-timeout seconds without a request (0 = never).
// stop:   read the daemon's pid file and SIGTERM it.
// status: read-only report — pid file, liveness, socket reachability.
#include <csignal>
#include <fstream>
#include <filesystem>
#include <string>

#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/logger.hpp"
#include "core/serve/daemon.hpp"

namespace fs = std::filesystem;

int cmd_daemon(const args::Daemon& a) {
    std::string socket_path = a.socket_path;
    if (socket_path.empty()) {
        socket_path = crossscale::default_serve_socket_path();
    }
    if (socket_path.empty()) {
        LOG_ERROR("Daemon", "no default socket path available (set XDG_CACHE_HOME or HOME)");
        return 1;
    }

    std::string pid_path = socket_path + ".pid";

    if (a.subcommand == "stop") {
        // Read pid, send SIGTERM. The daemon's run() loop observes the
        // resulting g_running=false and exits; its destructor unlinks the
        // socket, and the start path unlinks the pid file.
        std::ifstream pf(pid_path);
        if (!pf) {
            LOG_ERROR("Daemon", "no pid file at " + pid_path + " (daemon not running?)");
            return 1;
        }
        pid_t pid = 0;
        pf >> pid;
        if (pid <= 0) {
            LOG_ERROR("Daemon", "corrupt pid file at " + pid_path);
            return 1;
        }
        if (::kill(pid, SIGTERM) < 0) {
            LOG_ERROR("Daemon", std::string("kill pid ") + std::to_string(pid) +
                               " failed: " + std::strerror(errno));
            // Stale pid file: clean it up so next start isn't confused.
            if (errno == ESRCH) std::filesystem::remove(pid_path);
            return 1;
        }
        // Wait for the daemon to actually exit: its accept() loop only wakes
        // on the next poll tick (~500 ms), so returning immediately would
        // leave `daemon status` reporting "running" for a moment after stop.
        for (int i = 0; i < 60; ++i) {  // up to 3 s
            if (::kill(pid, 0) != 0 && errno == ESRCH) {
                LOG_INFO("Daemon", "stopped pid " + std::to_string(pid));
                return 0;
            }
            ::usleep(50 * 1000);
        }
        LOG_WARNING("Daemon", "pid " + std::to_string(pid) +
                      " still alive 3s after SIGTERM (processing a request?)");
        return 0;
    }

    if (a.subcommand == "status") {
        std::ifstream pf(pid_path);
        if (!pf) {
            LOG_INFO("Daemon", "not running (no pid file at " + pid_path + ")");
            return 1;
        }
        pid_t pid = 0;
        pf >> pid;
        if (pid <= 0) {
            LOG_ERROR("Daemon", "corrupt pid file at " + pid_path);
            return 1;
        }
        if (::kill(pid, 0) != 0) {
            LOG_INFO("Daemon", "pid " + std::to_string(pid) + " is dead (stale pid file)");
            return 1;
        }
        // Liveness alone can race with shutdown; the socket is the real contract.
        std::error_code ec;
        bool sock_exists = std::filesystem::exists(socket_path, ec);
        LOG_INFO("Daemon", "running, pid " + std::to_string(pid) +
                           " (socket " + socket_path +
                           (sock_exists ? ": up)" : ": missing)"));
        return 0;
    }

    // ── start ────────────────────────────────────────────────────────────────
    // Guard: if a pid file exists and the pid is alive, refuse (already running).
    {
        std::ifstream pf(pid_path);
        if (pf) {
            pid_t existing = 0;
            pf >> existing;
            if (existing > 0 && ::kill(existing, 0) == 0) {
                LOG_ERROR("Daemon", "daemon already running, pid " + std::to_string(existing));
                return 1;
            }
            // pid file present but process dead → stale, overwrite below.
        }
    }

    // Background via single fork + setsid (detaches controlling terminal).
    if (!a.foreground) {
        pid_t pid = ::fork();
        if (pid < 0) { LOG_ERROR("Daemon", "fork failed"); return 1; }
        if (pid > 0) return 0;  // parent exits
        ::setsid();
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
    }

    crossscale::ServeDaemon daemon;
    daemon.set_idle_timeout(a.idle_timeout_s);
    int rc = daemon.start(socket_path, a.models_dir, a.model_name,
                          a.force_cpu ? crossscale::ServeDevice::CPU
                                      : crossscale::ServeDevice::GPU);
    if (rc != 0) {
        std::filesystem::remove(pid_path);
        return rc;
    }

    // Write pid file after successful start (so stop can find us).
    std::ofstream(pid_path) << ::getpid();

    // Wire signals to the daemon's stop(). `signal_handler` only sets the
    // generic g_running flag, which the daemon's run() loop does NOT observe —
    // so a SIGTERM from `daemon stop` would otherwise leave the daemon stuck in
    // accept(). g_daemon lets the handler reach in and request the
    // daemon's own shutdown.
    g_daemon = &daemon;
    std::signal(SIGINT, daemon_signal_handler);
    std::signal(SIGTERM, daemon_signal_handler);

    daemon.run();  // returns when stop_requested_ is set (via signal) or idle timeout

    g_daemon = nullptr;

    // Cleanup on shutdown: daemon destructor unlinks the socket; we unlink pid.
    std::filesystem::remove(pid_path);
    LOG_INFO("Daemon", "daemon shutting down");
    return 0;
}
