#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <array>
#include <mutex>
#include "expected.hpp"
#include "network_error.hpp"  // NetworkError
#include "stream_socket.hpp"

namespace upix {

// Maximum payload size for network messages (100 MB).
inline constexpr uint32_t MAX_NETWORK_PAYLOAD = 100 * 1024 * 1024;

// Basic TCP socket wrapper. Framing I/O (send/receive and their _all
// variants) plus the fd and I/O lock live in StreamSocket.
class TCPSocket : public StreamSocket {
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

    // Connect with a bounded timeout: non-blocking connect + poll, then
    // restore blocking mode on success. Host resolution goes through
    // getaddrinfo (AF_UNSPEC + AI_ADDRCONFIG), so IPv6 literals and
    // hostnames work, unlike the IPv4-only connect() above; every candidate
    // address is tried until the shared deadline. Returns false on
    // timeout/failure (no error detail — callers log host:port).
    bool connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms);

    // Set timeout (milliseconds)
    void set_timeout(int timeout_ms);

    // Set ONLY the receive (SO_RCVTIMEO) timeout, leaving the send timeout
    // untouched. Used by the receive-poll loops that multiplex one connection
    // between a reader thread and a sender thread: the reader polls with a
    // short timeout so it can re-check its wake flags between frames, while
    // concurrent response sends keep the generous full timeout for large
    // transfers (shrinking SO_SNDTIMEO would abort big sends on a peer that
    // merely stalled for a moment).
    void set_recv_timeout(int timeout_ms);

    // Close connection
    void close();

private:
    std::string peer_address_;
    uint16_t peer_port_ = 0;
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

private:
    int server_socket_ = -1;
    uint16_t port_ = 0;
};

// Network utilities
namespace network_utils {
    // Get local IP addresses
    std::vector<std::string> get_local_addresses();

    // CRC32 (zlib-backed)
    uint32_t crc32(const void* data, size_t size);
}

} // namespace upix
