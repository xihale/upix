#include "stream_socket.hpp"

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace upix {

#ifndef _WIN32
auto fd_send_some(int fd, const uint8_t* data, size_t size, size_t& sent)
    -> std::optional<NetworkError> {
    // A signal delivered during the syscall (the CLI/daemon installs SIGINT/
    // SIGTERM handlers) returns -1/EINTR; retrying keeps that from looking
    // like a dead connection.
    ssize_t n;
    do {
        n = ::send(fd, data, size, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return NetworkError{errno, "Send failed"};
    }
    sent = static_cast<size_t>(n);
    return std::nullopt;
}

auto fd_receive_some(int fd, uint8_t* buffer, size_t max_size, size_t& received)
    -> std::optional<NetworkError> {
    ssize_t n;
    do {
        n = ::recv(fd, buffer, max_size, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return NetworkError{errno, "Receive failed"};
    }
    received = static_cast<size_t>(n);
    return std::nullopt;
}
#endif

auto StreamSocket::send(const void* data, size_t size)
    -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(tx_mutex_);
    if (sock_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }
    size_t sent = 0;
    if (auto err = raw_send_some(static_cast<const uint8_t*>(data), size, sent)) {
        return std::unexpected(*err);
    }
    return sent;
}

auto StreamSocket::send_all(const void* data, size_t size)
    -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(tx_mutex_);
    size_t total_sent = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (total_sent < size) {
        size_t sent = 0;
        if (auto err = raw_send_some(ptr + total_sent, size - total_sent, sent)) {
            return std::unexpected(*err);
        }
        if (sent == 0) {
            // A zero-byte send for a non-empty request would spin here forever.
            return std::unexpected(NetworkError{EPIPE, "Send made no progress"});
        }
        total_sent += sent;
    }
    return {};
}

auto StreamSocket::receive(void* buffer, size_t max_size)
    -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(rx_mutex_);
    if (sock_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }
    size_t received = 0;
    if (auto err = raw_receive_some(static_cast<uint8_t*>(buffer), max_size,
                                    received)) {
        return std::unexpected(*err);
    }
    if (received == 0) {
        return std::unexpected(NetworkError{ECONNRESET, "Connection closed"});
    }
    return received;
}

auto StreamSocket::receive_all(void* buffer, size_t size)
    -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(rx_mutex_);
    size_t total_received = 0;
    uint8_t* ptr = static_cast<uint8_t*>(buffer);

    while (total_received < size) {
        size_t received = 0;
        if (auto err = raw_receive_some(ptr + total_received,
                                        size - total_received, received)) {
            return std::unexpected(*err);
        }
        total_received += received;
    }
    return {};
}

void StreamSocket::close_fd() {
    // Both direction locks: a concurrent send/recv must observe a stable
    // sock_ either side of the close (it then fails with EBADF — same as
    // closing mid-transfer before the split).
    std::lock_guard<std::recursive_mutex> tx_lock(tx_mutex_);
    std::lock_guard<std::recursive_mutex> rx_lock(rx_mutex_);
    if (sock_ >= 0) {
        raw_close();
        sock_ = -1;
    }
}

void StreamSocket::shutdown_wake() {
    // Only the rx side matters (the blocked reader); taking both locks like
    // close_fd() would convoy with the very recv we are waking.
    std::lock_guard<std::recursive_mutex> rx_lock(rx_mutex_);
    if (sock_ >= 0) {
        raw_wake();
    }
}

} // namespace upix
