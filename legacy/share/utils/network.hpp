#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <array>
#include <mutex>
#include <optional>
#include "expected.hpp"
#include "network_error.hpp"  // NetworkError
#include "stream_socket.hpp"

namespace upix {

// Maximum payload size for network messages (100 MB).
inline constexpr uint32_t MAX_NETWORK_PAYLOAD = 100 * 1024 * 1024;

// Basic TCP socket wrapper. Framing I/O (send/receive and their _all
// variants) plus the handle and I/O lock live in StreamSocket.
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

    // Announce shutdown in both directions of a live connection WITHOUT
    // releasing the descriptor: a thread parked in recv()/receive_all() sees
    // EOF and a thread parked in send()/send_all() fails, instead of waiting
    // out its SO_RCVTIMEO/SO_SNDTIMEO. This is what lets a shutdown path wake
    // a session handler whose peer went silent (the handler then unwinds and
    // the socket is closed by its owner as usual).
    //
    // Lifetime contract: no I/O lock is taken here — the very calls that must
    // be interrupted are the ones holding them — so the descriptor is only
    // touched while the caller has established that the socket object is
    // alive. Callers therefore hand out the pointer under a lock that the
    // owner also takes before it destroys the socket (Coordinator's session
    // wake handles do this), which is also what keeps this from ever acting
    // on a descriptor number that has been closed and recycled.
    void shutdown();

private:
    // StreamSocket raw primitives (see stream_socket.hpp).
    auto raw_send_some(const uint8_t* data, size_t size, size_t& sent)
        -> std::optional<NetworkError> override;
    auto raw_receive_some(uint8_t* buffer, size_t max_size, size_t& received)
        -> std::optional<NetworkError> override;
    void raw_wake() override;
    void raw_close() override;

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
    // SOCKET on Windows (HANDLE-width), fd on POSIX — -1 = closed.
    intptr_t server_socket_ = -1;
    uint16_t port_ = 0;
    // Guards server_socket_ across listen()/accept()/stop(). It exists so a
    // stop()/destructor close can never land between accept()'s poll() and its
    // ::accept(), where the descriptor would be closed — and its number reused
    // by an unrelated open() — mid-call. accept() holds it for at most one
    // 500 ms poll tick. A shutdown path that must not wait for that tick
    // requests the accept loop's stop token first and only closes the listener
    // once the loop has exited (see Coordinator::stop).
    mutable std::mutex fd_mutex_;
};

// Network utilities
namespace network_utils {
    // Get local IP addresses
    std::vector<std::string> get_local_addresses();

    // CRC32 (zlib-backed)
    uint32_t crc32(const void* data, size_t size);
}

} // namespace upix
