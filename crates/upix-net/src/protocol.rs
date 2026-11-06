//! Wire protocol types — Rust mirror of `share/core/distributed/protocol.hpp`.
//!
//! These types cross the wire to OLD C++ / Android clients. Byte layout and
//! field order are contractual: `#[repr(C)]`, little-endian on the wire,
//! natural alignment identical on Android arm64 and desktop x86_64.
//! Frame format: `magic[4] type[2] payload_length[4] checksum[4]` little-endian.
//!
//! Serialization / parsing / checksum (in `crate::frame`) and the crypto
//! session (in `crate::session`) must be byte-identical to the C++
//! implementation — proven by golden vectors in `tests/golden/`.

use crate::EncoderOptions;

pub const MAGIC: u32 = 0x5353_4358;
/// magic[4] type[2] payload_length[4] checksum[4], little-endian.
pub const HEADER_SIZE: usize = 14;
/// Single-frame payload cap (verify against protocol.cpp when porting).
pub const MAX_NETWORK_PAYLOAD: u32 = 100 * 1024 * 1024;
/// Chunk size for MSG_CLIENT_DATA continuation frames, both directions.
pub const CLIENT_DATA_CHUNK_BYTES: u32 = 8 * 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum MessageType {
    // Encrypted handshake. KEY_HELLO is plaintext (ephemeral + identity
    // pubkeys + S→C sig); AUTH_PROOF and everything after travel inside
    // EncryptedSession.
    KeyHello = 0x0007,
    AuthProof = 0x0008,
    AuthResponse = 0x0004,

    // One-shot image submit (phone→PC, inside EncryptedSession after pairing).
    ClientSubmit = 0x0500,
    ClientResponse = 0x0501,
    /// PC→phone streamed progress while a CLIENT_SUBMIT is being processed.
    TaskProgress = 0x0502,
    /// Continuation data frames, both directions (chunked submit upload /
    /// >MAX_NETWORK_PAYLOAD result stream).
    ClientData = 0x0503,
    /// phone→PC, empty payload: abort the task currently running on this
    /// session. Stray cancels after the response are tolerated and ignored.
    TaskCancel = 0x0504,

    // Pairing (inside EncryptedSession, before any CLIENT_SUBMIT).
    ClientConnectRequest = 0x0700,
    ClientConnectResponse = 0x0701,

    // Shared model library (phone↔PC), one op per connection.
    ModelListRequest = 0x0510,
    ModelListResponse = 0x0511,
    ModelUpload = 0x0512,
    ModelUploadResponse = 0x0513,
    ModelDownloadRequest = 0x0514,
    ModelDownloadResponse = 0x0515,

    // Local serve daemon (Unix domain socket), separate number range.
    ServeRequest = 0x0601,
    ServeResponse = 0x0602,
    ServeData = 0x0603,
    ServeProgress = 0x0604,
}

