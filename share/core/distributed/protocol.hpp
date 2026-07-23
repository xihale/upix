#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <vector>

namespace crossscale {

enum class MessageType : uint16_t {
    MSG_AUTH_REQUEST = 0x0003,
    MSG_AUTH_RESPONSE = 0x0004,
    MSG_DISCONNECT = 0x0005,
    MSG_HEARTBEAT = 0x0006,

    MSG_DEVICE_PROFILE = 0x0100,

    MSG_MODEL_LIST = 0x0200,
    MSG_MODEL_REQUEST = 0x0201,
    MSG_MODEL_DATA = 0x0202,

    MSG_TASK_ASSIGNMENT = 0x0300,
    MSG_TASK_ACCEPT = 0x0301,
    MSG_TASK_PROGRESS = 0x0303,
    MSG_TASK_RESULT = 0x0304,

    MSG_IMAGE_TILE = 0x0401,
    MSG_TILES_BATCH = 0x0403,

    MSG_CLIENT_SUBMIT = 0x0500,
    MSG_CLIENT_RESPONSE = 0x0501,

    // Local serve daemon (Unix domain socket). Separate range so the daemon's
    // protocol can evolve independently of the distributed TCP protocol.
    MSG_SERVE_REQUEST = 0x0601,   // client→daemon: upscale request
    MSG_SERVE_RESPONSE = 0x0602,  // daemon→client: upscale result
};

struct MessageHeader {
    uint32_t magic = 0x53534358;
    uint8_t version = 1;
    MessageType type;
    uint32_t payload_length;
    uint32_t checksum;
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

struct ClientTaskSubmit {
    uint32_t image_width;
    uint32_t image_height;
    uint8_t channels;
    char model_name[64];
    uint32_t tile_size;
    uint32_t tile_overlap;
    uint32_t image_data_length;
};

struct ClientTaskResponse {
    uint8_t success;
    char error_message[128];
    uint32_t result_data_length;
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

// ── Local serve daemon messages ─────────────────────────────────────────────
// The daemon holds a warm engine; clients send an image, receive the upscaled
// result. On the wire: Protocol header + payload, where
// payload = [Serve{Request,Response} struct][img_length bytes] contiguous.
// All integers little-endian (host order on POSIX serve targets).

struct ServeRequest {
    char model_name[64];     // 空名 = 用守护进程当前模型;非空且不同 → lazy reload
    int32_t tile_size;
    int32_t tile_overlap;
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t img_length;
};

struct ServeResponse {
    int32_t ok;              // 1 = success, 0 = error
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t img_length;
    char error[128];
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
};

}
