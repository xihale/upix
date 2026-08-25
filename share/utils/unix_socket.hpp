#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <mutex>
#include "stream_socket.hpp"  // StreamSocket + NetworkError (via network_error.hpp)

namespace upix {

// AF_UNIX stream socket — local-only, no network stack, filesystem-permission
// scoped. Mirrors the TCPSocket/TCPServer surface so callers can swap between
// them. Available on Linux/macOS; not compiled on platforms without AF_UNIX
// (the daemon subcommand gates on this at the CLI layer). Framing I/O plus
// the fd and I/O lock live in StreamSocket.
class UnixSocket : public StreamSocket {
public:
    UnixSocket();
    ~UnixSocket();

    // No copy
    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;

    // Move
    UnixSocket(UnixSocket&& other) noexcept;
    UnixSocket& operator=(UnixSocket&& other) noexcept;

    // Connect to a server listening at the given filesystem path.
    auto connect(const std::string& path) -> expected<void, NetworkError>;

    void close();

    const std::string& path() const { return path_; }

private:
    friend class UnixServer;

    std::string path_;
};

// AF_UNIX stream server.
class UnixServer {
public:
    UnixServer();
    ~UnixServer();

    UnixServer(const UnixServer&) = delete;
    UnixServer& operator=(const UnixServer&) = delete;

    // Create, bind, and listen at path. Removes any stale socket file first,
    // then chmod 0600 so only the owning user can connect.
    auto listen(const std::string& path, int backlog = 16)
        -> expected<void, NetworkError>;

    // Blocks until a client connects. Polls with a 500ms timeout so the accept
    // loop can wake to observe a shutdown request.
    auto accept() -> expected<std::unique_ptr<UnixSocket>, NetworkError>;

    void stop();
    const std::string& path() const { return path_; }

private:
    int server_socket_ = -1;
    std::string path_;
};

} // namespace upix
