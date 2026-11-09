#include "protocol.hpp"
#include "coordinator.hpp"
#include "utils/network.hpp"
#include "utils/logger.hpp"
#include <cstring>

namespace upix {

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

auto Protocol::verify_frame(const uint8_t* header, const void* payload,
                            size_t payload_size) -> bool {
    // Mirror serialize() exactly: rebuild the frame with the slot zeroed and
    // compare. Serve payloads are bounded (~8 KB), so the copy is free. The
    // vector is value-initialized, so copying only the first 10 header bytes
    // (everything before the checksum slot) leaves the slot zero.
    std::vector<uint8_t> buffer(HEADER_SIZE + payload_size, 0);
    std::memcpy(buffer.data(), header, 10);
    if (payload_size > 0 && payload) {
        std::memcpy(buffer.data() + HEADER_SIZE, payload, payload_size);
    }
    uint32_t expected = 0;
    std::memcpy(&expected, header + 10, 4);
    return compute_checksum(buffer.data(), buffer.size()) == expected;
}

auto Protocol::compute_checksum(const void* data, size_t size) -> uint32_t {
    return network_utils::crc32(data, size);
}

} // namespace upix
