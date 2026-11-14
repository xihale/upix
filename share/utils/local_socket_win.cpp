#include "local_socket.hpp"
#include "network.hpp"  // NetworkError

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <string>

namespace upix {
namespace {

// The `path` parameter is a pipe NAME (no prefix); accept a full
// \\.\pipe\... too so callers can pass whatever default_serve_socket_path()
// handed them without platform branching at the call site.
auto pipe_name(const std::string& path) -> std::wstring {
    std::string full = path;
    if (full.rfind("\\\\.\\pipe\\", 0) != 0) {
        full = "\\\\.\\pipe\\" + full;
    }
    return std::wstring(full.begin(), full.end());  // ASCII names only
}

auto last_error_code() -> int {
    return static_cast<int>(GetLastError());
}

// Map Windows errors to the errno-flavored codes the rest of the codebase
// branches on (ECONNREFUSED = "no daemon", EAGAIN = "try again", EADDRINUSE,
// ECONNRESET = peer closed / aborted op).
auto map_error(int werr) -> int {
    switch (werr) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_CANNOT_MAKE:
            return ECONNREFUSED;  // no pipe instance = "no daemon"
        case ERROR_PIPE_BUSY:
        case ERROR_SEM_TIMEOUT:
        case WAIT_TIMEOUT:
            return EAGAIN;
        case ERROR_ACCESS_DENIED:
            return EADDRINUSE;  // live server owns the name (first-instance)
        case ERROR_OPERATION_ABORTED:
        case ERROR_CANCELLED:
            return ECANCELED;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
            return ECONNRESET;
        default:
            return EIO;
    }
}

}  // namespace

LocalSocket::LocalSocket() = default;

LocalSocket::~LocalSocket() {
    close();
}

LocalSocket::LocalSocket(LocalSocket&& other) noexcept
    : StreamSocket(std::move(other))
    , path_(std::move(other.path_)) {}

LocalSocket& LocalSocket::operator=(LocalSocket&& other) noexcept {
    if (this != &other) {
        close();
        std::lock_guard<std::recursive_mutex> other_tx(other.tx_mutex_);
        std::lock_guard<std::recursive_mutex> other_rx(other.rx_mutex_);
        take_handle_from(other);
        path_ = std::move(other.path_);
    }
    return *this;
}

auto LocalSocket::connect(const std::string& path)
    -> expected<void, NetworkError> {
    close();

    if (path.empty()) {
        return std::unexpected(NetworkError{EINVAL, "Empty pipe name"});
    }
    const std::wstring name = pipe_name(path);

    // ERROR_PIPE_BUSY means a live server with all instances occupied:
    // wait briefly for a free instance, then connect. ERROR_FILE_NOT_FOUND
    // maps to ECONNREFUSED (treated as "no daemon" upstream).
    HANDLE h = nullptr;
    for (int attempt = 0; attempt < 10; ++attempt) {
        // Must open with FILE_FLAG_OVERLAPPED because raw_send_some / raw_receive_some
        // issue overlapped I/O so CancelIoEx and read deadlines work reliably.
        h = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                          0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                          nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        const int werr = last_error_code();
        if (werr != ERROR_PIPE_BUSY) {
            close();
            return std::unexpected(NetworkError{map_error(werr),
                                                "Connect to pipe failed"});
        }
        if (!::WaitNamedPipeW(name.c_str(), 500)) {
            const int werr2 = last_error_code();
            close();
            return std::unexpected(NetworkError{map_error(werr2),
                                                "Pipe busy and stayed busy"});
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        close();
        return std::unexpected(NetworkError{EAGAIN, "Pipe stayed busy"});
    }

    sock_ = reinterpret_cast<intptr_t>(h);
    // Byte-mode reads/writes (a pipe created with PIPE_TYPE_BYTE by the
    // server); GetNamedPipeHandleState would confirm, but the daemon owns
    // the mode — request nothing here.
    path_ = path;
    return {};
}

void LocalSocket::set_timeout(int timeout_ms) {
    // Windows pipes carry no SO_RCVTIMEO: the overlapped read wait uses this
    // deadline (defaulting to the serve protocol's 60 s I/O bound).
    timeout_ms_ = timeout_ms;
}

void LocalSocket::close() {
    close_fd();
    path_.clear();
}

// Overlapped one-syscall write. Deadlines: the framing layer holds no
// timeout state on Windows pipes (set_timeout is advisory only); a send of a
// framed response completes or fails — blocking forever is the TCP-parity
// behavior for a peer that stalled mid-request.
auto LocalSocket::raw_send_some(const uint8_t* data, size_t size, size_t& sent)
    -> std::optional<NetworkError> {
    HANDLE h = reinterpret_cast<HANDLE>(sock_);
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return NetworkError{EIO, "Event creation failed"};
    }
    DWORD written = 0;
    if (!::WriteFile(h, data, static_cast<DWORD>(size), nullptr, &ov)) {
        const int werr = last_error_code();
        if (werr == ERROR_IO_PENDING) {
            if (!::GetOverlappedResult(h, &ov, &written, TRUE)) {
                ::CloseHandle(ov.hEvent);
                return NetworkError{map_error(last_error_code()),
                                    "Write failed"};
            }
        } else {
            ::CloseHandle(ov.hEvent);
            return NetworkError{map_error(werr), "Write failed"};
        }
    } else if (!::GetOverlappedResult(h, &ov, &written, FALSE)) {
        ::CloseHandle(ov.hEvent);
        return NetworkError{map_error(last_error_code()), "Write failed"};
    }
    ::CloseHandle(ov.hEvent);
    sent = written;
    return std::nullopt;
}

