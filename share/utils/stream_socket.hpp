#pragma once

#include <mutex>
#include "expected.hpp"
#include "network_error.hpp"  // NetworkError (used in expected return types)

namespace upix {

// Shared implementation for stream sockets backed by a plain fd (TCP and
// AF_UNIX): owns the fd plus the I/O lock that keeps concurrent users from
// interleaving partial sends/receives (which would corrupt the framing
// protocol), and implements the send/receive primitives. TCPSocket and
// UnixSocket build their connect/peer-bookkeeping surface on top and expose
// these methods unchanged through public inheritance.
class StreamSocket {
public:
    auto send(const void* data, size_t size)
        -> expected<size_t, NetworkError>;

    auto send_all(const void* data, size_t size)
        -> expected<void, NetworkError>;

    auto receive(void* buffer, size_t max_size)
        -> expected<size_t, NetworkError>;

    auto receive_all(void* buffer, size_t size)
        -> expected<void, NetworkError>;

protected:
    StreamSocket() = default;
    ~StreamSocket() { close_fd(); }

    // Mutexes are non-movable; derived classes implement their own move
    // operations around take_fd_from().
    StreamSocket(const StreamSocket&) = delete;
    StreamSocket& operator=(const StreamSocket&) = delete;

    // Move-construct the fd part (no locking — mirrors the derived classes'
    // move constructors, which also run unlocked).
    StreamSocket(StreamSocket&& other) noexcept
        : socket_(other.socket_) {
        other.socket_ = -1;
    }

    // Close the fd (no-op when already closed). Derived close() may add their
    // own bookkeeping on top (e.g. clearing the Unix socket path).
    void close_fd();

    // Steal the fd from `other` (used by derived move assignments). The
    // caller is responsible for locking other.io_mutex_ when required.
    void take_fd_from(StreamSocket& other) {
        socket_ = other.socket_;
        other.socket_ = -1;
    }

    int socket_ = -1;
    mutable std::recursive_mutex io_mutex_;
};

} // namespace upix
