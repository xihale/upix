#include "network.hpp"
#include "logger.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#endif
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include <zlib.h>

namespace upix {

#ifdef _WIN32
namespace {

// WinSock lifecycle: one WSAStartup per process, refcount-safe to call from
// any connect/listen path. Windows sockets need it; POSIX is a no-op call.
std::once_flag g_wsa_once;
void ensure_winsock() {
    std::call_once(g_wsa_once, [] {
        WSADATA data{};
        (void)::WSAStartup(MAKEWORD(2, 2), &data);
    });
}

using sock_t = SOCKET;
constexpr sock_t kInvalidSock = INVALID_SOCKET;

auto sock_close(sock_t s) { ::closesocket(s); }

// Map a WSA error to the errno-flavored code NetworkError carries (callers
// branch on EAGAIN / ECONNRESET / EADDRINUSE / ECONNREFUSED / ETIMEDOUT).
auto sock_err() -> int {
    const int w = ::WSAGetLastError();
    switch (w) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEINTR: return EINTR;
        case WSAEBADF: case WSAENOTSOCK: return EBADF;
        case WSAECONNRESET: case WSAECONNABORTED: return ECONNRESET;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEACCES: return EACCES;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAENETUNREACH: case WSAENETDOWN: return ENETUNREACH;
        case WSAEINVAL: return EINVAL;
        case WSAEMSGSIZE: return EMSGSIZE;
        case WSAENOTCONN: case WSAESHUTDOWN: return ENOTCONN;
        default: return EIO;
    }
}

// poll() parity (Vista+). WSAPOLLFD mirrors pollfd closely enough for the
// accept loops and the connect deadline.
auto sock_poll(pollfd* fds, int n, int timeout_ms) -> int {
    return ::WSAPoll(fds, static_cast<ULONG>(n), timeout_ms);
}

// Aggressive keepalive parity with the POSIX TCP_KEEPIDLE/INTVL/CNT tuning:
// SIO_KEEPALIVE_VALS carries only (onoff, time_ms, interval_ms) — no probe
// count, Vista+ default count applies. idle=10s, interval=3s.
void apply_tcp_keepalive(sock_t s) {
    BOOL enabled = TRUE;
    (void)::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    tcp_keepalive ka{};
    ka.onoff = 1;
    ka.keepalivetime = 10'000;
    ka.keepaliveinterval = 3'000;
    DWORD ret = 0;
    (void)::WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0,
                     &ret, nullptr, nullptr);
}

#else  // POSIX

namespace {

using sock_t = int;
constexpr sock_t kInvalidSock = -1;

auto sock_close(sock_t s) { ::close(s); }
auto sock_err() -> int { return errno; }
auto sock_poll(pollfd* fds, int n, int timeout_ms) -> int {
    return ::poll(fds, static_cast<nfds_t>(n), timeout_ms);
}

void apply_tcp_keepalive(sock_t s) {
    int enabled = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
#ifdef TCP_KEEPIDLE
    int idle = 10;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int interval = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
    int count = 5;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

// ── Shared socket-option setup (TCP_NODELAY + 1 MB buffers + keepalive) ────
void tune_connected_sock(sock_t s) {
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));
    int buf_size = 1 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    apply_tcp_keepalive(s);
}

}  // namespace
#endif  // platform split (WinSock vs POSIX)

// ── Raw primitives (StreamSocket contract) ─────────────────────────────────

#ifdef _WIN32

auto TCPSocket::raw_send_some(const uint8_t* data, size_t size, size_t& sent)
    -> std::optional<NetworkError> {
    const int n = ::send(static_cast<SOCKET>(sock_),
                         reinterpret_cast<const char*>(data),
                         static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) {
        return NetworkError{sock_err(), "Send failed"};
    }
    sent = static_cast<size_t>(n);
    return std::nullopt;
}

auto TCPSocket::raw_receive_some(uint8_t* buffer, size_t max_size,
                                 size_t& received)
    -> std::optional<NetworkError> {
    const int n = ::recv(static_cast<SOCKET>(sock_),
                         reinterpret_cast<char*>(buffer),
                         static_cast<int>(max_size), 0);
    if (n == SOCKET_ERROR) {
        return NetworkError{sock_err(), "Receive failed"};
    }
    received = static_cast<size_t>(n);
    return std::nullopt;
}

void TCPSocket::raw_wake() {
    (void)::shutdown(static_cast<SOCKET>(sock_), SD_BOTH);
}

void TCPSocket::raw_close() {
    sock_close(static_cast<SOCKET>(sock_));
}

#else

auto TCPSocket::raw_send_some(const uint8_t* data, size_t size, size_t& sent)
    -> std::optional<NetworkError> {
    return fd_send_some(static_cast<int>(sock_), data, size, sent);
}

auto TCPSocket::raw_receive_some(uint8_t* buffer, size_t max_size,
                                 size_t& received)
    -> std::optional<NetworkError> {
    return fd_receive_some(static_cast<int>(sock_), buffer, max_size, received);
}

