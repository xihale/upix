// `upix daemon {start, stop, status}` — manage the local warm-engine daemon.
//
// start:  fork into background (unless --fg) and run share's
//         run_serve_daemon() (open the Unix socket, load a warm ncnn engine,
//         serve single-image upscale requests so that `upix process` skips
//         the ~490ms init+load fixed cost; exits by itself after
//         --idle-timeout seconds without a request, 0 = never).
// stop:   share's serve_daemon_stop() — verified pid + SIGTERM + exit wait.
// status: share's serve_daemon_status() — read-only report.
//
// The desktop drives the very same three functions in-process
// (docs/SPEC-frontends.md stage 2), so a desktop without a CLI installed
// still supervises the daemon.
#include <csignal>
#include <filesystem>
#include <format>
#include <print>
#include <string>

#include <unistd.h>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "core/serve/daemon.hpp"      // default_serve_socket_path
#include "core/serve/daemon_control.hpp"
#include "utils/logger.hpp"

namespace fs = std::filesystem;

int cmd_daemon(const args::Daemon& a) {
    std::string socket_path = a.socket_path;
    if (socket_path.empty()) {
        socket_path = upix::default_serve_socket_path();
    }
    if (socket_path.empty()) {
        std::println(stderr, "daemon: no default socket path available "
                             "(set XDG_CACHE_HOME or HOME)");
        return 1;
    }

    if (a.subcommand == "stop") {
        if (a.json) enable_machine_output();
        const bool stopped = upix::serve_daemon_stop(socket_path);
        if (a.json) {
            emit_json_line(std::format(R"({{"stopped":{}}})",
                                       stopped ? "true" : "false"));
        }
        return stopped ? 0 : 1;
    }

    if (a.subcommand == "status") {
        const auto st = upix::serve_daemon_status(socket_path);
        if (a.json) {
            if (!st.running) {
                emit_json_line(std::format(
                    R"({{"running":false,"stale":{}}})",
                    st.stale ? "true" : "false"));
                return 1;
            }
            emit_json_line(std::format(
                R"({{"running":true,"pid":{},"socket":"{}","socketMissing":{}}})",
                st.pid, json_escape(st.socket_path),
                st.socket_missing ? "true" : "false"));
            return 0;
        }
        if (!st.running) {
            if (st.stale) std::println("not running (stale pid file)");
            else std::println("not running");
            return 1;
        }
        // Liveness alone can race with shutdown; the socket is the real contract.
        if (st.socket_missing)
            std::println("running (pid {}, socket {} — socket file missing!)",
                         st.pid, st.socket_path);
        else
            std::println("running (pid {}, socket {})", st.pid, st.socket_path);
        return 0;
    }

    // ── start ────────────────────────────────────────────────────────────────
    // Background via single fork + setsid (detaches controlling terminal);
    // the child runs share's run_serve_daemon body, which owns the pid file
    // and the signal wiring.
    if (!a.foreground) {
        pid_t pid = ::fork();
        if (pid < 0) { LOG_ERROR("Daemon", "fork failed"); return 1; }
        if (pid > 0) return 0;  // parent exits
        ::setsid();
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
    }

    return upix::run_serve_daemon({socket_path, a.models_dir, a.model_name,
                                   a.force_cpu, a.idle_timeout_s});
}
