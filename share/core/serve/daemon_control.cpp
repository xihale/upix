#include "core/serve/daemon_control.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#endif

#include "core/distributed/protocol.hpp"
#include "core/model_manager.hpp"
#include "core/serve/daemon.hpp"
#include "utils/encoder_options.hpp"
#include "utils/local_socket.hpp"  // local_endpoint_exists
#include "utils/logger.hpp"
#include "utils/network.hpp"       // MAX_NETWORK_PAYLOAD

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

namespace {

constexpr uint32_t MAX_SERVE_CLIENT_PATH_BYTES = 4096u;
constexpr uint32_t MAX_SERVE_CLIENT_RESPONSE_BYTES =
    static_cast<uint32_t>(sizeof(ServeResponse)) + 65536u;

auto bounded_cstr(const char* src, size_t cap) -> std::string {
    const void* nul = std::memchr(src, '\0', cap);
    const size_t len = nul ? static_cast<size_t>(static_cast<const char*>(nul) - src)
                           : cap;
    return {src, len};
}

auto resolve_socket_or_default(const std::string& socket_path) -> std::string {
    return socket_path.empty() ? default_serve_socket_path() : socket_path;
}

auto probe_daemon_connect(const std::string& sock_path) -> bool {
    if (sock_path.empty() || !local_endpoint_exists(sock_path)) {
        return false;
    }
    LocalSocket sock;
    return sock.connect(sock_path).has_value();
}

#ifdef _WIN32
auto utf8_to_wide(std::string_view s) -> std::wstring {
    if (s.empty()) return {};
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring w(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          w.data(), needed);
    return w;
}

auto append_quoted_win32_arg(std::wstring& cmdline, std::wstring_view arg) -> void {
    if (!cmdline.empty()) cmdline.push_back(L' ');
    const bool needs_quotes = arg.empty() ||
        arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        cmdline.append(arg);
        return;
    }
    cmdline.push_back(L'"');
    for (auto it = arg.begin(); ; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            cmdline.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            cmdline.append(backslashes * 2 + 1, L'\\');
            cmdline.push_back(L'"');
        } else {
            cmdline.append(backslashes, L'\\');
            cmdline.push_back(*it);
        }
    }
    cmdline.push_back(L'"');
}
#endif

}  // namespace

auto current_executable_path() -> std::optional<fs::path> {
#ifdef _WIN32
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(
            nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::nullopt;
        if (n < buf.size() - 1) {
            return fs::path(std::wstring_view(buf.data(), n));
        }
        if (buf.size() >= 32768) return std::nullopt;
        buf.resize(buf.size() * 2, L'\0');
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0) return std::nullopt;
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::nullopt;
    }
    std::error_code ec;
    auto canonical = fs::weakly_canonical(fs::path(buf.c_str()), ec);
    return ec ? fs::path(buf.c_str()) : canonical;
#elif defined(__linux__)
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (ec || p.empty()) return std::nullopt;
    return p;
#else
    return std::nullopt;
#endif
}

auto daemon_endpoint_present(const std::string& socket_path) -> bool {
    const std::string resolved = resolve_socket_or_default(socket_path);
    if (resolved.empty()) return false;
    return local_endpoint_exists(resolved);
}

