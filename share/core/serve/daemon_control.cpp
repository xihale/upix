#include "core/serve/daemon_control.hpp"

#include <csignal>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#endif

#include "core/serve/daemon.hpp"
#include "utils/logger.hpp"
#include "utils/local_socket.hpp"  // local_endpoint_exists

namespace fs = std::filesystem;

namespace upix {
namespace {

// Signals (POSIX) / console handlers (Windows) reach into the running daemon
// (a handler cannot capture state, so the address sits in file-static
// storage — exactly one daemon per process).
ServeDaemon* g_control_daemon = nullptr;

#ifdef _WIN32
extern "C" BOOL WINAPI control_daemon_console_handler(DWORD type) {
    // CTRL_C/CTRL_BREAK in a console run; CTRL_CLOSE when the console window
    // dies. Any of them means "stop serving": request shutdown, return TRUE
    // so the default termination waits for our normal unwind.
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            if (g_control_daemon) g_control_daemon->stop();
            return TRUE;
        default:
            return FALSE;
    }
}

void install_shutdown_handlers() {
    ::SetConsoleCtrlHandler(control_daemon_console_handler, TRUE);
}

void sleep_ms(int ms) { ::Sleep(static_cast<DWORD>(ms)); }
#else
extern "C" void control_daemon_signal_handler(int) {
    if (g_control_daemon) g_control_daemon->stop();
}

void install_shutdown_handlers() {
    std::signal(SIGINT, control_daemon_signal_handler);
    std::signal(SIGTERM, control_daemon_signal_handler);
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
#endif

}  // namespace

#ifdef _WIN32
auto pid_is_upix_daemon(pid_t pid) -> bool {
    if (pid == 0) return false;
    // Windows has no zombies; a live process answers with a queryable
    // handle. PROCESS_QUERY_LIMITED_INFORMATION needs no elevation.
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t image[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool named = ::QueryFullProcessImageNameW(h, 0, image, &size) != 0;
    ::CloseHandle(h);
    if (!named) return false;
    // Base name only — "upix.exe"/"upix-cli.exe" in ANY directory counts;
    // the pid file lives under the user's own LOCALAPPDATA\upix\, so the
    // recycling threat model is a same-user process, not a system one.
    const std::wstring_view full(image, size);
    const auto slash = full.find_last_of(L"\\/");
    const std::wstring_view base = slash == std::wstring_view::npos
                                       ? full : full.substr(slash + 1);
    return base.size() >= 4 && base.compare(0, 4, L"upix") == 0;
}
#else
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
#endif

auto serve_daemon_status(const std::string& socket_path) -> ServeDaemonStatus {
    ServeDaemonStatus st;
    st.socket_path = socket_path;
    const std::string pid_path = serve_pid_file_path(socket_path);
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
    st.socket_missing = !local_endpoint_exists(socket_path);
    return st;
}

auto serve_daemon_stop(const std::string& socket_path) -> bool {
    const std::string pid_path = serve_pid_file_path(socket_path);
    std::ifstream pf(pid_path);
    if (!pf) return false;
    pid_t pid = 0;
    pf >> pid;
    if (pid <= 0) {
        // Corrupt pid file: remove so the next start isn't confused.
        std::error_code ec;
        fs::remove(pid_path, ec);
        return false;
    }
    // Same guard the start path applies: the pid file is untrusted input.
    // Signalling whatever pid a stale or corrupted file names would turn
    // a stop into an arbitrary-pid kill primitive.
    if (!pid_is_upix_daemon(pid)) {
        std::error_code ec;
        fs::remove(pid_path, ec);
        return false;
    }
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) {
        LOG_ERROR("Daemon", "stop: OpenProcess pid {} failed ({})", pid,
                  ::GetLastError());
        return false;
    }
    if (!::TerminateProcess(h, 1)) {
        LOG_ERROR("Daemon", "stop: TerminateProcess pid {} failed ({})", pid,
                  ::GetLastError());
        ::CloseHandle(h);
        return false;
    }
    // Wait for the daemon to actually exit so a follow-up status check does
    // not report "running" for a moment after stop. The pipe name self-cleans
    // with the process; the pid file removal below matches POSIX semantics.
    const DWORD w = ::WaitForSingleObject(h, 3000);
    ::CloseHandle(h);
    if (w != WAIT_OBJECT_0) {
        LOG_WARNING("Daemon",
                    "stop signal sent; pid {} still exiting (processing a request?)",
                    pid);
    }
    std::error_code ec;
    fs::remove(pid_path, ec);
    return true;
#else
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
        sleep_ms(50);
    }
    LOG_WARNING("Daemon",
                "stop signal sent; pid {} still exiting (processing a request?)", pid);
    return true;
#endif
}

auto run_serve_daemon(const ServeDaemonOptions& options) -> int {
    std::string socket_path = options.socket_path;
    if (socket_path.empty()) {
        socket_path = default_serve_socket_path();
    }
    if (socket_path.empty()) {
        LOG_ERROR("Daemon",
                  "no default endpoint available (set XDG_CACHE_HOME or HOME)");
        return 1;
    }
    const std::string pid_path = serve_pid_file_path(socket_path);

    // Windows: the pid file directory is %LOCALAPPDATA%\upix\ — create it.
#ifdef _WIN32
    {
        std::error_code ec;
        const fs::path p(pid_path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
        }
    }
#endif

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
        // daemon holds the endpoint) must not unlink the winner's pid file.
        std::ifstream pf(pid_path);
        pid_t owner = 0;
        if (pf) pf >> owner;
        if (!pid_is_upix_daemon(owner)) {
            std::error_code ec;
            fs::remove(pid_path, ec);
        }
        return rc;
    }

    // Write pid file after successful start (so stop can find us).
#ifdef _WIN32
    std::ofstream(pid_path) << static_cast<unsigned long>(::GetCurrentProcessId());
#else
    std::ofstream(pid_path) << ::getpid();
#endif

    g_control_daemon = &daemon;
    install_shutdown_handlers();

    daemon.run();  // returns when stop_requested_ is set (via signal) or idle timeout

    g_control_daemon = nullptr;

    // Cleanup on shutdown: the daemon destructor unlinks the socket (no-op on
    // Windows — pipe names vanish with the handle); we unlink pid.
    std::error_code ec;
    fs::remove(pid_path, ec);
    LOG_INFO("Daemon", "daemon shutting down");
    return 0;
}

}  // namespace upix