void TCPSocket::raw_wake() {
    ::shutdown(static_cast<int>(sock_), SHUT_RDWR);
}

void TCPSocket::raw_close() {
    ::close(static_cast<int>(sock_));
}

#endif

TCPSocket::TCPSocket() = default;

TCPSocket::~TCPSocket() {
    close();
}

TCPSocket::TCPSocket(TCPSocket&& other) noexcept
    : StreamSocket(std::move(other))
    , peer_address_(std::move(other.peer_address_))
    , peer_port_(other.peer_port_) {
    // Note: tx_mutex_/rx_mutex_ are default-constructed (mutexes are non-movable).
    // The moved-from socket is left in a closed state.
    other.peer_port_ = 0;
}

TCPSocket& TCPSocket::operator=(TCPSocket&& other) noexcept {
    if (this != &other) {
        close();
        // Direction locks intentionally not moved; lock other's mutexes to read its state.
        std::lock_guard<std::recursive_mutex> other_tx(other.tx_mutex_);
        std::lock_guard<std::recursive_mutex> other_rx(other.rx_mutex_);
        take_handle_from(other);
        peer_address_ = std::move(other.peer_address_);
        peer_port_ = other.peer_port_;
        other.peer_port_ = 0;
    }
    return *this;
}

auto TCPSocket::connect(const std::string& host, uint16_t port)
    -> expected<void, NetworkError> {
    close();
#ifdef _WIN32
    ensure_winsock();
#endif

    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSock) {
        return std::unexpected(NetworkError{sock_err(), "Failed to create socket"});
    }

    tune_connected_sock(s);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        sock_close(s);
        return std::unexpected(NetworkError{sock_err(), "Invalid address"});
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int e = sock_err();
        sock_close(s);
        return std::unexpected(NetworkError{e, "Connection failed"});
    }

    sock_ = static_cast<intptr_t>(s);
    peer_address_ = host;
    peer_port_ = port;
    return {};
}

bool TCPSocket::connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms) {
    close();
#ifdef _WIN32
    ensure_winsock();
#endif

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

        sock_t fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kInvalidSock) {
            continue;
        }

        // Non-blocking for the connect phase only, so the poll can bound it.
#ifdef _WIN32
            u_long nonblocking = 1;
            const bool flags_saved = ::ioctlsocket(fd, FIONBIO, &nonblocking) == 0;
#else
        const int saved_flags = ::fcntl(fd, F_GETFL, 0);
        const bool flags_saved = saved_flags >= 0;
        if (flags_saved) {
            ::fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK);
        }
#endif

        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), ai->ai_addrlen);
        bool connected = rc == 0;
        if (!connected) {
            const int e = sock_err();
#ifdef _WIN32
            if (e == EINPROGRESS || e == EAGAIN) {
#else
            if (e == EINPROGRESS) {
#endif
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLOUT;
                for (;;) {
                    const int pr = sock_poll(&pfd, 1, static_cast<int>(remaining_ms));
                    if (pr < 0) {
                        if (sock_err() == EINTR) continue;
                        break;
                    }
                    if (pr == 0) {
                        break;  // timed out
                    }
                    int so_error = 0;
                    socklen_t optlen = sizeof(so_error);
                    connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                             reinterpret_cast<char*>(&so_error),
                                             &optlen) == 0 && so_error == 0;
                    break;
                }
            }
        }

        if (connected) {
#ifdef _WIN32
            u_long blocking = 0;
            if (flags_saved) ::ioctlsocket(fd, FIONBIO, &blocking);
#else
            if (flags_saved) {
                ::fcntl(fd, F_SETFL, saved_flags);  // restore blocking mode
            }
#endif
            ::freeaddrinfo(candidates);

            sock_ = static_cast<intptr_t>(fd);
            tune_connected_sock(sock_);
            peer_address_ = host;
            peer_port_ = port;
            return true;
        }
        sock_close(fd);
    }

    ::freeaddrinfo(candidates);
    return false;
}