auto spawn_serve_daemon(const ServeDaemonOptions& options,
                        std::string& err) -> bool {
    err.clear();
    const std::string sock = resolve_socket_or_default(options.socket_path);
    if (sock.empty()) {
        err = "no default daemon socket path available";
        return false;
    }
    const auto exe = current_executable_path();
    if (!exe.has_value()) {
        err = "cannot locate current executable path";
        return false;
    }
    const std::string models_dir = options.models_dir.empty()
        ? default_models_dir()
        : options.models_dir;
    const std::string device = options.force_cpu ? "cpu" : "gpu";
    const std::string idle = std::to_string(
        options.idle_timeout_s > 0 ? options.idle_timeout_s : 0);

#ifdef _WIN32
    std::wstring cmdline;
    append_quoted_win32_arg(cmdline, exe->wstring());
    append_quoted_win32_arg(cmdline, L"--upix-internal-daemon");
    append_quoted_win32_arg(cmdline, utf8_to_wide(sock));
    append_quoted_win32_arg(cmdline, utf8_to_wide(models_dir));
    append_quoted_win32_arg(cmdline, utf8_to_wide(options.model_name));
    append_quoted_win32_arg(cmdline, utf8_to_wide(device));
    append_quoted_win32_arg(cmdline, utf8_to_wide(idle));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(
            exe->c_str(),
            cmdline.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS | CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        err = std::format("CreateProcessW failed ({})", ::GetLastError());
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
#else
    // Double-fork so the intermediate trampoline exits immediately and can be
    // reaped without leaving zombies in long-lived parents (Desktop GUI).
    const ::pid_t mid = ::fork();
    if (mid < 0) {
        err = std::format("fork failed: {}", std::strerror(errno));
        return false;
    }
    if (mid == 0) {
        (void)::setsid();
        const ::pid_t grand = ::fork();
        if (grand != 0) {
            ::_exit(grand < 0 ? 127 : 0);
        }
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        std::string exe_str = exe->string();
        std::string flag = "--upix-internal-daemon";
        std::string sock_arg = sock;
        std::string dir_arg = models_dir;
        std::string model_arg = options.model_name;
        std::string dev_arg = device;
        std::string idle_arg = idle;
        char* const child_argv[] = {
            exe_str.data(),
            flag.data(),
            sock_arg.data(),
            dir_arg.data(),
            model_arg.data(),
            dev_arg.data(),
            idle_arg.data(),
            nullptr,
        };
        ::execv(exe_str.c_str(), child_argv);
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(mid, &status, 0) < 0 && errno == EINTR) {}
    return true;
#endif
}

auto maybe_run_internal_serve_daemon(int argc, char* argv[])
    -> std::optional<int> {
    if (argc < 2 || std::string_view(argv[1]) != "--upix-internal-daemon") {
        return std::nullopt;
    }
    ServeDaemonOptions opts;
    opts.models_dir = default_models_dir();

    // Positional contract:
    //   argv[2] = socket_path
    //   argv[3] = models_dir
    //   argv[4] = model_name (may be empty)
    //   argv[5] = "cpu" | "gpu"
    //   argv[6] = idle_timeout_s ("0" = never)
    // Also tolerate --idle-secs=N / --idle-seconds=N / --idle-timeout=N flags.
    int pos = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.starts_with("--idle-secs=")) {
            opts.idle_timeout_s = std::max(0, std::atoi(std::string(arg.substr(12)).c_str()));
            continue;
        }
        if (arg.starts_with("--idle-seconds=")) {
            opts.idle_timeout_s = std::max(0, std::atoi(std::string(arg.substr(15)).c_str()));
            continue;
        }
        if (arg.starts_with("--idle-timeout=")) {
            opts.idle_timeout_s = std::max(0, std::atoi(std::string(arg.substr(15)).c_str()));
            continue;
        }
        switch (pos++) {
            case 0:
                if (!arg.empty()) opts.socket_path = std::string(arg);
                break;
            case 1:
                if (!arg.empty()) opts.models_dir = std::string(arg);
                break;
            case 2:
                opts.model_name = std::string(arg);
                break;
            case 3:
                opts.force_cpu = (arg == "cpu" || arg == "CPU" || arg == "1");
                break;
            case 4:
                if (!arg.empty()) opts.idle_timeout_s = std::max(0, std::atoi(std::string(arg).c_str()));
                break;
            default:
                break;
        }
    }
    return run_serve_daemon(opts);
}

auto ensure_serve_daemon_running(
    const std::string& models_dir,
    int idle_secs,
    std::string& err,
    const std::string& socket_path,
    const std::string& model_name,
    bool force_cpu) -> bool {
    err.clear();
    const std::string sock = resolve_socket_or_default(socket_path);
    if (sock.empty()) {
        err = "no default daemon socket path available";
        return false;
    }
    if (probe_daemon_connect(sock)) {
        return true;
    }
    if (!serve_daemon_status(sock).running) {
        ServeDaemonOptions opts;
        opts.socket_path = sock;
        opts.models_dir = models_dir;
        opts.model_name = model_name;
        opts.force_cpu = force_cpu;
        opts.idle_timeout_s = idle_secs >= 0 ? idle_secs : 600;
        if (!spawn_serve_daemon(opts, err)) {
            return false;
        }
    }
    for (int i = 0; i < 300; ++i) {  // 30 s @ 100 ms
        if (probe_daemon_connect(sock)) {
            return true;
        }
        sleep_ms(100);
    }
    err = std::format("warm engine daemon did not become ready at {}", sock);
    return false;
}

