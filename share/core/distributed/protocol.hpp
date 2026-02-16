#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace crossscale {

enum class MessageType : uint16_t {
    MSG_HANDSHAKE_REQUEST = 0x0001,
    MSG_HANDSHAKE_RESPONSE = 0x0002,
    MSG_AUTH_REQUEST = 0x0003,
    MSG_AUTH_RESPONSE = 0x0004,
    MSG_DISCONNECT = 0x0005,
    MSG_HEARTBEAT = 0x0006,
    MSG_HEARTBEAT_ACK = 0x0007,
    
    MSG_DEVICE_PROFILE = 0x0100,
    MSG_DEVICE_LIST = 0x0101,
    MSG_DEVICE_JOIN = 0x0102,
    MSG_DEVICE_LEAVE = 0x0103,
    
    MSG_MODEL_LIST = 0x0200,
    MSG_MODEL_REQUEST = 0x0201,
    MSG_MODEL_DATA = 0x0202,
    MSG_MODEL_SYNC_START = 0x0203,
    MSG_MODEL_SYNC_COMPLETE = 0x0204,
    
    MSG_TASK_ASSIGNMENT = 0x0300,
    MSG_TASK_ACCEPT = 0x0301,
    MSG_TASK_REJECT = 0x0302,
    MSG_TASK_PROGRESS = 0x0303,
    MSG_TASK_RESULT = 0x0304,
    MSG_TASK_CANCEL = 0x0305,
    
    MSG_IMAGE_INFO = 0x0400,
    MSG_IMAGE_TILE = 0x0401,
    MSG_IMAGE_COMPLETE = 0x0402,
    
    MSG_ERROR = 0xFFFF,
};

struct MessageHeader {
    uint32_t magic = 0x53534358;
    uint8_t version = 1;
    MessageType type;
    uint32_t payload_length;
    uint32_t checksum;
};

struct HandshakeRequest {
    uint32_t protocol_version;
    char device_name[64];
    uint8_t device_type;
};

struct HandshakeResponse {
    uint8_t accepted;
    char rejection_reason[128];
    char server_name[64];
    uint16_t heartbeat_interval;
};

struct AuthRequest {
    char auth_key[64];
};

struct AuthResponse {
    uint8_t authenticated;
    char device_id[36];
};

struct DeviceProfileMessage {
    char device_id[36];
    char device_name[64];
    uint8_t device_type;
    float tiles_per_second;
    uint32_t recommended_tile_size;
    uint32_t max_batch_size;
    char ip_address[46];
    uint16_t port;
};

struct TaskAssignmentMessage {
    char task_id[36];
    char model_name[64];
    uint32_t tile_count;
};

struct TileInfo {
    uint32_t x, y;
    uint32_t width, height;
    uint32_t overlap;
};

struct ImageTileMessage {
    char task_id[36];
    uint32_t tile_index;
    uint32_t tile_x, tile_y;
    uint32_t width, height;
    uint8_t channels;
    uint8_t pixel_format;
    uint32_t data_length;
};

struct ModelInfoMessage {
    char name[64];
    uint32_t param_size;
    uint32_t bin_size;
    char hash[64];
};

struct ModelListMessage {
    uint32_t model_count;
};

struct ModelDataMessage {
    char model_name[64];
    uint8_t file_type;
    uint64_t file_offset;
    uint32_t data_length;
};

class Protocol {
public:
    static constexpr uint32_t MAGIC = 0x53534358;
    static constexpr uint8_t VERSION = 1;
    static constexpr size_t HEADER_SIZE = 15;
    
    static auto serialize(const MessageHeader& header, const void* payload) 
        -> std::vector<uint8_t>;
    
    static auto parse_header(const uint8_t* data, size_t size)
        -> std::optional<MessageHeader>;
    
    static auto compute_checksum(const void* data, size_t size) -> uint32_t;
    static bool verify_checksum(const uint8_t* data, size_t size, uint32_t expected);
};

}
