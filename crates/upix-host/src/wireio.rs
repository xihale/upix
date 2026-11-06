//! Safe little-endian codecs for the wire structs this crate consumes or
//! produces (mirrors of `share/core/distributed/protocol.hpp` structs that
//! ride inside frame payloads). `upix-net` owns the byte-exact frame header
//! (`frame::serialize_frame` / `parse_header`); these helpers only cover the
//! payload structs so `upix-host` never needs unsafe `repr(C)` casts.
//!
//! Field order, widths and padding are contractual — they must match the C++
//! structs on the wire (natural alignment, little-endian; identical on
//! Android arm64 and desktop x86_64).

use upix_net::protocol::{
    AuthResponse, ClientConnectRequest, ClientConnectResponse, ClientTaskResponse,
    ClientTaskSubmit, EncoderOptionsWire, KeyHelloMessage, ModelDownloadRequest,
    ModelDownloadResponse, ModelLibraryEntry, ModelOpResponse, ModelUploadRequest,
    ServeProgress, ServeRequest, ServeResponse, TaskProgress,
};

pub const KEY_HELLO_SIZE: usize = 128; // 32 + 32 + 64
pub const AUTH_PROOF_SIZE: usize = 64;
pub const AUTH_RESPONSE_SIZE: usize = 38; // 1 + 36 + 1
pub const CONNECT_REQUEST_SIZE: usize = 81; // 64 + 17
pub const CONNECT_RESPONSE_SIZE: usize = 5;
pub const CLIENT_TASK_SUBMIT_SIZE: usize = 140; // 64 + 16 + 16 + 4*4 + 28
pub const CLIENT_TASK_RESPONSE_SIZE: usize = 133; // 1 + 128 + 4
pub const TASK_PROGRESS_SIZE: usize = 12;
pub const MODEL_LIST_RESPONSE_SIZE: usize = 4;
pub const MODEL_LIBRARY_ENTRY_SIZE: usize = 80; // 64 + 4 + 4 pad + 8, align 8
pub const MODEL_UPLOAD_REQUEST_SIZE: usize = 88;
pub const MODEL_OP_RESPONSE_SIZE: usize = 129; // 1 + 128
pub const MODEL_DOWNLOAD_REQUEST_SIZE: usize = 64;
pub const MODEL_DOWNLOAD_RESPONSE_SIZE: usize = 160; // 1 + 3 pad + 4 + 8*3 + 128
pub const SERVE_REQUEST_SIZE: usize = 128; // 64 + 16 + 5*4 + 28
pub const SERVE_RESPONSE_SIZE: usize = 212; // 4*4 + 4 + 128 + 64
pub const SERVE_PROGRESS_SIZE: usize = 12;

// ── primitive readers/writers ───────────────────────────────────────────────

pub fn rd_u8(b: &[u8], off: usize) -> Option<u8> {
    b.get(off).copied()
}

pub fn rd_u32(b: &[u8], off: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(off..off + 4)?.try_into().ok()?))
}

pub fn rd_u64(b: &[u8], off: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(off..off + 8)?.try_into().ok()?))
}

pub fn rd_i32(b: &[u8], off: usize) -> Option<i32> {
    Some(i32::from_le_bytes(b.get(off..off + 4)?.try_into().ok()?))
}

/// Fixed-width char[] field → String, cut at the first NUL. Wire fields are
/// NOT guaranteed NUL-terminated, so the capacity always bounds the read
/// (the `bounded_string` rule from coordinator.cpp — a trailing field must
/// never be read past the struct).
pub fn rd_bounded_str(b: &[u8], off: usize, capacity: usize) -> String {
    let end = (off + capacity).min(b.len());
    let raw = &b[off..end];
    match raw.iter().position(|&c| c == 0) {
        Some(n) => String::from_utf8_lossy(&raw[..n]).into_owned(),
        None => String::from_utf8_lossy(raw).into_owned(),
    }
}

pub fn put_u8(v: u8, out: &mut Vec<u8>) {
    out.push(v);
}

pub fn put_u32(v: u32, out: &mut Vec<u8>) {
    out.extend_from_slice(&v.to_le_bytes());
}

pub fn put_u64(v: u64, out: &mut Vec<u8>) {
    out.extend_from_slice(&v.to_le_bytes());
}

pub fn put_i32(v: i32, out: &mut Vec<u8>) {
    out.extend_from_slice(&v.to_le_bytes());
}