auto serve_request(
    const ServeRequestArgs& args,
    const std::string& socket_path,
    const std::atomic<bool>* cancel,
    ServeProgressCallback on_progress) -> ServeOutcome {
    ServeOutcome out;
    if (cancel && cancel->load()) {
        out.cancelled = true;
        out.message = "Cancelled";
        return out;
    }

    const std::string sock_path = resolve_socket_or_default(socket_path);
    if (sock_path.empty()) {
        out.transport_fallback = true;
        out.message = "no default serve socket path available";
        return out;
    }

    if (args.input_path.empty()) {
        out.message = "empty input path";
        return out;
    }

    std::error_code ec;
    const fs::path in_abs_p = fs::absolute(args.input_path, ec).lexically_normal();
    const std::string in_abs = ec ? args.input_path : in_abs_p.string();
    ec.clear();
    std::string out_abs;
    if (!args.output_path.empty()) {
        const fs::path out_abs_p = fs::absolute(args.output_path, ec).lexically_normal();
        out_abs = ec ? args.output_path : out_abs_p.string();
    }

    if (in_abs.empty() ||
        in_abs.size() > MAX_SERVE_CLIENT_PATH_BYTES ||
        out_abs.size() > MAX_SERVE_CLIENT_PATH_BYTES) {
        out.message = "invalid request paths";
        return out;
    }

    LocalSocket sock;
    if (auto conn = sock.connect(sock_path); !conn) {
        out.transport_fallback = true;
        out.message = conn.error().message;
        return out;
    }
    sock.set_timeout(300000);  // 5 min per frame; cancel watcher wakes immediately

    ServeRequest req{};
    args.model_name.copy(req.model_name, sizeof(req.model_name) - 1);
    args.output_format.copy(req.output_format, sizeof(req.output_format) - 1);
    req.tile_size = static_cast<int32_t>(args.tile_size);
    req.tile_overlap = args.tile_overlap;
    req.output_quality = args.quality;

    EncoderOptions enc;
    enc.png_level = args.png_level;
    enc.jpeg_subsampling = args.chroma;
    enc.webp_method = args.webp_method;
    enc.webp_lossless = args.webp_lossless;
    enc.avif_speed = args.avif_speed;
    enc.avif_subsampling = (args.avif_subsampling == 420 || args.avif_subsampling == 444)
        ? args.avif_subsampling
        : (args.chroma == 444 ? 444 : 420);
    enc.avif_threads = args.avif_threads;
    req.encoder = to_wire(enc);
    req.force = args.force ? 1 : 0;
    req.input_path_length = static_cast<uint32_t>(in_abs.size());
    req.output_path_length = static_cast<uint32_t>(out_abs.size());

    std::vector<uint8_t> payload(sizeof(req) + in_abs.size() + out_abs.size());
    std::memcpy(payload.data(), &req, sizeof(req));
    std::memcpy(payload.data() + sizeof(req), in_abs.data(), in_abs.size());
    if (!out_abs.empty()) {
        std::memcpy(payload.data() + sizeof(req) + in_abs.size(),
                    out_abs.data(), out_abs.size());
    }

    MessageHeader hdr{};
    hdr.magic = Protocol::MAGIC;
    hdr.type = MessageType::MSG_SERVE_REQUEST;
    hdr.payload_length = static_cast<uint32_t>(payload.size());
    hdr.checksum = 0;
    const auto frame = Protocol::serialize(hdr, payload.data());
    if (!sock.send_all(frame.data(), frame.size())) {
        out.transport_fallback = true;
        out.message = "failed to send request to daemon";
        return out;
    }

    // Optional cancellation watcher: calling shutdown_wake() from a background
    // watcher wakes receive_all() immediately without holding rx_mutex_.
    struct CancelWatch {
        std::atomic<bool> done{false};
        std::jthread thread;
        CancelWatch(const std::atomic<bool>* c, LocalSocket* s) {
            if (!c) return;
            thread = std::jthread([this, c, s]() {
                while (!done.load(std::memory_order_acquire)) {
                    if (c->load(std::memory_order_acquire)) {
                        s->shutdown_wake();
                        break;
                    }
                    sleep_ms(20);
                }
            });
        }
        ~CancelWatch() {
            done.store(true, std::memory_order_release);
        }
    } cancel_watch(cancel, &sock);

    std::vector<uint8_t> rhdr(Protocol::HEADER_SIZE);
    while (true) {
        if (cancel && cancel->load()) {
            out.cancelled = true;
            out.message = "Cancelled";
            return out;
        }
        if (!sock.receive_all(rhdr.data(), rhdr.size())) {
            if (cancel && cancel->load()) {
                out.cancelled = true;
                out.message = "Cancelled";
            } else {
                out.message = "daemon connection lost mid-request";
            }
            return out;
        }
        auto parsed = Protocol::parse_header(rhdr);
        if (!parsed) {
            out.message = "malformed daemon response";
            return out;
        }
        if (parsed->payload_length > MAX_SERVE_CLIENT_RESPONSE_BYTES ||
            parsed->payload_length > MAX_NETWORK_PAYLOAD) {
            out.message = "oversized daemon frame";
            return out;
        }

        if (parsed->type == MessageType::MSG_SERVE_PROGRESS) {
            if (parsed->payload_length != sizeof(ServeProgress)) {
                out.message = "malformed daemon progress frame";
                return out;
            }
            std::vector<uint8_t> pbuf(parsed->payload_length);
            if (!sock.receive_all(pbuf.data(), pbuf.size())) {
                if (cancel && cancel->load()) {
                    out.cancelled = true;
                    out.message = "Cancelled";
                } else {
                    out.message = "daemon connection lost mid-progress";
                }
                return out;
            }
            if (!Protocol::verify_frame(rhdr.data(), pbuf.data(), pbuf.size())) {
                out.message = "daemon progress frame failed checksum";
                return out;
            }
            ServeProgress sp{};
            std::memcpy(&sp, pbuf.data(), sizeof(sp));
            if (on_progress) {
                on_progress(sp.completed_tiles, sp.total_tiles, sp.stage);
            }
            continue;
        }

        if (parsed->type == MessageType::MSG_SERVE_DATA) {
            std::vector<uint8_t> dbuf(parsed->payload_length);
            if (!dbuf.empty() && !sock.receive_all(dbuf.data(), dbuf.size())) {
                if (cancel && cancel->load()) {
                    out.cancelled = true;
                    out.message = "Cancelled";
                } else {
                    out.message = "daemon connection lost mid-data";
                }
                return out;
            }
            if (!Protocol::verify_frame(rhdr.data(), dbuf.data(), dbuf.size())) {
                out.message = "daemon data frame failed checksum";
                return out;
            }
            continue;
        }

        if (parsed->type != MessageType::MSG_SERVE_RESPONSE ||
            parsed->payload_length < sizeof(ServeResponse) - 8) {
            out.message = "malformed daemon response";
            return out;
        }

        std::vector<uint8_t> rhead(parsed->payload_length);
        if (!sock.receive_all(rhead.data(), rhead.size())) {
            if (cancel && cancel->load()) {
                out.cancelled = true;
                out.message = "Cancelled";
            } else {
                out.message = "truncated daemon response";
            }
            return out;
        }
        if (!Protocol::verify_frame(rhdr.data(), rhead.data(), rhead.size())) {
            out.message = "daemon response failed checksum";
            return out;
        }

        ServeResponse resp{};
        constexpr size_t kLegacyRespSize = sizeof(ServeResponse) - 8;
        std::memcpy(&resp, rhead.data(), kLegacyRespSize);
        size_t resp_bytes = std::min(rhead.size(), sizeof(resp));
        if (rhead.size() == kLegacyRespSize + static_cast<size_t>(resp.out_path_length)) {
            resp_bytes = kLegacyRespSize;
        } else if (resp_bytes > kLegacyRespSize) {
            std::memcpy(reinterpret_cast<uint8_t*>(&resp) + kLegacyRespSize,
                        rhead.data() + kLegacyRespSize,
                        resp_bytes - kLegacyRespSize);
        }
        if (!resp.ok) {
            out.message = bounded_cstr(resp.error, sizeof(resp.error));
            if (out.message.empty()) out.message = "daemon processing failed";
            if (cancel && cancel->load()) out.cancelled = true;
            return out;
        }
        const size_t avail_path = rhead.size() > resp_bytes ? rhead.size() - resp_bytes : 0;
        const size_t path_len = std::min<size_t>(resp.out_path_length, avail_path);
        out.exit_code = 0;
        out.ok = true;
        out.skipped = (resp.skipped != 0);
        out.width = resp.width;
        out.height = resp.height;
        out.channels = resp.channels;
        out.out_path.assign(
            reinterpret_cast<const char*>(rhead.data() + resp_bytes), path_len);
        out.model_name = bounded_cstr(resp.model_name, sizeof(resp.model_name));
        if (out.skipped) {
            out.message = "Skipped (already super-resolved)";
        }
        return out;
    }
}

}  // namespace upix
