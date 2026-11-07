#include "stream_socket.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace upix {

auto StreamSocket::send(const void* data, size_t size)
    -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(tx_mutex_);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }

    // A signal delivered during the syscall (the CLI/daemon installs SIGINT/
    // SIGTERM handlers) returns -1/EINTR; retrying keeps that from looking
    // like a dead connection.
    ssize_t sent;
    do {
        sent = ::send(socket_, data, size, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        return std::unexpected(NetworkError{errno, "Send failed"});
    }
    return static_cast<size_t>(sent);
}

auto StreamSocket::send_all(const void* data, size_t size)
    -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(tx_mutex_);
    size_t total_sent = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (total_sent < size) {
        auto result = send(ptr + total_sent, size - total_sent);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        if (result.value() == 0) {
            // A zero-byte send for a non-empty request would spin here forever.
            return std::unexpected(NetworkError{EPIPE, "Send made no progress"});
        }
        total_sent += result.value();
    }
    return {};
}

auto StreamSocket::receive(void* buffer, size_t max_size)
    -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(rx_mutex_);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }

    // EINTR is retried for the same reason as in send().
    ssize_t received;
    do {
        received = ::recv(socket_, buffer, max_size, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        return std::unexpected(NetworkError{errno, "Receive failed"});
    }
    if (received == 0) {
        return std::unexpected(NetworkError{ECONNRESET, "Connection closed"});
    }
    return static_cast<size_t>(received);
}

auto StreamSocket::receive_all(void* buffer, size_t size)
    -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(rx_mutex_);
    size_t total_received = 0;
    uint8_t* ptr = static_cast<uint8_t*>(buffer);

    while (total_received < size) {
        auto result = receive(ptr + total_received, size - total_received);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        total_received += result.value();
    }
    return {};
}

void StreamSocket::close_fd() {
    // Both direction locks: a concurrent send/recv must observe a stable
    // socket_ either side of the close (it then fails with EBADF — same as
    // closing mid-transfer before the split).
    std::lock_guard<std::recursive_mutex> tx_lock(tx_mutex_);
    std::lock_guard<std::recursive_mutex> rx_lock(rx_mutex_);
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

void StreamSocket::shutdown_wake() {
    // Only the rx side matters (the blocked reader); taking both locks like
    // close_fd() would convoy with the very recv we are waking.
    std::lock_guard<std::recursive_mutex> rx_lock(rx_mutex_);
    if (socket_ >= 0) {
        ::shutdown(socket_, SHUT_RDWR);
    }
}

} // namespace upix
