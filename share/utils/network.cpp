#include "network.hpp"
#include "logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include <zlib.h>

namespace upix {

namespace {
// Enable TCP keepalive with aggressive intervals so long-lived request/response
// connections (phone -> PC distributed upscale) survive NAT / stateful firewall
// idle eviction during the long, traffic-free processing phase. Without this, a
// NAT that drops an idle connection (~30-60s on many home/Wi-Fi routers) silently
// kills the link while the PC is still upscaling; the PC's eventual response send
// then fails (looks "stuck" until SO_SNDTIMEO) and the phone reports the PC
// unreachable and falls back to native.
//   idle=10s before first probe (under typical NAT timeouts), intvl=3s, count=5
void apply_tcp_keepalive(int fd) {
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
#ifdef TCP_KEEPIDLE
    int idle = 10;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int interval = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
    int count = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}
}  // namespace

TCPSocket::TCPSocket() = default;

TCPSocket::~TCPSocket() {
    close();
}

TCPSocket::TCPSocket(TCPSocket&& other) noexcept
    : StreamSocket(std::move(other))
    , peer_address_(std::move(other.peer_address_))
    , peer_port_(other.peer_port_) {
    // Note: io_mutex_ is default-constructed (mutexes are non-movable).
    // The moved-from socket is left in a closed state.
    other.peer_port_ = 0;
}

TCPSocket& TCPSocket::operator=(TCPSocket&& other) noexcept {
    if (this != &other) {
        close();
        // io_mutex_ intentionally not moved; lock other's mutex to read its state.
        std::lock_guard<std::recursive_mutex> other_lock(other.io_mutex_);
        take_fd_from(other);
        peer_address_ = std::move(other.peer_address_);
        peer_port_ = other.peer_port_;
        other.peer_port_ = 0;
    }
    return *this;
}

auto TCPSocket::connect(const std::string& host, uint16_t port)
    -> expected<void, NetworkError> {
    close();
    
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{errno, "Failed to create socket"});
    }

    int flag = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set default socket buffer size (1MB) for better network performance
    int buf_size = 1 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    apply_tcp_keepalive(socket_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close();
        return std::unexpected(NetworkError{errno, "Invalid address"});
    }

    if (::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return std::unexpected(NetworkError{errno, "Connection failed"});
    }

    peer_address_ = host;
    peer_port_ = port;
    return {};
}

bool TCPSocket::connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms) {
    close();

    // AF_UNSPEC + AI_ADDRCONFIG: resolve to every family configured on this
    // host, so IPv6 literals and DNS names work, unlike the inet_pton(AF_INET)
    // path in connect(). Numeric literals are unaffected by AI_ADDRCONFIG.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
#ifdef AI_ADDRCONFIG
    hints.ai_flags = AI_ADDRCONFIG;
#endif

    addrinfo* candidates = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &candidates) != 0 || candidates == nullptr) {
        return false;
    }

    // One deadline shared by all candidate addresses so a host with many
    // A/AAAA records cannot multiply the timeout.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    for (addrinfo* ai = candidates; ai != nullptr; ai = ai->ai_next) {
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) {
            break;
        }

        sockaddr_storage addr{};
        if (ai->ai_addrlen > sizeof(addr)) {
            continue;
        }
        std::memcpy(&addr, ai->ai_addr, ai->ai_addrlen);
        if (ai->ai_family == AF_INET) {
            reinterpret_cast<sockaddr_in*>(&addr)->sin_port = htons(port);
        } else if (ai->ai_family == AF_INET6) {
            reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port = htons(port);
        } else {
            continue;
        }

        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        // Non-blocking for the connect phase only, so poll() can bound it.
        const int saved_flags = ::fcntl(fd, F_GETFL, 0);
        if (saved_flags >= 0) {
            ::fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK);
        }

        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), ai->ai_addrlen);
        bool connected = false;
        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            for (;;) {
                const int pr = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (pr == 0) {
                    break;  // timed out
                }
                int so_error = 0;
                socklen_t optlen = sizeof(so_error);
                connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                         &so_error, &optlen) == 0 && so_error == 0;
                break;
            }
        }

        if (connected) {
            if (saved_flags >= 0) {
                ::fcntl(fd, F_SETFL, saved_flags);  // restore blocking mode
            }
            ::freeaddrinfo(candidates);

            socket_ = fd;
            int flag = 1;
            setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            int buf_size = 1 * 1024 * 1024;
            setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
            setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
            apply_tcp_keepalive(socket_);
            peer_address_ = host;
            peer_port_ = port;
            return true;
        }
        ::close(fd);
    }

    ::freeaddrinfo(candidates);
    return false;
}

