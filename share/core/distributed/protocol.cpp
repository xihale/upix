#include "protocol.hpp"
#include "coordinator.hpp"
#include "utils/network.hpp"
#include <cstring>
#include <nlohmann/json.hpp>

namespace crossscale {

auto Protocol::serialize(const MessageHeader& header, const void* payload)
    -> std::vector<uint8_t> {
    
    size_t total_size = HEADER_SIZE + header.payload_length;
    std::vector<uint8_t> buffer(total_size);
    
    uint8_t* ptr = buffer.data();
    
    std::memcpy(ptr, &header.magic, 4); ptr += 4;
    *ptr++ = header.version;
    uint16_t type = static_cast<uint16_t>(header.type);
    std::memcpy(ptr, &type, 2); ptr += 2;
    std::memcpy(ptr, &header.payload_length, 4); ptr += 4;
    
    if (header.payload_length > 0 && payload) {
        std::memcpy(ptr + 4, payload, header.payload_length);
    }
    
    uint32_t checksum = compute_checksum(buffer.data(), total_size - 4);
    std::memcpy(ptr, &checksum, 4);
    
    return buffer;
}

auto Protocol::parse_header(const uint8_t* data, size_t size)
    -> std::optional<MessageHeader> {
    
    if (size < HEADER_SIZE) {
        return std::nullopt;
    }
    
    MessageHeader header;
    
    std::memcpy(&header.magic, data, 4);
    if (header.magic != MAGIC) {
        return std::nullopt;
    }
    
    header.version = data[4];
    
    uint16_t type;
    std::memcpy(&type, data + 5, 2);
    header.type = static_cast<MessageType>(type);
    
    std::memcpy(&header.payload_length, data + 7, 4);
    
    std::memcpy(&header.checksum, data + 11, 4);
    
    return header;
}

auto Protocol::compute_checksum(const void* data, size_t size) -> uint32_t {
    return network_utils::crc32(data, size);
}

bool Protocol::verify_checksum(const uint8_t* data, size_t size, uint32_t expected) {
    return compute_checksum(data, size) == expected;
}

auto ConnectionInfo::to_json() const -> std::string {
    nlohmann::json j;
    j["v"] = 1;
    j["ip"] = ip;
    j["port"] = port;
    j["key"] = auth_key;
    j["name"] = server_name;
    j["model"] = current_model;
    return j.dump();
}

auto ConnectionInfo::from_json(const std::string& json)
    -> expected<ConnectionInfo, CoordinatorError> {
    
    try {
        auto j = nlohmann::json::parse(json);
        
        ConnectionInfo info;
        info.ip = j.value("ip", "");
        info.port = j.value("port", 0);
        info.auth_key = j.value("key", "");
        info.server_name = j.value("name", "");
        info.current_model = j.value("model", "");
        
        return info;
    } catch (const std::exception& e) {
        return CoordinatorError{1, "Failed to parse connection info"};
    }
}

auto ConnectionInfo::to_short_string() const -> std::string {
    return ip + ":" + std::to_string(port) + "#" + auth_key;
}

auto ConnectionInfo::from_short_string(const std::string& str)
    -> expected<ConnectionInfo, CoordinatorError> {
    
    auto colon_pos = str.find(':');
    auto hash_pos = str.find('#');
    
    if (colon_pos == std::string::npos || hash_pos == std::string::npos) {
        return CoordinatorError{1, "Invalid connection string format"};
    }
    
    ConnectionInfo info;
    info.ip = str.substr(0, colon_pos);
    info.port = static_cast<uint16_t>(std::stoi(str.substr(colon_pos + 1, hash_pos - colon_pos - 1)));
    info.auth_key = str.substr(hash_pos + 1);
    
    return info;
}

} // namespace crossscale
