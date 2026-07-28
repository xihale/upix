// `crossscale serve {start, stop}` — manage the local warm-engine daemon.
//
// start: fork into background (unless --fg), open the Unix socket, load a warm
//        ncnn engine, and serve single-image upscale requests so that
//        `crossscale process` skips the ~490ms init+load fixed cost.
// stop:  read the daemon's pid file and SIGTERM it.
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

int cmd_serve(const args::Serve& a) {
    std::string socket_path = a.socket_path;
    if (socket_path.empty()) {
        socket_path = crossscale::default_serve_socket_path();
    }
    if (socket_path.empty()) {
        LOG_ERROR("Serve", "no default socket path available (set XDG_CACHE_HOME or HOME)");
        return 1;
    }

    std::string pid_path = socket_path + ".pid";

    if (a.subcommand == "stop") {
        // Read pid, send SIGTERM. The daemon's run() loop observes the
        // resulting g_running=false and exits; its destructor unlinks the
        // socket, and the start path unlinks the pid file.
        std::ifstream pf(pid_path);
        if (!pf) {
            LOG_ERROR("Serve", "no pid file at " + pid_path + " (daemon not running?)");
            return 1;
        }
        pid_t pid = 0;
        pf >> pid;
        if (pid <= 0) {
            LOG_ERROR("Serve", "corrupt pid file at " + pid_path);
            return 1;
        }
        if (::kill(pid, SIGTERM) < 0) {
            LOG_ERROR("Serve", std::string("kill pid ") + std::to_string(pid) +
                               " failed: " + std::strerror(errno));
            // Stale pid file: clean it up so next start isn't confused.
            if (errno == ESRCH) std::filesystem::remove(pid_path);
            return 1;
        }
        LOG_INFO("Serve", "stopped pid " + std::to_string(pid));
        return 0;
    }

    // ── start ───────────────────────────────────────────────────────────────
    // Guard: if a pid file exists and the pid is alive, refuse (already running).
    {
        std::ifstream pf(pid_path);
        if (pf) {
            pid_t existing = 0;
            pf >> existing;
            if (existing > 0 && ::kill(existing, 0) == 0) {
                LOG_ERROR("Serve", "daemon already running, pid " + std::to_string(existing));
                return 1;
            }
            // pid file present but process dead → stale, overwrite below.
        }
    }

    // Background via single fork + setsid (detaches controlling terminal).
    if (!a.foreground) {
        pid_t pid = ::fork();
        if (pid < 0) { LOG_ERROR("Serve", "fork failed"); return 1; }
        if (pid > 0) return 0;  // parent exits
        ::setsid();
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
    }

    crossscale::ServeDaemon daemon;
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
    // so a SIGTERM from `serve stop` would otherwise leave the daemon stuck in
    // accept(). g_serve_daemon lets the handler reach in and request the
    // daemon's own shutdown.
    g_serve_daemon = &daemon;
    std::signal(SIGINT, serve_signal_handler);
    std::signal(SIGTERM, serve_signal_handler);

    daemon.run();  // returns when stop_requested_ is set (via signal)

    g_serve_daemon = nullptr;

    // Cleanup on shutdown: daemon destructor unlinks the socket; we unlink pid.
    std::filesystem::remove(pid_path);
    LOG_INFO("Serve", "daemon shutting down");
    return 0;
}