/// Fixed char[N] field: copy the string, NUL-pad, never overrun (truncate at
/// capacity-1 exactly like the C++ `strncpy(dst, src, sizeof-1)` pattern).
pub fn put_fixed_str(s: &str, cap: usize, out: &mut Vec<u8>) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(cap.saturating_sub(1));
    out.extend_from_slice(&bytes[..n]);
    out.resize(out.len() + (cap - n), 0);
}

// ── per-struct codecs ───────────────────────────────────────────────────────

pub fn enc_key_hello(m: &KeyHelloMessage) -> Vec<u8> {
    let mut v = Vec::with_capacity(KEY_HELLO_SIZE);
    v.extend_from_slice(&m.eph_pub);
    v.extend_from_slice(&m.id_pub);
    v.extend_from_slice(&m.sig);
    v
}

pub fn dec_key_hello(b: &[u8]) -> Option<KeyHelloMessage> {
    if b.len() < KEY_HELLO_SIZE {
        return None;
    }
    Some(KeyHelloMessage {
        eph_pub: b[0..32].try_into().ok()?,
        id_pub: b[32..64].try_into().ok()?,
        sig: b[64..128].try_into().ok()?,
    })
}

pub fn enc_auth_proof(sig: &[u8; 64]) -> Vec<u8> {
    sig.to_vec()
}

pub fn dec_auth_proof(b: &[u8]) -> Option<[u8; 64]> {
    if b.len() < AUTH_PROOF_SIZE {
        return None;
    }
    b[0..64].try_into().ok()
}

pub fn enc_auth_response(m: &AuthResponse) -> Vec<u8> {
    let mut v = Vec::with_capacity(AUTH_RESPONSE_SIZE);
    put_u8(m.authenticated, &mut v);
    v.extend_from_slice(&m.device_id);
    put_u8(m.trusted, &mut v);
    v
}

pub fn dec_auth_response(b: &[u8]) -> Option<AuthResponse> {
    if b.len() < AUTH_RESPONSE_SIZE {
        return None;
    }
    Some(AuthResponse {
        authenticated: rd_u8(b, 0)?,
        device_id: b.get(1..37)?.try_into().ok()?,
        trusted: rd_u8(b, 37)?,
    })
}

pub fn enc_connect_request(m: &ClientConnectRequest) -> Vec<u8> {
    let mut v = Vec::with_capacity(CONNECT_REQUEST_SIZE);
    v.extend_from_slice(&m.device_name);
    v.extend_from_slice(&m.fingerprint);
    v
}

pub fn dec_connect_request(b: &[u8]) -> Option<ClientConnectRequest> {
    if b.len() < CONNECT_REQUEST_SIZE {
        return None;
    }
    Some(ClientConnectRequest {
        device_name: b[0..64].try_into().ok()?,
        fingerprint: b[64..81].try_into().ok()?,
    })
}

pub fn enc_connect_response(m: &ClientConnectResponse) -> Vec<u8> {
    let mut v = Vec::with_capacity(CONNECT_RESPONSE_SIZE);
    put_u8(m.approved, &mut v);
    put_u8(m.trusted, &mut v);
    put_u8(m.avif_decode, &mut v);
    put_u8(m.avif_encode, &mut v);
    put_u8(m.pipeline_depth, &mut v);
    v
}

pub fn dec_connect_response(b: &[u8]) -> Option<ClientConnectResponse> {
    if b.len() < CONNECT_RESPONSE_SIZE {
        return None;
    }
    Some(ClientConnectResponse {
        approved: rd_u8(b, 0)?,
        trusted: rd_u8(b, 1)?,
        avif_decode: rd_u8(b, 2)?,
        avif_encode: rd_u8(b, 3)?,
        pipeline_depth: rd_u8(b, 4)?,
    })
}

pub fn enc_task_submit(m: &ClientTaskSubmit) -> Vec<u8> {
    let mut v = Vec::with_capacity(CLIENT_TASK_SUBMIT_SIZE);
    v.extend_from_slice(&m.model_name);
    v.extend_from_slice(&m.input_format);
    v.extend_from_slice(&m.output_format);
    put_u32(m.tile_size, &mut v);
    put_u32(m.tile_overlap, &mut v);
    put_u32(m.output_quality, &mut v);
    put_u32(m.input_data_length, &mut v);
    let e = enc_encoder_wire(&m.encoder);
    v.extend_from_slice(&e);
    v
}

pub fn dec_task_submit(b: &[u8]) -> Option<ClientTaskSubmit> {
    if b.len() < CLIENT_TASK_SUBMIT_SIZE {
        return None;
    }
    Some(ClientTaskSubmit {
        model_name: b[0..64].try_into().ok()?,
        input_format: b[64..80].try_into().ok()?,
        output_format: b[80..96].try_into().ok()?,
        tile_size: rd_u32(b, 96)?,
        tile_overlap: rd_u32(b, 100)?,
        output_quality: rd_u32(b, 104)?,
        input_data_length: rd_u32(b, 108)?,
        encoder: dec_encoder_wire(b, 112)?,
    })
}

