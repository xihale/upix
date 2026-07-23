#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <array>
#include <mutex>
#include "expected.hpp"

namespace crossscale {

struct NetworkError {
    int code;
    std::string message;
};

// Basic TCP socket wrapper
class TCPSocket {
public:
    TCPSocket();
    ~TCPSocket();
    
    friend class TCPServer;
    
    // No copy
    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;
    
    // Move
    TCPSocket(TCPSocket&& other) noexcept;
    TCPSocket& operator=(TCPSocket&& other) noexcept;
    
    // Connect to a server
    auto connect(const std::string& host, uint16_t port)
        -> expected<void, NetworkError>;
    
    auto send(const void* data, size_t size)
        -> expected<size_t, NetworkError>;
    
    auto send_all(const void* data, size_t size)
        -> expected<void, NetworkError>;
    
    auto receive(void* buffer, size_t max_size)
        -> expected<size_t, NetworkError>;
    
    auto receive_all(void* buffer, size_t size)
        -> expected<void, NetworkError>;
    
    // Set timeout (milliseconds)
    void set_timeout(int timeout_ms);

    // Set socket buffer sizes (SO_SNDBUF and SO_RCVBUF)
    void set_buffer_size(size_t size);

    // Close connection
    void close();
    
    // Check if connected
    bool is_connected() const;
    
    // Get socket handle
    int handle() const { return socket_; }
    
    // Get peer address
    std::string peer_address() const;
    uint16_t peer_port() const;

private:
    int socket_ = -1;
    std::string peer_address_;
    uint16_t peer_port_ = 0;
    // Guards all socket I/O so that heartbeat and task threads can't interleave
    // partial sends/receives and corrupt the framing protocol.
    mutable std::recursive_mutex io_mutex_;
};

// TCP Server
class TCPServer {
public:
    TCPServer();
    ~TCPServer();
    
    // No copy
    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;
    
    // Start listening on port
    auto listen(uint16_t port, int backlog = 10)
        -> expected<void, NetworkError>;
    
    auto accept()
        -> expected<std::unique_ptr<TCPSocket>, NetworkError>;
    
    // Stop server
    void stop();
    
    // Check if listening
    bool is_listening() const;
    
    // Get port
    uint16_t port() const { return port_; }

private:
    int server_socket_ = -1;
    uint16_t port_ = 0;
};

// Network utilities
namespace network_utils {
    // Get local IP addresses
    std::vector<std::string> get_local_addresses();

    // Generate random auth key
    std::string generate_auth_key(size_t length = 16);

    // Simple CRC32
    uint32_t crc32(const void* data, size_t size);
}

} // namespace crossscale