// Overlapped read with the SO_RCVTIMEO parity: the daemon's request reader
// relies on bound reads (LocalSocket::set_timeout) so an unresponsive client
// cannot pin a connection thread forever. Wait the full timeout, then cancel.
auto LocalSocket::raw_receive_some(uint8_t* buffer, size_t max_size,
                                   size_t& received)
    -> std::optional<NetworkError> {
    HANDLE h = reinterpret_cast<HANDLE>(sock_);
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return NetworkError{EIO, "Event creation failed"};
    }
    DWORD got = 0;
    if (::ReadFile(h, buffer, static_cast<DWORD>(max_size), nullptr, &ov)) {
        if (!::GetOverlappedResult(h, &ov, &got, FALSE)) {
            const int werr = last_error_code();
            ::CloseHandle(ov.hEvent);
            return NetworkError{map_error(werr), "Read failed"};
        }
        ::CloseHandle(ov.hEvent);
        received = got;
        return std::nullopt;
    }
    const int werr = last_error_code();
    if (werr != ERROR_IO_PENDING) {
        ::CloseHandle(ov.hEvent);
        return NetworkError{map_error(werr), "Read failed"};
    }
    // Bound wait: the timeout lives in the socket's set_timeout contract.
    // Pipes don't carry SO_RCVTIMEO, so we hold it here (default 60 s, the
    // same bound the TCP serve client applies). The value arrives via the
    // timeout_ms_ set by set_timeout — but that is per-object state the base
    // class does not store, so LocalSocket keeps its own copy.
    const DWORD wait_ms = static_cast<DWORD>(timeout_ms_ > 0 ? timeout_ms_ : 60000);
    const DWORD w = ::WaitForSingleObject(ov.hEvent, wait_ms);
    if (w == WAIT_TIMEOUT) {
        // Deadline hit: cancel just this operation, then absorb its completion.
        ::CancelIoEx(h, &ov);
        DWORD ignored = 0;
        ::GetOverlappedResult(h, &ov, &ignored, TRUE);
        ::CloseHandle(ov.hEvent);
        return NetworkError{EAGAIN, "Read timed out"};
    }
    if (w != WAIT_OBJECT_0) {
        ::CloseHandle(ov.hEvent);
        return NetworkError{EIO, "Wait failed"};
    }
    if (!::GetOverlappedResult(h, &ov, &got, TRUE)) {
        const int werr = last_error_code();
        ::CloseHandle(ov.hEvent);
        // CancelIoEx from shutdown_wake lands here (ERROR_OPERATION_ABORTED).
        return NetworkError{map_error(werr), "Read failed"};
    }
    ::CloseHandle(ov.hEvent);
    received = got;
    return std::nullopt;
}

void LocalSocket::raw_wake() {
    // Interrupt a blocked ReadFile/WriteFile from another thread.
    ::CancelIoEx(reinterpret_cast<HANDLE>(sock_), nullptr);
}

void LocalSocket::raw_close() {
    ::CloseHandle(reinterpret_cast<HANDLE>(sock_));
}

// ── LocalServer ─────────────────────────────────────────────────────────────

LocalServer::LocalServer() = default;

LocalServer::~LocalServer() {
    stop();
}

auto LocalServer::listen(const std::string& path, int backlog)
    -> expected<void, NetworkError> {
    (void)backlog;
    stop();

    if (path.empty()) {
        return std::unexpected(NetworkError{EINVAL, "Empty pipe name"});
    }
    const std::wstring name = pipe_name(path);

    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        return std::unexpected(NetworkError{EIO, "Event creation failed"});
    }

    // FILE_FLAG_FIRST_PIPE_INSTANCE fails with ERROR_ACCESS_DENIED when
    // another live server already owns the name — the bind-first guard's
    // Windows equivalent (no stale files exist: the kernel removes the name
    // with the last handle). PIPE_REJECT_REMOTE_CLIENTS keeps the endpoint
    // local-only (the POSIX chmod 0600 parity).
    HANDLE h = ::CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        16,  // instances — the listen-backlog parity; extra clients get ERROR_PIPE_BUSY and wait
        4 * 1024 * 1024,  // out (server writes: responses / payloads)
        4 * 1024 * 1024,  // in  (server reads: requests)
        0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const int werr = last_error_code();
        stop();
        return std::unexpected(NetworkError{map_error(werr),
                                            werr == ERROR_ACCESS_DENIED
                                                ? "Pipe already served by a live process"
                                                : "CreateNamedPipe failed"});
    }

    server_handle_ = reinterpret_cast<intptr_t>(h);
    path_ = path;
    return {};
}