pub fn enc_encoder_wire(e: &EncoderOptionsWire) -> [u8; 28] {
    let mut out = [0u8; 28];
    out[0..4].copy_from_slice(&e.png_level.to_le_bytes());
    out[4..8].copy_from_slice(&e.jpeg_subsampling.to_le_bytes());
    out[8..12].copy_from_slice(&e.webp_method.to_le_bytes());
    out[12] = e.webp_lossless;
    out[13..16].copy_from_slice(&e.reserved);
    out[16..20].copy_from_slice(&e.avif_speed.to_le_bytes());
    out[20..24].copy_from_slice(&e.avif_subsampling.to_le_bytes());
    out[24..28].copy_from_slice(&e.avif_threads.to_le_bytes());
    out
}

pub fn dec_encoder_wire(b: &[u8], off: usize) -> Option<EncoderOptionsWire> {
    if b.len() < off + 28 {
        return None;
    }
    let g4i = |o: usize| rd_i32(b, o);
    Some(EncoderOptionsWire {
        png_level: g4i(off)?,
        jpeg_subsampling: g4i(off + 4)?,
        webp_method: g4i(off + 8)?,
        webp_lossless: rd_u8(b, off + 12)?,
        reserved: b.get(off + 13..off + 16)?.try_into().ok()?,
        avif_speed: g4i(off + 16)?,
        avif_subsampling: g4i(off + 20)?,
        avif_threads: g4i(off + 24)?,
    })
}

pub fn enc_task_response(m: &ClientTaskResponse) -> Vec<u8> {
    let mut v = Vec::with_capacity(CLIENT_TASK_RESPONSE_SIZE);
    put_u8(m.success, &mut v);
    v.extend_from_slice(&m.error_message);
    put_u32(m.result_data_length, &mut v);
    v
}

pub fn dec_task_response(b: &[u8]) -> Option<ClientTaskResponse> {
    if b.len() < CLIENT_TASK_RESPONSE_SIZE {
        return None;
    }
    Some(ClientTaskResponse {
        success: rd_u8(b, 0)?,
        error_message: b.get(1..129)?.try_into().ok()?,
        result_data_length: rd_u32(b, 129)?,
    })
}

pub fn enc_task_progress(m: &TaskProgress) -> [u8; 12] {
    let mut out = [0u8; 12];
    out[0..4].copy_from_slice(&m.stage.to_le_bytes());
    out[4..8].copy_from_slice(&m.percent.to_le_bytes());
    out[8..12].copy_from_slice(&m.eta_seconds.to_le_bytes());
    out
}

pub fn dec_task_progress(b: &[u8]) -> Option<TaskProgress> {
    if b.len() < TASK_PROGRESS_SIZE {
        return None;
    }
    Some(TaskProgress {
        stage: rd_u32(b, 0)?,
        percent: rd_u32(b, 4)?,
        eta_seconds: rd_u32(b, 8)?,
    })
}

pub fn enc_model_list_response(count: u32) -> [u8; 4] {
    count.to_le_bytes()
}

pub fn enc_library_entry(e: &ModelLibraryEntry) -> Vec<u8> {
    // C++ layout: char[64], uint32 (4 pad to align 8), uint64 → 76 bytes.
    let mut v = Vec::with_capacity(MODEL_LIBRARY_ENTRY_SIZE);
    v.extend_from_slice(&e.name);
    put_u32(e.scale_factor, &mut v);
    put_u32(0, &mut v); // alignment padding
    put_u64(e.total_size, &mut v);
    debug_assert_eq!(v.len(), MODEL_LIBRARY_ENTRY_SIZE);
    v
}

pub fn dec_library_entry(b: &[u8]) -> Option<ModelLibraryEntry> {
    if b.len() < MODEL_LIBRARY_ENTRY_SIZE {
        return None;
    }
    Some(ModelLibraryEntry {
        name: b[0..64].try_into().ok()?,
        scale_factor: rd_u32(b, 64)?,
        total_size: rd_u64(b, 72)?,
    })
}

pub fn enc_upload_request(m: &ModelUploadRequest) -> Vec<u8> {
    let mut v = Vec::with_capacity(MODEL_UPLOAD_REQUEST_SIZE);
    v.extend_from_slice(&m.name);
    put_u32(m.scale_factor, &mut v);
    put_u32(0, &mut v); // alignment padding
    put_u64(m.param_size, &mut v);
    put_u64(m.bin_size, &mut v);
    debug_assert_eq!(v.len(), MODEL_UPLOAD_REQUEST_SIZE);
    v
}

