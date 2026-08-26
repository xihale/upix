#pragma once

#include <mutex>
#include "expected.hpp"
#include "network_error.hpp"  // NetworkError (used in expected return types)

namespace upix {

// Shared implementation for stream sockets backed by a plain fd (TCP and
// AF_UNIX): owns the fd plus SEPARATE receive/transmit locks. Full-duplex
// streams allow simultaneous read+write, and the wire framing is per
// direction, so a reader polling with SO_RCVTIMEO must never keep a sender
// waiting for the whole poll window: a shared rx/tx lock here turned every
// interleave into a lock convoy (~200 ms per acquire; a 546 KB result took
// 11-23 s to transfer). send_all / receive_all hold their own direction's
// lock across the whole call, so concurrent writers/reader races remain
// impossible.
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

    // Close the fd under both direction locks: while it runs concurrently
    // with an in-flight recv/send the syscall returns EBADF/ENOTCONN either
    // way — same observable behavior as closing mid-transfer before.
    void close_fd();

    // Steal the fd from `other` (used by derived move assignments). The
    // caller is responsible for locking other.tx_mutex_/rx_mutex_ when
    // required.
    void take_fd_from(StreamSocket& other) {
        socket_ = other.socket_;
        other.socket_ = -1;
    }

    int socket_ = -1;
    mutable std::recursive_mutex tx_mutex_;
    mutable std::recursive_mutex rx_mutex_;
};

} // namespace upix
