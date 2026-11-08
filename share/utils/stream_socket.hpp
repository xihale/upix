#pragma once

#include <mutex>
#include <optional>
#include "expected.hpp"
#include "network_error.hpp"  // NetworkError (used in expected return types)

namespace upix {

// Shared implementation for stream sockets backed by an OS handle — a plain
// fd (TCP, AF_UNIX) on POSIX or a named-pipe HANDLE on Windows. Owns the
// handle plus SEPARATE receive/transmit locks. Full-duplex streams allow
// simultaneous read+write, and the wire framing is per direction, so a
// reader polling with SO_RCVTIMEO must never keep a sender waiting for the
// whole poll window: a shared rx/tx lock here turned every interleave into a
// lock convoy (~200 ms per acquire; a 546 KB result took 11-23 s to
// transfer). send_all / receive_all hold their own direction's lock across
// the whole call, so concurrent writers/reader races remain impossible.
//
// Subclasses provide the four raw platform primitives (one-syscall read/
// write, forced wake, and their own constructors/move/close plumbing);
// everything above byte chunks — framing, EAGAIN propagation, locking —
// lives here, platform-independent.
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

    // Best-effort wake for a peer thread blocked in receive_all (e.g. a GUI
    // thread cancelling a worker): on POSIX ::shutdown() reliably interrupts
    // a blocked recv with ECONNRESET, on Windows CancelIoEx aborts a blocked
    // ReadFile — close() on another thread is not guaranteed to wake either.
    // Harmless on an already-closed endpoint.
    void shutdown_wake();

protected:
    StreamSocket() = default;
    ~StreamSocket() { close_fd(); }

    // Mutexes are non-movable; derived classes implement their own move
    // operations around take_handle_from().
    StreamSocket(const StreamSocket&) = delete;
    StreamSocket& operator=(const StreamSocket&) = delete;

    // Move-construct the handle part (no locking — mirrors the derived
    // classes' move constructors, which also run unlocked).
    StreamSocket(StreamSocket&& other) noexcept
        : sock_(other.sock_) {
        other.sock_ = -1;
    }

    // Close the handle under both direction locks: while it runs
    // concurrently with an in-flight read/write the syscall fails with
    // EBADF/ERROR_OPERATION_ABORTED either way — same observable behavior as
    // closing mid-transfer before.
    void close_fd();

    // Steal the handle from `other` (used by derived move assignments). The
    // caller is responsible for locking other.tx_mutex_/rx_mutex_ when
    // required.
    void take_handle_from(StreamSocket& other) {
        sock_ = other.sock_;
        other.sock_ = -1;
    }

    // Raw platform primitives. `nullopt` = success; a NetworkError (EAGAIN,
    // ECONNRESET, aborted, ...) propagates verbatim to the framing layer.
    // POSIX implementors retry EINTR inside raw_send_some/raw_receive_some —
    // the framing layer must not see signals as failures.
    virtual auto raw_send_some(const uint8_t* data, size_t size, size_t& sent)
        -> std::optional<NetworkError> = 0;
    virtual auto raw_receive_some(uint8_t* buffer, size_t max_size,
                                  size_t& received)
        -> std::optional<NetworkError> = 0;
    virtual void raw_wake() = 0;
    virtual void raw_close() = 0;

    // fd (POSIX) or HANDLE (Windows). -1 = closed. `intptr_t` keeps both
    // worlds in one member.
    intptr_t sock_ = -1;
    mutable std::recursive_mutex tx_mutex_;
    mutable std::recursive_mutex rx_mutex_;
};

// One ::send()/::recv() syscall with EINTR retry — shared by the POSIX
// subclasses (TCP and AF_UNIX). Windows subclasses use overlapped ReadFile/
// WriteFile instead and never call these.
#ifdef _WIN32
// (no POSIX helpers on Windows)
#else
auto fd_send_some(int fd, const uint8_t* data, size_t size, size_t& sent)
    -> std::optional<NetworkError>;
auto fd_receive_some(int fd, uint8_t* buffer, size_t max_size, size_t& received)
    -> std::optional<NetworkError>;
#endif

} // namespace upix