auto LocalServer::accept()
    -> expected<std::unique_ptr<LocalSocket>, NetworkError> {
    if (server_handle_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Server not listening"});
    }
    HANDLE h = reinterpret_cast<HANDLE>(server_handle_);

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return std::unexpected(NetworkError{EIO, "Event creation failed"});
    }
    // ConnectNamedPipe completes when a client connects; the overlapped wait
    // gives the accept loop its 500 ms poll tick (stop() sets the event and
    // closes the server handle, which aborts the pending connect).
    const BOOL connected = ::ConnectNamedPipe(h, &ov);
    int werr = last_error_code();
    if (!connected && werr == ERROR_PIPE_CONNECTED) {
        // A client connected between CreateNamedPipe and ConnectNamedPipe.
    } else if (!connected && werr == ERROR_IO_PENDING) {
        HANDLE waits[2] = {ov.hEvent, stop_event_};
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w == WAIT_TIMEOUT) {
            // Tick: release the loop. Cancel the pending connect, then absorb
            // its completion — if a client raced in during the cancel, the
            // overlapped result SUCCEEDS and the connection is good; dropping
            // a just-connected client would leave it hanging until timeout.
            ::CancelIoEx(h, &ov);
            DWORD ignored = 0;
            if (::GetOverlappedResult(h, &ov, &ignored, TRUE)) {
                // fallthrough: connected
            } else {
                ::CloseHandle(ov.hEvent);
                return std::unexpected(NetworkError{EAGAIN, "Accept timed out"});
            }
        } else if (w != WAIT_OBJECT_0) {
            // stop_event_ fired (or the wait errored): absorb and unwind.
            ::CancelIoEx(h, &ov);
            DWORD ignored = 0;
            ::GetOverlappedResult(h, &ov, &ignored, TRUE);
            ::CloseHandle(ov.hEvent);
            return std::unexpected(NetworkError{ECANCELED, "Accept cancelled"});
        }
    } else if (!connected) {
        ::CloseHandle(ov.hEvent);
        return std::unexpected(NetworkError{map_error(werr), "Accept failed"});
    }
    ::CloseHandle(ov.hEvent);

    // Hand the connected instance to the client socket, then rebuild the
    // listening instance — on Windows every CreateNamedPipe handle IS one
    // instance; the server must keep a fresh one or no further client can
    // connect. No FILE_FLAG_FIRST_PIPE_INSTANCE here: the accepted instance
    // (now owned by the client socket) keeps the name alive, and "first" is
    // only the live-server guard in listen(). A failed rebuild
    // (pathological) retires the listener: the next accept reports EBADF and
    // the daemon exits through its usual path.
    server_handle_ = -1;
    HANDLE next = ::CreateNamedPipeW(
        pipe_name(path_).c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        16,
        4 * 1024 * 1024,
        4 * 1024 * 1024,
        0, nullptr);
    if (next != INVALID_HANDLE_VALUE) {
        server_handle_ = reinterpret_cast<intptr_t>(next);
    }

    auto sock = std::make_unique<LocalSocket>();
    sock->sock_ = reinterpret_cast<intptr_t>(h);
    sock->path_ = path_;
    return sock;
}

void LocalServer::stop() {
    if (server_handle_ >= 0) {
        if (stop_event_) ::SetEvent(stop_event_);
        ::CloseHandle(reinterpret_cast<HANDLE>(server_handle_));
        server_handle_ = -1;
        // Pending ConnectNamedPipe operations abort with ERROR_OPERATION_
        // ABORTED; their client sockets die with the closed server handle.
    }
    if (stop_event_) {
        ::CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    // No filesystem cleanup: pipe names vanish with the last handle.
    path_.clear();
}

// ── Watch handles ───────────────────────────────────────────────────────────

auto watch_dup(intptr_t handle) -> intptr_t {
    HANDLE dup_out = nullptr;
    (void)::DuplicateHandle(::GetCurrentProcess(),
                            reinterpret_cast<HANDLE>(handle),
                            ::GetCurrentProcess(), &dup_out, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
    return dup_out ? reinterpret_cast<intptr_t>(dup_out) : -1;
}

void watch_kill(intptr_t handle) {
    // Cancel ALL pending I/O on the connection (the serving thread may be
    // blocked in ReadFile), then release the dup.
    (void)::CancelIoEx(reinterpret_cast<HANDLE>(handle), nullptr);
    ::CloseHandle(reinterpret_cast<HANDLE>(handle));
}

void watch_close(intptr_t handle) {
    ::CloseHandle(reinterpret_cast<HANDLE>(handle));
}

auto local_endpoint_exists(const std::string& path) -> bool {
    // WaitNamedPipe with a 1 ms window: nonzero when an instance exists
    // (busy counts as existing); ERROR_FILE_NOT_FOUND when the name is gone.
    const std::wstring name = pipe_name(path);
    if (!::WaitNamedPipeW(name.c_str(), 1)) {
        return last_error_code() != ERROR_FILE_NOT_FOUND;
    }
    return true;
}

} // namespace upix

#endif  // _WIN32