pub fn dec_upload_request(b: &[u8]) -> Option<ModelUploadRequest> {
    if b.len() < MODEL_UPLOAD_REQUEST_SIZE {
        return None;
    }
    Some(ModelUploadRequest {
        name: b[0..64].try_into().ok()?,
        scale_factor: rd_u32(b, 64)?,
        param_size: rd_u64(b, 72)?,
        bin_size: rd_u64(b, 80)?,
    })
}

pub fn enc_op_response(m: &ModelOpResponse) -> Vec<u8> {
    let mut v = Vec::with_capacity(MODEL_OP_RESPONSE_SIZE);
    put_u8(m.success, &mut v);
    v.extend_from_slice(&m.error);
    v
}

pub fn dec_op_response(b: &[u8]) -> Option<ModelOpResponse> {
    if b.len() < MODEL_OP_RESPONSE_SIZE {
        return None;
    }
    Some(ModelOpResponse {
        success: rd_u8(b, 0)?,
        error: b.get(1..129)?.try_into().ok()?,
    })
}

pub fn enc_download_request(m: &ModelDownloadRequest) -> Vec<u8> {
    m.name.to_vec()
}

pub fn dec_download_request(b: &[u8]) -> Option<ModelDownloadRequest> {
    if b.len() < MODEL_DOWNLOAD_REQUEST_SIZE {
        return None;
    }
    Some(ModelDownloadRequest {
        name: b[0..64].try_into().ok()?,
    })
}

pub fn enc_download_response(m: &ModelDownloadResponse) -> Vec<u8> {
    // C++ layout: u8, u32, u64, u64, u64, char[128]:
    //   offset 0  success (1)
    //   offset 4  scale_factor (4)   ← 3 bytes padding after success
    //   offset 8  total_size (8)
    //   offset 16 param_size (8)
    //   offset 24 bin_size (8)
    //   offset 32 error[128]
    let mut v = Vec::with_capacity(MODEL_DOWNLOAD_RESPONSE_SIZE);
    put_u8(m.success, &mut v);
    v.extend_from_slice(&[0u8; 3]); // padding (offsets 1..4)
    put_u32(m.scale_factor, &mut v);
    put_u64(m.total_size, &mut v);
    put_u64(m.param_size, &mut v);
    put_u64(m.bin_size, &mut v);
    v.extend_from_slice(&m.error);
    debug_assert_eq!(v.len(), MODEL_DOWNLOAD_RESPONSE_SIZE);
    v
}

pub fn dec_download_response(b: &[u8]) -> Option<ModelDownloadResponse> {
    if b.len() < MODEL_DOWNLOAD_RESPONSE_SIZE {
        return None;
    }
    Some(ModelDownloadResponse {
        success: rd_u8(b, 0)?,
        scale_factor: rd_u32(b, 4)?,
        total_size: rd_u64(b, 8)?,
        param_size: rd_u64(b, 16)?,
        bin_size: rd_u64(b, 24)?,
        error: b.get(32..160)?.try_into().ok()?,
    })
}

pub fn enc_serve_request(m: &ServeRequest) -> Vec<u8> {
    // C++ layout: char[64] + char[16] + 5×i32 + (4 pad) + EncoderOptionsWire
    // (align 4 → no pad needed after five i32s; total 64+16+20+28 = 128? No:
    // EncoderOptionsWire is align 4, so no padding — sizeof(ServeRequest) is
    // 64+16+20+28 = 128? 64+16=80, +20=100, +28=128. Yes: 128.)
    let mut v = Vec::with_capacity(128);
    v.extend_from_slice(&m.model_name);
    v.extend_from_slice(&m.output_format);
    put_i32(m.tile_size, &mut v);
    put_i32(m.tile_overlap, &mut v);
    put_i32(m.output_quality, &mut v);
    put_u32(m.input_path_length, &mut v);
    put_u32(m.output_path_length, &mut v);
    v.extend_from_slice(&enc_encoder_wire(&m.encoder));
    debug_assert_eq!(v.len(), 128);
    v
}