impl MessageType {
    pub fn from_u16(v: u16) -> Option<Self> {
        Some(match v {
            0x0007 => Self::KeyHello,
            0x0008 => Self::AuthProof,
            0x0004 => Self::AuthResponse,
            0x0500 => Self::ClientSubmit,
            0x0501 => Self::ClientResponse,
            0x0502 => Self::TaskProgress,
            0x0503 => Self::ClientData,
            0x0504 => Self::TaskCancel,
            0x0700 => Self::ClientConnectRequest,
            0x0701 => Self::ClientConnectResponse,
            0x0510 => Self::ModelListRequest,
            0x0511 => Self::ModelListResponse,
            0x0512 => Self::ModelUpload,
            0x0513 => Self::ModelUploadResponse,
            0x0514 => Self::ModelDownloadRequest,
            0x0515 => Self::ModelDownloadResponse,
            0x0601 => Self::ServeRequest,
            0x0602 => Self::ServeResponse,
            0x0603 => Self::ServeData,
            0x0604 => Self::ServeProgress,
            _ => return None,
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MessageHeader {
    pub magic: u32,
    pub msg_type: MessageType,
    pub payload_length: u32,
    pub checksum: u32,
}

/// First handshake message (plaintext). Sent by both client (sig zeroed) and
/// server (sig = Ed25519 detached over the transcript
/// `c_eph||c_id||s_eph||s_id`).
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct KeyHelloMessage {
    pub eph_pub: [u8; 32],
    pub id_pub: [u8; 32],
    pub sig: [u8; 64],
}

/// Second handshake message (encrypted, inside EncryptedSession).
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct AuthProofMessage {
    pub sig: [u8; 64],
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct AuthResponse {
    pub authenticated: u8,
    pub device_id: [u8; 36], // NUL-padded C string
    pub trusted: u8,         // 0 = first-time (TOFU pending), 1 = already in TrustStore
}

/// Fixed-layout mirror of `upix_img::EncoderOptions` that rides
/// ClientTaskSubmit and ServeRequest. Natural 4-byte alignment, identical on
/// Android arm64 and desktop x86_64. sizeof == 28 — assert in tests.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct EncoderOptionsWire {
    pub png_level: i32,         // -1 = auto (derive from output_quality)
    pub jpeg_subsampling: i32,  // 420 default
    pub webp_method: i32,       // -1 = auto (libwebp default)
    pub webp_lossless: u8,
    pub reserved: [u8; 3],
    pub avif_speed: i32,        // 6 default
    pub avif_subsampling: i32,  // 420 default
    pub avif_threads: i32,      // 0 = auto (hardware concurrency)
}

impl From<&EncoderOptions> for EncoderOptionsWire {
    fn from(o: &EncoderOptions) -> Self {
        Self {
            png_level: o.png_level,
            jpeg_subsampling: o.jpeg_subsampling,
            webp_method: o.webp_method,
            webp_lossless: u8::from(o.webp_lossless),
            reserved: [0; 3],
            avif_speed: o.avif_speed,
            avif_subsampling: o.avif_subsampling,
            avif_threads: o.avif_threads,
        }
    }
}

impl From<EncoderOptionsWire> for EncoderOptions {
    fn from(w: EncoderOptionsWire) -> Self {
        Self {
            png_level: w.png_level,
            jpeg_subsampling: w.jpeg_subsampling,
            webp_method: w.webp_method,
            webp_lossless: w.webp_lossless != 0,
            avif_speed: w.avif_speed,
            avif_subsampling: w.avif_subsampling,
            avif_threads: w.avif_threads,
        }
    }
}

/// One-shot image submit (phone→PC). Two wire formats, distinguished by the
/// submit frame's payload length:
/// - legacy single frame — payload = sizeof(ClientTaskSubmit) + input bytes
/// - chunked (preferred for inputs > ~4MB) — submit frame carries ONLY the
///   struct (payload == sizeof(ClientTaskSubmit), input_data_length = total);
///   input bytes follow as N MSG_CLIENT_DATA frames (each ≤
///   CLIENT_DATA_CHUNK_BYTES plaintext, independently AEAD-sealed).
///
/// Structs only ever grow at the tail; receivers validate exact frame sizes,
/// so a version-mismatched pair fails loudly instead of misparsing.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ClientTaskSubmit {
    pub model_name: [u8; 64],    // empty = PC's first available model
    pub input_format: [u8; 16],  // "jpg"/"png"/"webp"/"avif" (from source mime/name)
    pub output_format: [u8; 16], // empty -> PC defaults to jpg
    pub tile_size: u32,          // 0 = auto: PC picks via TileSizeOptimizer (cached per model)
    pub tile_overlap: u32,
    pub output_quality: u32, // 0 -> PC defaults to 90
    pub input_data_length: u32, // total input bytes (inline OR across DATA chunks)
    pub encoder: EncoderOptionsWire,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ClientTaskResponse {
    pub success: u8,
    pub error_message: [u8; 128], // NUL-padded C string
    pub result_data_length: u32,
}

/// Streamed upscale progress (PC→phone), between CLIENT_SUBMIT and
/// CLIENT_RESPONSE. stage 0 = inference tiles; stage 1 = inference done,
/// encoding result.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct TaskProgress {
    pub stage: u32,
    pub percent: u32,     // 0..100 (stage 0)
    pub eta_seconds: u32, // stage 0, else 0
}

/// Pairing request: phone→PC right after the ECDH handshake succeeds.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ClientConnectRequest {
    pub device_name: [u8; 64],
    pub fingerprint: [u8; 17], // 16 hex chars + NUL
}

/// Pairing response: PC→phone. `pipeline_depth` is a tail-grown field; the
/// legacy size floor is 4 bytes (older PCs omit it = no pipelining).
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ClientConnectResponse {
    pub approved: u8,
    pub trusted: u8,
    pub avif_decode: u8,
    pub avif_encode: u8,
    pub pipeline_depth: u8,
}

/// One PC-side model in a MODEL_LIST_RESPONSE.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ModelLibraryEntry {
    pub name: [u8; 64],
    pub scale_factor: u32,
    pub total_size: u64, // param + bin bytes
}

/// MODEL_LIST_RESPONSE payload: [ModelListResponse][count * ModelLibraryEntry].
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ModelListResponse {
    pub count: u32,
}

/// MODEL_UPLOAD payload: [ModelUploadRequest][param bytes][bin bytes].
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ModelUploadRequest {
    pub name: [u8; 64],
    pub scale_factor: u32,
    pub param_size: u64,
    pub bin_size: u64,
}

/// Generic op ack (MODEL_UPLOAD_RESPONSE).
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ModelOpResponse {
    pub success: u8,
    pub error: [u8; 128],
}

/// MODEL_DOWNLOAD_REQUEST payload.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ModelDownloadRequest {
    pub name: [u8; 64],
}

/// MODEL_DOWNLOAD_RESPONSE payload: [ModelDownloadResponse][param][bin].
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ModelDownloadResponse {
    pub success: u8,
    pub scale_factor: u32,
    pub total_size: u64,
    pub param_size: u64,
    pub bin_size: u64,
    pub error: [u8; 128],
}

/// Local serve daemon (AF_UNIX). The daemon is the whole pipeline: read file
/// → decode → upscale → encode → write file. Paths cross the socket, not
/// image bytes.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ServeRequest {
    pub model_name: [u8; 64], // empty = daemon's current model; else lazy reload
    pub output_format: [u8; 16], // empty = derive from paths
    pub tile_size: i32,       // 0 = auto (engine recommended, same rule as inline CLI)
    pub tile_overlap: i32,
    pub output_quality: i32,
    pub input_path_length: u32,  // bytes of input path following this struct
    pub output_path_length: u32, // bytes of output path; 0 = daemon derives
    pub encoder: EncoderOptionsWire,
    // payload: [input_path bytes][output_path bytes] — no NUL terminators.
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ServeResponse {
    pub ok: i32, // 1 = success, 0 = error
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub out_path_length: u32, // bytes of output path following this struct
    pub error: [u8; 128],
    pub model_name: [u8; 64], // model actually used (reflected in derived output name)
    // payload on success: [output path bytes]
}

/// MSG_SERVE_PROGRESS payload. stage 0: tile progress (counters mirror
/// ProcessingProgress); stage 1: inference done, encoding.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ServeProgress {
    pub completed_tiles: u32,
    pub total_tiles: u32,
    pub stage: u32,
}
