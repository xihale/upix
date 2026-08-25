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
    // PC→phone streamed progress while a CLIENT_SUBMIT is being processed. Keeps
    // traffic flowing during the (potentially minutes-long) upscale so NAT/stateful
    // firewalls cannot evict an idle connection, and lays the groundwork for the
    // phone to show real progress. The terminal result is still MSG_CLIENT_RESPONSE.
    MSG_TASK_PROGRESS = 0x0502,
    // Compressed-input submit (phone→PC): the phone ships the original file
    // bytes and the PC decodes, upscales, encodes, and streams the result
    // back. Only sent by APKs that know this protocol, so legacy peers never
    // see it. Progress for these tasks rides on MSG_TASK_PROGRESS_V2.
    MSG_CLIENT_SUBMIT_V2 = 0x0503,
    // PC→phone: continuation result bytes after a MSG_CLIENT_RESPONSE whose
    // encoded result exceeds MAX_NETWORK_PAYLOAD. Results at or below the cap
    // stay in the single RESPONSE frame, so APKs predating this type still
    // interoperate for every payload they could already receive.
    MSG_CLIENT_DATA = 0x0504,
    // Rich progress for MSG_CLIENT_SUBMIT_V2 tasks (TaskProgressV2 payload):
    // completed/total tiles let the phone compute percent and its own ETA.
    MSG_TASK_PROGRESS_V2 = 0x0505,

    // Pairing: phone→PC request immediately after the ECDH handshake; PC→phone
    // reply (gated on interactive approval the first time, auto-approved once
    // trusted). Sent inside EncryptedSession, before any CLIENT_SUBMIT.
    MSG_CLIENT_CONNECT_REQUEST = 0x0700,
    MSG_CLIENT_CONNECT_RESPONSE = 0x0701,

    // Shared model library (phone↔PC), one op per connection, same one-shot
    // pattern as CLIENT_SUBMIT: handshake → CONNECT_REQUEST → op frame → reply.
    MSG_MODEL_LIST_REQUEST     = 0x0510,  // phone→PC, empty payload
    MSG_MODEL_LIST_RESPONSE    = 0x0511,  // PC→phone: count + entries
    MSG_MODEL_UPLOAD           = 0x0512,  // phone→PC: header + param + bin
    MSG_MODEL_UPLOAD_RESPONSE  = 0x0513,  // PC→phone: ModelOpResponse
    MSG_MODEL_DOWNLOAD_REQUEST = 0x0514,  // phone→PC: name
    MSG_MODEL_DOWNLOAD_RESPONSE= 0x0515,  // PC→phone: header + param + bin

    // Local serve daemon (Unix domain socket). Separate range so the daemon's
    // protocol can evolve independently of the TCP protocol.
    MSG_SERVE_REQUEST = 0x0601,   // client→daemon: upscale request
    MSG_SERVE_RESPONSE = 0x0602,  // daemon→client: [ServeResponse][leading bytes]
    MSG_SERVE_DATA = 0x0603,      // daemon→client: continuation image bytes
    // daemon→client: tile progress while a request is being processed, so the
    // client can render the same live progress line the inline path shows.
    // Sent between MSG_SERVE_REQUEST and MSG_SERVE_RESPONSE.
    MSG_SERVE_PROGRESS = 0x0604,
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
// contiguously inside the same encrypted frame. output_format/quality tell the
// PC how to encode the upscaled result (raw RGB doesn't scale to camera photos).
struct ClientTaskSubmit {
    uint32_t image_width;
    uint32_t image_height;
    uint8_t channels;
    char model_name[64];
    uint32_t tile_size;
    uint32_t tile_overlap;
    uint32_t image_data_length;
    char output_format[8];  // "jpg"/"png"/"webp"/"avif"
    uint8_t output_quality; // 1..100
};

struct ClientTaskResponse {
    uint8_t success;
    char error_message[128];
    uint32_t result_data_length;
};

// Streamed upscale progress (PC→phone), 0..100. Sent inside the EncryptedSession
// as MSG_TASK_PROGRESS frames between CLIENT_SUBMIT and CLIENT_RESPONSE. The phone
// drains these (and may surface them to the UI) until the terminal CLIENT_RESPONSE.
struct TaskProgressMessage {
    uint32_t percent;  // 0..100 inclusive
};