void TCPSocket::set_timeout(int timeout_ms) {
    if (socket_ < 0) return;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void TCPSocket::set_recv_timeout(int timeout_ms) {
    if (socket_ < 0) return;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void TCPSocket::close() {
    close_fd();
}

TCPServer::TCPServer() = default;

TCPServer::~TCPServer() {
    stop();
}

auto TCPServer::listen(uint16_t port, int backlog)
    -> expected<void, NetworkError> {
    stop();

    server_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        return std::unexpected(NetworkError{errno, "Failed to create socket"});
    }

    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(server_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        stop();
        return std::unexpected(NetworkError{errno, "Bind failed"});
    }

    if (::listen(server_socket_, backlog) < 0) {
        stop();
        return std::unexpected(NetworkError{errno, "Listen failed"});
    }

    port_ = port;
    return {};
}

auto TCPServer::accept()
    -> expected<std::unique_ptr<TCPSocket>, NetworkError> {
    if (server_socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Server not listening"});
    }

    // Poll with a 500ms timeout so the accept loop can wake up periodically
    // and observe a shutdown (running_ == false). A bare ::accept() would
    // block forever and close() from another thread does not reliably wake
    // it on Linux, hanging the process on Ctrl+C.
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
            // Timeout: no client. Signal the caller via EAGAIN so it can
            // re-check the running flag and loop back if still alive.
            return std::unexpected(NetworkError{EAGAIN, "Accept timed out"});
        }
        break;
    }

    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    int client_socket = ::accept(server_socket_,
                                  reinterpret_cast<sockaddr*>(&client_addr),
                                  &addr_len);
    if (client_socket < 0) {
        return std::unexpected(NetworkError{errno, "Accept failed"});
    }

    auto socket = std::make_unique<TCPSocket>();
    socket->socket_ = client_socket;
    socket->peer_address_ = std::string(INET_ADDRSTRLEN, '\0');
    inet_ntop(AF_INET, &client_addr.sin_addr,
              socket->peer_address_.data(), INET_ADDRSTRLEN);
    socket->peer_address_.resize(std::strlen(socket->peer_address_.c_str()));
    socket->peer_port_ = ntohs(client_addr.sin_port);

    int flag = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set default socket buffer size (1MB) for better network performance
    int buf_size = 1 * 1024 * 1024;
    setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(client_socket, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    apply_tcp_keepalive(client_socket);

    return socket;
}

void TCPServer::stop() {
    if (server_socket_ >= 0) {
        ::close(server_socket_);
        server_socket_ = -1;
    }
    port_ = 0;
}

namespace network_utils {

std::vector<std::string> get_local_addresses() {
    std::vector<std::string> addresses;
    
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, 
                         &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr,
                         buf, sizeof(buf));
                std::string name = p->ifa_name;
                if (name != "lo" && name.find("docker") == std::string::npos) {
                    addresses.push_back(buf);
                }
            }
        }
        freeifaddrs(ifa);
    }
    
    return addresses;
}

// Standard CRC-32 (IEEE 802.3) — identical to the former hand-rolled table.
// zlib ships this exact algorithm and is already linked (transitively via libpng).
uint32_t crc32(const void* data, size_t size) {
    return ::crc32(0L, static_cast<const Bytef*>(data), static_cast<uInt>(size));
}

} // namespace network_utils

} // namespace upix
