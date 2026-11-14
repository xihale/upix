#include "local_socket.hpp"
#include "network.hpp"  // NetworkError

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <cstring>
#include <cerrno>

namespace upix {

LocalSocket::LocalSocket() = default;

LocalSocket::~LocalSocket() {
    close();
}

LocalSocket::LocalSocket(LocalSocket&& other) noexcept
    : StreamSocket(std::move(other))
    , path_(std::move(other.path_)) {
    // Direction locks are non-movable; default-constructed. Moved-from socket is left closed.
}

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

auto LocalSocket::connect(const std::string& path) -> expected<void, NetworkError> {
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

    sock_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_ < 0) {
        return std::unexpected(NetworkError{errno, "Failed to create socket"});
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
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

void LocalSocket::set_timeout(int timeout_ms) {
    if (sock_ < 0) return;
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

void LocalSocket::close() {
    close_fd();
    path_.clear();
}

auto LocalSocket::raw_send_some(const uint8_t* data, size_t size, size_t& sent)
    -> std::optional<NetworkError> {
    return fd_send_some(static_cast<int>(sock_), data, size, sent);
}

auto LocalSocket::raw_receive_some(uint8_t* buffer, size_t max_size,
                                   size_t& received)
    -> std::optional<NetworkError> {
    return fd_receive_some(static_cast<int>(sock_), buffer, max_size, received);
}

void LocalSocket::raw_wake() {
    ::shutdown(static_cast<int>(sock_), SHUT_RDWR);
}

void LocalSocket::raw_close() {
    ::close(static_cast<int>(sock_));
}

// ── LocalServer ─────────────────────────────────────────────────────────────

LocalServer::LocalServer() = default;

LocalServer::~LocalServer() {
    stop();
}

auto LocalServer::listen(const std::string& path, int backlog)
    -> expected<void, NetworkError> {
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

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // Bind first, without unlinking an existing socket file: that file may
    // belong to a live daemon, and blind unlink+bind would silently steal its
    // accept queue, leaving two "running" daemons where only one is reachable
    // (and both serve files with independent engines). Only remove the file
    // after a probe proves nothing live is behind it.
    auto try_bind = [&]() -> bool {
        return ::bind(server_socket_, reinterpret_cast<const sockaddr*>(&addr),
                      sizeof(addr)) == 0;
    };
    if (!try_bind()) {
        int e = errno;
        if (e != EADDRINUSE) {
            stop();
            return std::unexpected(NetworkError{e, "Bind failed"});
        }
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe < 0) {
            int pe = errno;
            stop();
            return std::unexpected(NetworkError{pe, "Probe socket failed"});
        }
        if (::connect(probe, reinterpret_cast<const sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            ::close(probe);
            stop();
            return std::unexpected(NetworkError{EADDRINUSE,
                "Socket already served by a live process"});
        }
        int pe = errno;
        ::close(probe);
        if (pe != ECONNREFUSED) {
            // Not connectable and not refused (EACCES, ENOENT race, ...) —
            // refuse to touch a file we cannot understand.
            stop();
            return std::unexpected(NetworkError{pe,
                "Socket path in use by unconnectable file"});
        }
        // ECONNREFUSED alone is not proof of a dead daemon: Linux reports it
        // for connect() to any existing non-socket file too. Only ever remove
        // the path when it really is (was) a socket — never a user file that
        // happens to sit on our path.
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0 && !S_ISSOCK(st.st_mode)) {
            stop();
            return std::unexpected(NetworkError{EEXIST,
                "Socket path exists but is not a socket file"});
        }
        // Stale file from a crashed instance: clean it up and retry once.
        if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
            int ue = errno;
            stop();
            return std::unexpected(NetworkError{ue, "Failed to unlink stale socket"});
        }
        if (!try_bind()) {
            e = errno;
            stop();
            return std::unexpected(NetworkError{e, "Bind failed"});
        }
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

auto LocalServer::accept() -> expected<std::unique_ptr<LocalSocket>, NetworkError> {
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
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    auto sock = std::make_unique<LocalSocket>();
    sock->sock_ = client;
    sock->path_ = path_;
    return sock;
}

void LocalServer::stop() {
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

// ── Watch handles ───────────────────────────────────────────────────────────

auto watch_dup(intptr_t handle) -> intptr_t {
    return ::dup(static_cast<int>(handle));
}

void watch_kill(intptr_t handle) {
    // shutdown() wakes a thread blocked in recv() with ECONNRESET; close()
    // alone (from another thread) is not guaranteed to wake it.
    ::shutdown(static_cast<int>(handle), SHUT_RDWR);
    ::close(static_cast<int>(handle));
}

void watch_close(intptr_t handle) {
    ::close(static_cast<int>(handle));
}

auto local_endpoint_exists(const std::string& path) -> bool {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

} // namespace upix

#endif  // !_WIN32
