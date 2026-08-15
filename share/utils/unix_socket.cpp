#include "unix_socket.hpp"
#include "network.hpp"  // NetworkError

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

namespace crossscale {

UnixSocket::UnixSocket() = default;

UnixSocket::~UnixSocket() {
    close();
}

UnixSocket::UnixSocket(UnixSocket&& other) noexcept
    : StreamSocket(std::move(other))
    , path_(std::move(other.path_)) {
    // io_mutex_ is non-movable; default-constructed. Moved-from socket is left closed.
}

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        close();
        std::lock_guard<std::recursive_mutex> other_lock(other.io_mutex_);
        take_fd_from(other);
        path_ = std::move(other.path_);
    }
    return *this;
}

auto UnixSocket::connect(const std::string& path) -> expected<void, NetworkError> {
    close();

    if (path.empty()) {
        return std::unexpected(NetworkError{EINVAL, "Empty socket path"});
    }
    // sockaddr_un.sun_path is limited (typically 108 bytes on Linux). Reject
    // early with a clear message rather than failing silently in ::connect.
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::unexpected(NetworkError{ENAMETOOLONG,
            "Socket path too long (max " + std::to_string(sizeof(sockaddr_un::sun_path) - 1) + ")"});
    }

    socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{errno, "Failed to create socket"});
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        close();
        // ECONNREFUSED on a present-but-unlistened socket means a stale file
        // from a crashed daemon. Surface it distinctly so the client can treat
        // it as "no daemon" and fall back to the inline path.
        return std::unexpected(NetworkError{e, "Connection failed"});
    }

    path_ = path;
    return {};
}

void UnixSocket::close() {
    close_fd();
    path_.clear();
}

// ── UnixServer ──────────────────────────────────────────────────────────────

UnixServer::UnixServer() = default;

UnixServer::~UnixServer() {
    stop();
}

auto UnixServer::listen(const std::string& path, int backlog) -> expected<void, NetworkError> {
    stop();

    if (path.empty()) {
        return std::unexpected(NetworkError{EINVAL, "Empty socket path"});
    }
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::unexpected(NetworkError{ENAMETOOLONG,
            "Socket path too long (max " + std::to_string(sizeof(sockaddr_un::sun_path) - 1) + ")"});
    }

    server_socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        return std::unexpected(NetworkError{errno, "Failed to create socket"});
    }

    // Remove any stale socket file left by a crashed previous instance. Ignore
    // ENOENT; other unlink errors are fatal (the bind below would fail anyway).
    if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
        int e = errno;
        stop();
        return std::unexpected(NetworkError{e, "Failed to unlink stale socket"});
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        stop();
        return std::unexpected(NetworkError{e, "Bind failed"});
    }

    // Restrict to owner-only: local auth via filesystem permissions.
    if (::chmod(path.c_str(), 0600) < 0) {
        int e = errno;
        stop();
        return std::unexpected(NetworkError{e, "chmod failed"});
    }

    if (::listen(server_socket_, backlog) < 0) {
        int e = errno;
        stop();
        return std::unexpected(NetworkError{e, "Listen failed"});
    }

    path_ = path;
    return {};
}

auto UnixServer::accept() -> expected<std::unique_ptr<UnixSocket>, NetworkError> {
    if (server_socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Server not listening"});
    }
    // Poll with a short timeout so the accept loop can observe a shutdown.
    pollfd pfd{};
    pfd.fd = server_socket_;
    pfd.events = POLLIN;
    for (;;) {
        const int pr = ::poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(NetworkError{errno, "Poll failed"});
        }
        if (pr == 0) {
            // Timeout: signal caller (EAGAIN) so it can re-check running flag.
            return std::unexpected(NetworkError{EAGAIN, "Accept timed out"});
        }
        break;
    }

    int client = ::accept(server_socket_, nullptr, nullptr);
    if (client < 0) {
        return std::unexpected(NetworkError{errno, "Accept failed"});
    }

    auto sock = std::make_unique<UnixSocket>();
    sock->socket_ = client;
    sock->path_ = path_;
    return sock;
}

void UnixServer::stop() {
    if (server_socket_ >= 0) {
        ::close(server_socket_);
        server_socket_ = -1;
    }
    // Remove the socket file so the path is clean for the next instance and so
    // clients get an immediate ECONNREFUSED (treated as "no daemon").
    if (!path_.empty()) {
        ::unlink(path_.c_str());
    }
    path_.clear();
}

} // namespace crossscale