// Rich progress (PC→phone) for SUBMIT_V2 tasks. stage 0 = inference tiles,
// percent/eta_seconds valid; stage 1 = inference done, the PC is encoding
// the result (percent/eta meaningless). The PC computes the ETA, so the
// phone gets a real time estimate without needing tile internals.
struct TaskProgressV2 {
    uint32_t stage;           // 0 = tiles, 1 = encoding result
    uint32_t percent;         // 0..100 (stage 0)
    uint32_t eta_seconds;     // estimated seconds remaining (stage 0), else 0
};

// SUBMIT_V2 payload header; compressed input file bytes follow contiguously.
struct ClientTaskSubmitV2 {
    char model_name[64];      // empty = PC's first available model
    char input_format[16];    // "jpg"/"png"/"webp"/"avif" (from source mime/name)
    char output_format[16];   // empty -> PC defaults to jpg
    uint32_t tile_size;       // 0 = auto
    uint32_t tile_overlap;
    uint32_t output_quality;  // 0 -> PC defaults to 90
    uint32_t input_data_length;
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

// ── Shared model library messages ───────────────────────────────────────────
// All structs use natural alignment with fixed-width fields, so the layouts
// match on Android arm64 and desktop x86_64. Variable-size data (file bytes,
// entry arrays) follows the struct contiguously inside the same frame.

// One PC-side model in a MODEL_LIST_RESPONSE.
struct ModelLibraryEntry {
    char name[64];
    uint32_t scale_factor;
    uint64_t total_size;   // param + bin bytes
};

// MODEL_LIST_RESPONSE payload: [ModelListResponse][count * ModelLibraryEntry].
struct ModelListResponse {
    uint32_t count;
};

// MODEL_UPLOAD payload: [ModelUploadRequest][param bytes][bin bytes].
// Files are written flat into the PC models dir as <name>.param / <name>.bin
// so ModelManager derives the model name from the stem alone.
struct ModelUploadRequest {
    char name[64];
    uint32_t scale_factor;
    uint64_t param_size;
    uint64_t bin_size;
};

// Generic op ack (MODEL_UPLOAD_RESPONSE).
struct ModelOpResponse {
    uint8_t success;
    char error[128];
};

// MODEL_DOWNLOAD_REQUEST payload.
struct ModelDownloadRequest {
    char name[64];
};

// MODEL_DOWNLOAD_RESPONSE payload: [ModelDownloadResponse][param][bin].
struct ModelDownloadResponse {
    uint8_t success;
    uint32_t scale_factor;
    uint64_t total_size;
    uint64_t param_size;
    uint64_t bin_size;
    char error[128];
};

// ── Local serve daemon messages ─────────────────────────────────────────────
// The daemon is the whole pipeline: read file → decode → upscale → encode →
// write file. The client sends PATHS plus parameters (both sides run on the
// same host/user — that is what the local unix socket guarantees), and gets
// progress frames + the output path back. No image bytes cross the socket at
// all, so the client's own footprint stays near zero.
struct ServeRequest {
    char model_name[64];     // empty = use daemon's current model; non-empty & different -> lazy reload
    char output_format[16];  // encoder for the result; empty = derive from paths
    int32_t tile_size;       // 0 = auto: daemon substitutes its engine's recommended size
                             // (the same rule the inline CLI path applies), so both paths
                             // produce identical pixels; >0 = use exactly this size
    int32_t tile_overlap;
    int32_t output_quality;  // encoder quality (ProcessingConfig::output_quality)
    uint32_t input_path_length;   // bytes of input path following this struct
    uint32_t output_path_length;  // bytes of output path following this struct; 0 = daemon derives
    // payload: [input_path bytes][output_path bytes] — no NUL terminators.
};

struct ServeResponse {
    int32_t ok;              // 1 = success, 0 = error
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t out_path_length;  // bytes of output path following this struct
    char error[128];
    char model_name[64];     // model actually used (daemon's choice when request
                             // named none) — reflected in the derived output name
    // payload on success: [output path bytes]
};

// MSG_SERVE_PROGRESS payload. stage 0: tile progress (the counters mirror
// ProcessingProgress so the client can rebuild its usual "42% (7/169, ETA)"
// line). stage 1: inference done, the daemon is encoding — lets the client
// label the post-100% wait instead of going silent.
struct ServeProgress {
    uint32_t completed_tiles;
    uint32_t total_tiles;
    uint32_t stage;          // 0 = tiles, 1 = encoding
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
