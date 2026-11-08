#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <mutex>
#include "stream_socket.hpp"  // StreamSocket + NetworkError (via network_error.hpp)

namespace upix {

// Local stream endpoint for same-machine IPC between frontends and the serve
// daemon — AF_UNIX (filesystem-scoped, chmod 0600) on POSIX, a named pipe
// (\\.\pipe\<name>, PIPE_REJECT_REMOTE_CLIENTS) on Windows. Mirrors the
// TCPSocket/TCPServer surface so callers can swap between transports, and
// lets the serve daemon, its clients, and their tests share one endpoint
// concept per platform (docs/SPEC-frontends.md: stage 2 without a CLI).
//
// The `path` parameter is a filesystem path on POSIX. On Windows it is a
// pipe NAME — the transport prefixes `\\.\pipe\` when it is missing — and
// the kernel garbage-collects it when the last server handle closes, so the
// stale-socket choreography (bind/probe/unlink) is Windows-trivially safe.
// Framing I/O plus the handle and I/O lock live in StreamSocket.
class LocalSocket : public StreamSocket {
public:
    LocalSocket();
    ~LocalSocket();

    // No copy
    LocalSocket(const LocalSocket&) = delete;
    LocalSocket& operator=(const LocalSocket&) = delete;

    // Move
    LocalSocket(LocalSocket&& other) noexcept;
    LocalSocket& operator=(LocalSocket&& other) noexcept;

    // Connect to a server listening at the given path/pipe name.
    auto connect(const std::string& path) -> expected<void, NetworkError>;

    // Bound blocking reads and writes, matching the TCP socket API.
    void set_timeout(int timeout_ms);

    void close();

    const std::string& path() const { return path_; }

    // Teardown needs the raw handle (POSIX fd / Windows HANDLE) to wake a
    // connection thread blocked in recv()/ReadFile(). (Calling close() from
    // another thread would deadlock on the rx lock, which receive_all holds
    // across the blocking read.) Subclasses may read protected base members
    // through their own objects.
    [[nodiscard]] auto handle() const -> intptr_t { return sock_; }

private:
    friend class LocalServer;

    // StreamSocket raw primitives (see stream_socket.hpp); per-platform
    // implementation TUs (local_socket_posix.cpp / local_socket_win.cpp).
    auto raw_send_some(const uint8_t* data, size_t size, size_t& sent)
        -> std::optional<NetworkError> override;
    auto raw_receive_some(uint8_t* buffer, size_t max_size, size_t& received)
        -> std::optional<NetworkError> override;
    void raw_wake() override;
    void raw_close() override;

    // Deadline for raw reads (Windows pipes carry no SO_RCVTIMEO; the
    // overlapped wait uses this). POSIX sets SO_RCVTIMEO/SO_SNDTIMEO instead.
    int timeout_ms_ = 0;
    std::string path_;
};

// Local stream server.
class LocalServer {
public:
    LocalServer();
    ~LocalServer();

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    // Create, bind, and listen at path/pipe name. POSIX removes any stale
    // socket file first (never a live daemon's — bind-probe-verify), then
    // chmod 0600 so only the owning user can connect. Windows fails with
    // EADDRINUSE when a live server already owns the pipe name
    // (FILE_FLAG_FIRST_PIPE_INSTANCE).
    auto listen(const std::string& path, int backlog = 16)
        -> expected<void, NetworkError>;

    // Blocks up to ~500 ms so the accept loop can wake to observe a shutdown
    // request (returns EAGAIN on the timeout, like the TCP server poll).
    auto accept() -> expected<std::unique_ptr<LocalSocket>, NetworkError>;

    void stop();
    const std::string& path() const { return path_; }

private:
#ifdef _WIN32
    intptr_t server_handle_ = -1;
    // Signalled by stop(): aborts a pending overlapped ConnectNamedPipe.
    void* stop_event_ = nullptr;
#else
    int server_socket_ = -1;
#endif
    std::string path_;
};

// ── Watch handles (teardown helpers shared by the serve daemon) ────────────
// A connection's serving thread can block in receive_all() while teardown
// must interrupt it. The daemon dups the connection handle (`watch_dup`),
// stashes the copy in its thread record, and later `watch_kill`s the copy —
// POSIX: shutdown()+close; Windows: CancelIoEx+CloseHandle. `watch_close`
// releases a copy without waking (already-dead connection).

auto watch_dup(intptr_t handle) -> intptr_t;
void watch_kill(intptr_t handle);
void watch_close(intptr_t handle);

// Does the endpoint exist (socket file / named pipe instance)?
auto local_endpoint_exists(const std::string& path) -> bool;

} // namespace upix