void TCPSocket::set_timeout(int timeout_ms) {
    if (sock_ < 0) return;
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeout_ms);
    setsockopt(static_cast<SOCKET>(sock_), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(static_cast<SOCKET>(sock_), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void TCPSocket::set_recv_timeout(int timeout_ms) {
    if (sock_ < 0) return;
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeout_ms);
    setsockopt(static_cast<SOCKET>(sock_), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void TCPSocket::close() {
    close_fd();
}

void TCPSocket::shutdown() {
    // SHUT_RDWR parity, never close(): the handle stays owned by this object
    // (a recycled number must never be reachable through a live TCPSocket),
    // while every blocking recv/send on it returns immediately. Deliberately
    // lock-free w.r.t. tx_mutex_/rx_mutex_ — see the header contract.
    if (sock_ >= 0) {
        raw_wake();
    }
}

TCPServer::TCPServer() = default;

TCPServer::~TCPServer() {
    stop();
}

auto TCPServer::listen(uint16_t port, int backlog)
    -> expected<void, NetworkError> {
    stop();
#ifdef _WIN32
    ensure_winsock();
#endif

    sock_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSock) {
        return std::unexpected(NetworkError{sock_err(), "Failed to create socket"});
    }

#ifdef _WIN32
    // SO_REUSEADDR on Windows allows hijacking a port another socket is
    // BOUND to (not merely TIME_WAIT); skip it — fresh listeners rarely
    // race TIME_WAIT for an ephemeral coordinator port, and refusal is the
    // safer failure.
#else
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int e = sock_err();
        sock_close(fd);
        return std::unexpected(NetworkError{e, "Bind failed"});
    }

    if (::listen(fd, backlog) < 0) {
        const int e = sock_err();
        sock_close(fd);
        return std::unexpected(NetworkError{e, "Listen failed"});
    }

    {
        std::lock_guard<std::mutex> lk(fd_mutex_);
        server_socket_ = static_cast<intptr_t>(fd);
        port_ = port;
    }
    return {};
}

auto TCPServer::accept()
    -> expected<std::unique_ptr<TCPSocket>, NetworkError> {
    // Held across poll+accept: a concurrent stop() from another thread must
    // not close the descriptor while it is in (or about to enter) a syscall —
    // the fd number could then be reused by an unrelated open() and accepted
    // as if it were this listener.
    std::lock_guard<std::mutex> fd_lock(fd_mutex_);
    if (server_socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Server not listening"});
    }
    const sock_t listener = static_cast<sock_t>(server_socket_);

    // Poll with a 500ms timeout so the accept loop can wake up periodically
    // and observe a shutdown (running_ == false). A bare ::accept() would
    // block forever and close() from another thread does not reliably wake
    // it, hanging the process on Ctrl+C.
    pollfd pfd{};
    pfd.fd = listener;
    pfd.events = POLLIN;
    for (;;) {
        const int pr = sock_poll(&pfd, 1, 500);
        if (pr < 0) {
            if (sock_err() == EINTR) continue;
            return std::unexpected(NetworkError{sock_err(), "Poll failed"});
        }
        if (pr == 0) {
            // Timeout: no client. Signal the caller via EAGAIN so it can
            // re-check the running flag and loop back if still alive.
            return std::unexpected(NetworkError{EAGAIN, "Accept timed out"});
        }
        // The listener is only readable when a connection is pending; anything
        // else (POLLNVAL after an external close, POLLERR/POLLHUP) means the
        // descriptor is gone and accept must not be called on it.
        if ((pfd.revents & POLLIN) == 0) {
            return std::unexpected(
                NetworkError{ECONNABORTED, "Listen socket no longer usable"});
        }
        break;
    }

    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    sock_t client_socket = ::accept(listener,
                                    reinterpret_cast<sockaddr*>(&client_addr),
                                    &addr_len);
    if (client_socket == kInvalidSock) {
        return std::unexpected(NetworkError{sock_err(), "Accept failed"});
    }

    auto socket = std::make_unique<TCPSocket>();
    socket->sock_ = static_cast<intptr_t>(client_socket);
    socket->peer_address_ = std::string(INET_ADDRSTRLEN, '\0');
    inet_ntop(AF_INET, &client_addr.sin_addr,
              socket->peer_address_.data(), INET_ADDRSTRLEN);
    socket->peer_address_.resize(std::strlen(socket->peer_address_.c_str()));
    socket->peer_port_ = ntohs(client_addr.sin_port);

    tune_connected_sock(client_socket);

    return socket;
}

void TCPServer::stop() {
    std::lock_guard<std::mutex> lk(fd_mutex_);
    if (server_socket_ >= 0) {
        sock_close(static_cast<sock_t>(server_socket_));
        server_socket_ = -1;
    }
    port_ = 0;
}

namespace network_utils {

std::vector<std::string> get_local_addresses() {
    std::vector<std::string> addresses;
#ifdef _WIN32
    ensure_winsock();
    // GetAdaptersAddresses parity of the POSIX getifaddrs walk: collect the
    // unicast IPv4 of every non-loopback Up interface.
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                           GAA_FLAG_SKIP_MULTICAST |
                                           GAA_FLAG_SKIP_DNS_SERVER,
                               nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                           GAA_FLAG_SKIP_MULTICAST |
                                           GAA_FLAG_SKIP_DNS_SERVER,
                               nullptr, adapters, &size) != NO_ERROR) {
        return addresses;
    }
    for (auto* a = adapters; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET,
                        &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr,
                        ip, sizeof(ip));
            addresses.push_back(ip);
        }
    }
#else
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
#endif
    return addresses;
}

// Standard CRC-32 (IEEE 802.3) — identical to the former hand-rolled table.
// zlib ships this exact algorithm and is already linked (transitively via libpng).
uint32_t crc32(const void* data, size_t size) {
    return ::crc32(0L, static_cast<const Bytef*>(data), static_cast<uInt>(size));
}

} // namespace network_utils

} // namespace upix
