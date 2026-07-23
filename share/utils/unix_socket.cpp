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
    : socket_(other.socket_)
    , path_(std::move(other.path_)) {
    // io_mutex_ is non-movable; default-constructed. Moved-from socket is left closed.
    other.socket_ = -1;
}

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        close();
        std::lock_guard<std::recursive_mutex> other_lock(other.io_mutex_);
        socket_ = other.socket_;
        path_ = std::move(other.path_);
        other.socket_ = -1;
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

auto UnixSocket::send(const void* data, size_t size) -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }
    ssize_t sent = ::send(socket_, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
        return std::unexpected(NetworkError{errno, "Send failed"});
    }
    return static_cast<size_t>(sent);
}

auto UnixSocket::send_all(const void* data, size_t size) -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    size_t total = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    while (total < size) {
        auto r = send(ptr + total, size - total);
        if (!r.has_value()) return std::unexpected(r.error());
        total += r.value();
    }
    return {};
}

auto UnixSocket::receive(void* buffer, size_t max_size) -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }
    ssize_t received = ::recv(socket_, buffer, max_size, 0);
    if (received < 0) {
        return std::unexpected(NetworkError{errno, "Receive failed"});
    }
    if (received == 0) {
        return std::unexpected(NetworkError{ECONNRESET, "Connection closed"});
    }
    return static_cast<size_t>(received);
}

auto UnixSocket::receive_all(void* buffer, size_t size) -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    size_t total = 0;
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    while (total < size) {
        auto r = receive(ptr + total, size - total);
        if (!r.has_value()) return std::unexpected(r.error());
        total += r.value();
    }
    return {};
}

void UnixSocket::set_timeout(int timeout_ms) {
    if (socket_ < 0) return;
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void UnixSocket::close() {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    path_.clear();
}

bool UnixSocket::is_connected() const {
    return socket_ >= 0;
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
