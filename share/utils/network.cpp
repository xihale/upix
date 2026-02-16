#include "network.hpp"
#include "logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <cstring>
#include <random>
#include <chrono>

namespace crossscale {

TCPSocket::TCPSocket() = default;

TCPSocket::~TCPSocket() {
    close();
}

TCPSocket::TCPSocket(TCPSocket&& other) noexcept
    : socket_(other.socket_)
    , peer_address_(std::move(other.peer_address_))
    , peer_port_(other.peer_port_) {
    other.socket_ = -1;
    other.peer_port_ = 0;
}

TCPSocket& TCPSocket::operator=(TCPSocket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = other.socket_;
        peer_address_ = std::move(other.peer_address_);
        peer_port_ = other.peer_port_;
        other.socket_ = -1;
        other.peer_port_ = 0;
    }
    return *this;
}

auto TCPSocket::connect(const std::string& host, uint16_t port)
    -> expected<void, NetworkError> {
    close();
    
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        return NetworkError{errno, "Failed to create socket"};
    }

    int flag = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set default socket buffer size (1MB) for better network performance
    int buf_size = 1 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close();
        return NetworkError{errno, "Invalid address"};
    }

    if (::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return NetworkError{errno, "Connection failed"};
    }

    peer_address_ = host;
    peer_port_ = port;
    return {};
}

auto TCPSocket::send(const void* data, size_t size)
    -> expected<size_t, NetworkError> {
    if (socket_ < 0) {
        return NetworkError{EBADF, "Socket not connected"};
    }

    ssize_t sent = ::send(socket_, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
        return NetworkError{errno, "Send failed"};
    }
    return static_cast<size_t>(sent);
}

auto TCPSocket::send_all(const void* data, size_t size)
    -> expected<void, NetworkError> {
    size_t total_sent = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (total_sent < size) {
        auto result = send(ptr + total_sent, size - total_sent);
        if (!result.has_value()) {
            return result.error();
        }
        total_sent += result.value();
    }
    return {};
}

auto TCPSocket::receive(void* buffer, size_t max_size)
    -> expected<size_t, NetworkError> {
    if (socket_ < 0) {
        return NetworkError{EBADF, "Socket not connected"};
    }

    ssize_t received = ::recv(socket_, buffer, max_size, 0);
    if (received < 0) {
        return NetworkError{errno, "Receive failed"};
    }
    if (received == 0) {
        return NetworkError{ECONNRESET, "Connection closed"};
    }
    return static_cast<size_t>(received);
}

auto TCPSocket::receive_all(void* buffer, size_t size)
    -> expected<void, NetworkError> {
    size_t total_received = 0;
    uint8_t* ptr = static_cast<uint8_t*>(buffer);

    while (total_received < size) {
        auto result = receive(ptr + total_received, size - total_received);
        if (!result.has_value()) {
            return result.error();
        }
        total_received += result.value();
    }
    return {};
}

void TCPSocket::set_timeout(int timeout_ms) {
    if (socket_ < 0) return;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void TCPSocket::set_buffer_size(size_t size) {
    if (socket_ < 0) return;

    int buf_size = static_cast<int>(size);
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
}

void TCPSocket::close() {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    peer_address_.clear();
    peer_port_ = 0;
}

bool TCPSocket::is_connected() const {
    return socket_ >= 0;
}

std::string TCPSocket::peer_address() const {
    return peer_address_;
}

uint16_t TCPSocket::peer_port() const {
    return peer_port_;
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
        return NetworkError{errno, "Failed to create socket"};
    }

    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(server_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        stop();
        return NetworkError{errno, "Bind failed"};
    }

    if (::listen(server_socket_, backlog) < 0) {
        stop();
        return NetworkError{errno, "Listen failed"};
    }

    port_ = port;
    return {};
}

auto TCPServer::accept()
    -> expected<std::unique_ptr<TCPSocket>, NetworkError> {
    if (server_socket_ < 0) {
        return NetworkError{EBADF, "Server not listening"};
    }

    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    
    int client_socket = ::accept(server_socket_, 
                                  reinterpret_cast<sockaddr*>(&client_addr), 
                                  &addr_len);
    if (client_socket < 0) {
        return NetworkError{errno, "Accept failed"};
    }

    auto socket = std::make_unique<TCPSocket>();
    socket->socket_ = client_socket;
    socket->peer_address_ = inet_ntoa(client_addr.sin_addr);
    socket->peer_port_ = ntohs(client_addr.sin_port);

    int flag = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set default socket buffer size (1MB) for better network performance
    int buf_size = 1 * 1024 * 1024;
    setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(client_socket, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    return socket;
}

void TCPServer::stop() {
    if (server_socket_ >= 0) {
        ::close(server_socket_);
        server_socket_ = -1;
    }
    port_ = 0;
}

bool TCPServer::is_listening() const {
    return server_socket_ >= 0;
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

bool is_port_available(uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    bool available = (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(sock);
    
    return available;
}

std::string generate_auth_key(size_t length) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
    
    std::string key;
    key.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        key += chars[dis(gen)];
    }
    return key;
}

static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void init_crc32_table() {
    if (crc32_table_init) return;
    
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

uint32_t crc32(const void* data, size_t size) {
    init_crc32_table();
    
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    
    for (size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ ptr[i]) & 0xFF];
    }
    
    return crc ^ 0xFFFFFFFF;
}

uint32_t crc32(const std::vector<uint8_t>& data) {
    return crc32(data.data(), data.size());
}

} // namespace network_utils

} // namespace crossscale