pub fn dec_serve_request(b: &[u8]) -> Option<ServeRequest> {
    if b.len() < 128 {
        return None;
    }
    Some(ServeRequest {
        model_name: b[0..64].try_into().ok()?,
        output_format: b[64..80].try_into().ok()?,
        tile_size: rd_i32(b, 80)?,
        tile_overlap: rd_i32(b, 84)?,
        output_quality: rd_i32(b, 88)?,
        input_path_length: rd_u32(b, 92)?,
        output_path_length: rd_u32(b, 96)?,
        encoder: dec_encoder_wire(b, 100)?,
    })
}

pub fn enc_serve_response(m: &ServeResponse) -> Vec<u8> {
    // C++ layout: 5×i32 (20) + u32 out_path_length (4) + char[128] + char[64].
    // All-align-4 → no padding: sizeof(ServeResponse) = 24+128+64 = 216.
    let mut v = Vec::with_capacity(SERVE_RESPONSE_SIZE);
    put_i32(m.ok, &mut v);
    put_i32(m.width, &mut v);
    put_i32(m.height, &mut v);
    put_i32(m.channels, &mut v);
    put_u32(m.out_path_length, &mut v);
    v.extend_from_slice(&m.error);
    v.extend_from_slice(&m.model_name);
    debug_assert_eq!(v.len(), SERVE_RESPONSE_SIZE);
    v
}

pub fn dec_serve_response(b: &[u8]) -> Option<ServeResponse> {
    if b.len() < SERVE_RESPONSE_SIZE {
        return None;
    }
    Some(ServeResponse {
        ok: rd_i32(b, 0)?,
        width: rd_i32(b, 4)?,
        height: rd_i32(b, 8)?,
        channels: rd_i32(b, 12)?,
        out_path_length: rd_u32(b, 16)?,
        error: b.get(20..148)?.try_into().ok()?,
        model_name: b.get(148..212)?.try_into().ok()?,
    })
}

pub fn enc_serve_progress(m: &ServeProgress) -> [u8; 12] {
    let mut out = [0u8; 12];
    out[0..4].copy_from_slice(&m.completed_tiles.to_le_bytes());
    out[4..8].copy_from_slice(&m.total_tiles.to_le_bytes());
    out[8..12].copy_from_slice(&m.stage.to_le_bytes());
    out
}

pub fn dec_serve_progress(b: &[u8]) -> Option<ServeProgress> {
    if b.len() < SERVE_PROGRESS_SIZE {
        return None;
    }
    Some(ServeProgress {
        completed_tiles: rd_u32(b, 0)?,
        total_tiles: rd_u32(b, 4)?,
        stage: rd_u32(b, 8)?,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_match_cxx_static_asserts() {
        assert_eq!(KEY_HELLO_SIZE, 128);
        assert_eq!(AUTH_RESPONSE_SIZE, 38);
        assert_eq!(CONNECT_REQUEST_SIZE, 81);
        assert_eq!(CLIENT_TASK_SUBMIT_SIZE, 140);
        assert_eq!(CLIENT_TASK_RESPONSE_SIZE, 133);
        assert_eq!(MODEL_UPLOAD_REQUEST_SIZE, 88);
        assert_eq!(MODEL_DOWNLOAD_RESPONSE_SIZE, 160);
        assert_eq!(SERVE_REQUEST_SIZE, 128);
        assert_eq!(SERVE_RESPONSE_SIZE, 212);
    }

    #[test]
    fn task_submit_roundtrip() {
        let m = ClientTaskSubmit {
            model_name: {
                let mut a = [0u8; 64];
                a[..4].copy_from_slice(b"abc\0");
                a
            },
            input_format: {
                let mut a = [0u8; 16];
                a[..3].copy_from_slice(b"jpg");
                a
            },
            output_format: [0u8; 16],
            tile_size: 0,
            tile_overlap: 16,
            output_quality: 90,
            input_data_length: 12345,
            encoder: EncoderOptionsWire {
                png_level: -1,
                jpeg_subsampling: 420,
                webp_method: -1,
                webp_lossless: 0,
                reserved: [0; 3],
                avif_speed: 6,
                avif_subsampling: 420,
                avif_threads: 0,
            },
        };
        let enc = enc_task_submit(&m);
        assert_eq!(enc.len(), 140);
        let dec = dec_task_submit(&enc).unwrap();
        assert_eq!(&dec.model_name[..4], b"abc\0");
        assert_eq!(dec.tile_overlap, 16);
        assert_eq!(dec.input_data_length, 12345);
        assert_eq!(dec.encoder.avif_speed, 6);
    }

    #[test]
    fn truncated_reads_are_none_not_panic() {
        assert!(dec_task_submit(&[0u8; 10]).is_none());
        assert!(dec_serve_request(&[0u8; 100]).is_none());
        assert!(dec_download_response(&[0u8; 100]).is_none());
    }
}
