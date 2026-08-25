#include "stream_socket.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace upix {

auto StreamSocket::send(const void* data, size_t size)
    -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }

    ssize_t sent = ::send(socket_, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
        return std::unexpected(NetworkError{errno, "Send failed"});
    }
    return static_cast<size_t>(sent);
}

auto StreamSocket::send_all(const void* data, size_t size)
    -> expected<void, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    size_t total_sent = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (total_sent < size) {
        auto result = send(ptr + total_sent, size - total_sent);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        total_sent += result.value();
    }
    return {};
}

auto StreamSocket::receive(void* buffer, size_t max_size)
    -> expected<size_t, NetworkError> {
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    if (socket_ < 0) {
        return std::unexpected(NetworkError{EBADF, "Socket not connected"});
    }

    ssize_t received = ::recv(socket_, buffer, max_size, 0);
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
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
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
    std::lock_guard<std::recursive_mutex> lock(io_mutex_);
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

} // namespace upix
