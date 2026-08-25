#include "protocol.hpp"
#include "coordinator.hpp"
#include "utils/network.hpp"
#include "utils/logger.hpp"
#include <cstring>

namespace crossscale {

auto Protocol::serialize(const MessageHeader& header, const void* payload)
    -> std::vector<uint8_t> {

    size_t total_size = HEADER_SIZE + header.payload_length;
    std::vector<uint8_t> buffer(total_size);

    uint8_t* ptr = buffer.data();

    std::memcpy(ptr, &header.magic, 4); ptr += 4;
    uint16_t type = static_cast<uint16_t>(header.type);
    std::memcpy(ptr, &type, 2); ptr += 2;
    std::memcpy(ptr, &header.payload_length, 4); ptr += 4;

    if (header.payload_length > 0 && payload) {
        std::memcpy(ptr + 4, payload, header.payload_length);
    }

    // crc32 over the ENTIRE frame [0, total_size). The checksum slot [10..13]
    // is still zero here (buffer is value-initialized), so the checksum covers
    // every header field and payload byte while excluding its own bytes.
    uint32_t checksum = compute_checksum(buffer.data(), total_size);
    std::memcpy(ptr, &checksum, 4);

    return buffer;
}

auto Protocol::parse_header(std::span<const uint8_t> data)
    -> std::optional<MessageHeader> {

    if (data.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    MessageHeader header;

    std::memcpy(&header.magic, data.data(), 4);
    if (header.magic != MAGIC) {
        return std::nullopt;
    }

    uint16_t type;
    std::memcpy(&type, data.data() + 4, 2);
    header.type = static_cast<MessageType>(type);

    std::memcpy(&header.payload_length, data.data() + 6, 4);

    std::memcpy(&header.checksum, data.data() + 10, 4);

    return header;
}

auto Protocol::compute_checksum(const void* data, size_t size) -> uint32_t {
    return network_utils::crc32(data, size);
}

} // namespace crossscale
