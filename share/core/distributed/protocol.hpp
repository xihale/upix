#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crossscale {

// Wire protocol for the 1:1 phone↔PC model. The phone is a submitter (not a
// compute worker): it handshakes, pairs, then sends CLIENT_SUBMIT frames
// carrying an image; the PC upscales each one on its own engine and replies
// with CLIENT_RESPONSE. The multi-worker dispatch messages (DEVICE_PROFILE,
// MODEL_*, TASK_*, TILES_BATCH) have been removed along with mDNS discovery.
enum class MessageType : uint16_t {
    // Encrypted handshake. KEY_HELLO is plaintext (carries ephemeral + identity
    // pubkeys + S→C sig); AUTH_PROOF and everything after travel inside
    // EncryptedSession.
    MSG_KEY_HELLO = 0x0007,
    MSG_AUTH_PROOF = 0x0008,
    MSG_AUTH_RESPONSE = 0x0004,

    // One-shot image submit (phone→PC, inside EncryptedSession after pairing).
    MSG_CLIENT_SUBMIT = 0x0500,
    MSG_CLIENT_RESPONSE = 0x0501,

    // Pairing: phone→PC request immediately after the ECDH handshake; PC→phone
    // reply (gated on interactive approval the first time, auto-approved once
    // trusted). Sent inside EncryptedSession, before any CLIENT_SUBMIT.
    MSG_CLIENT_CONNECT_REQUEST = 0x0700,
    MSG_CLIENT_CONNECT_RESPONSE = 0x0701,

    // Local serve daemon (Unix domain socket). Separate range so the daemon's
    // protocol can evolve independently of the TCP protocol.
    MSG_SERVE_REQUEST = 0x0601,   // client→daemon: upscale request
    MSG_SERVE_RESPONSE = 0x0602,  // daemon→client: upscale result
};

struct MessageHeader {
    uint32_t magic = 0x53534358;
    uint8_t version = 2;
    MessageType type;
    uint32_t payload_length;
    uint32_t checksum;
};

// First handshake message (plaintext). Sent by both client (sig zeroed) and
// server (sig = Ed25519 detached over the transcript c_eph||c_id||s_eph||s_id).
struct KeyHelloMessage {
    uint8_t eph_pub[32];
    uint8_t id_pub[32];
    uint8_t sig[64];
};

// Second handshake message (encrypted, inside EncryptedSession).
// Client's Ed25519 detached signature over the same transcript.
struct AuthProofMessage {
    uint8_t sig[64];
};

struct AuthResponse {
    uint8_t authenticated;
    char device_id[36];
    uint8_t trusted;  // 0 = first-time (TOFU pending), 1 = already in TrustStore
};

// One-shot image submit (phone→PC). The raw u8 image pixels follow the struct
// contiguously inside the same encrypted frame.
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

// Pairing request: phone→PC immediately after the ECDH handshake succeeds.
// Carries the device's human-readable name, its own fingerprint (so the PC can
// show "Xiaomi ... ABCD1234" in the approval prompt), and whether the phone has
// its data-stream encryption toggle on (the effective stream mode is negotiated
// as min(pc, phone) by the PC).
struct ClientConnectRequest {
    char device_name[64];
    char fingerprint[17];          // 16 hex chars + NUL
    uint8_t wants_encrypted_stream; // 1 = phone's toggle is ON
};

// Pairing response: PC→phone.
//   approved         0 = PC denied (phone must disconnect), 1 = allowed
//   trusted          0 = first-time (just approved this session), 1 = already
//                    in TrustStore (auto-approved, no prompt shown on PC)
//   stream_encrypted final negotiated data-stream mode (0 = plaintext, 1 = enc)
struct ClientConnectResponse {
    uint8_t approved;
    uint8_t trusted;
    uint8_t stream_encrypted;
};

// ── Local serve daemon messages ─────────────────────────────────────────────
// The daemon holds a warm engine; clients send an image, receive the upscaled
// result. On the wire: Protocol header + payload, where
// payload = [Serve{Request,Response} struct][img_length bytes] contiguous.
struct ServeRequest {
    char model_name[64];     // empty = use daemon's current model; non-empty & different -> lazy reload
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
    static constexpr uint8_t VERSION = 2;
    static constexpr size_t HEADER_SIZE = 15;

    static auto serialize(const MessageHeader& header, const void* payload)
        -> std::vector<uint8_t>;

    static auto parse_header(const uint8_t* data, size_t size)
        -> std::optional<MessageHeader>;

    static auto compute_checksum(const void* data, size_t size) -> uint32_t;
};

}
