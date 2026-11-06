//! Frame serialization / parsing / checksum — Rust port of
//! `share/core/distributed/protocol.cpp`.
//!
//! Frame layout (little-endian): `magic[4] type[2] payload_length[4]
//! checksum[4] payload[...]`. The checksum slot is computed over the whole
//! frame while the slot itself is zero (see `serialize_frame`).
//!
//! ENCRYPTED FRAMES: checksum is forced to 0 (see `crate::session`) and MUST
//! skip `verify_frame` — a plaintext CRC over ciphertext would leak bytes and
//! the Poly1305 MAC already covers integrity.

use crate::protocol::{MessageHeader, HEADER_SIZE, MAGIC};
use crc32fast::Hasher;

/// CRC-32 (IEEE 802.3, zlib-compatible) over `data`. Mirrors
/// `network_utils::crc32` (zlib `crc32(0L, ...)`).
pub fn compute_checksum(data: &[u8]) -> u32 {
    let mut h = Hasher::new();
    h.update(data);
    h.finalize()
}

fn put_u16(out: &mut [u8], v: u16) {
    out[0] = (v & 0xFF) as u8;
    out[1] = (v >> 8) as u8;
}

fn put_u32(out: &mut [u8], v: u32) {
    out[0] = (v & 0xFF) as u8;
    out[1] = ((v >> 8) & 0xFF) as u8;
    out[2] = ((v >> 16) & 0xFF) as u8;
    out[3] = ((v >> 24) & 0xFF) as u8;
}

/// Serialize `header` + `payload` into one wire frame.
///
/// Mirrors `Protocol::serialize`: the checksum covers the ENTIRE frame
/// `[0, total)` while the slot `[10..13]` is still zero, so every header
/// field and payload byte is covered but the slot's own bytes are not.
///
/// `payload.len()` is authoritative for the copy (the C++ reads
/// `header.payload_length` bytes and would overread on mismatch; callers
/// there always pass consistent values, which the golden vectors pin).
pub fn serialize_frame(header: MessageHeader, payload: &[u8]) -> Vec<u8> {
    let payload_len = payload.len();
    let total_size = HEADER_SIZE + payload_len;
    let mut buffer = vec![0u8; total_size];

    put_u32(&mut buffer[0..4], header.magic);
    put_u16(&mut buffer[4..6], header.msg_type as u16);
    put_u32(&mut buffer[6..10], payload_len as u32);
    // buffer[10..14] stays zero (value-initialized), then payload at [14..].
    buffer[HEADER_SIZE..].copy_from_slice(payload);

    let checksum = compute_checksum(&buffer);
    put_u32(&mut buffer[10..14], checksum);
    buffer
}

/// Parse a frame header from the start of `data`. Returns `None` if fewer
/// than `HEADER_SIZE` bytes are available, the magic is wrong, or the type is
/// not a known `MessageType` (the C++ casts the raw u16 unchecked; the Rust
/// header type is a closed enum, so unknown values are rejected here — pinned
/// by the `unknown_type` golden vector).
pub fn parse_header(data: &[u8]) -> Option<MessageHeader> {
    if data.len() < HEADER_SIZE {
        return None;
    }

    let magic = u32::from_le_bytes(data[0..4].try_into().ok()?);
    if magic != MAGIC {
        return None;
    }

    let raw_type = u16::from_le_bytes(data[4..6].try_into().ok()?);
    let msg_type = crate::protocol::MessageType::from_u16(raw_type)?;
    let payload_length = u32::from_le_bytes(data[6..10].try_into().ok()?);
    let checksum = u32::from_le_bytes(data[10..14].try_into().ok()?);

    Some(MessageHeader {
        magic,
        msg_type,
        payload_length,
        checksum,
    })
}

/// Integrity check, inverse of `serialize_frame`: rebuild the frame with the
/// checksum slot zeroed (only the first 10 header bytes are copied) and
/// compare against the slot value.
///
/// Enforced on the serve daemon path (plaintext AF_UNIX frames). Encrypted
/// frames carry checksum 0 and MUST NOT be verified here.
pub fn verify_frame(header: &[u8; HEADER_SIZE], payload: &[u8]) -> bool {
    let mut buffer = vec![0u8; HEADER_SIZE + payload.len()];
    buffer[..10].copy_from_slice(&header[..10]);
    buffer[HEADER_SIZE..].copy_from_slice(payload);
    let expected = u32::from_le_bytes(header[10..14].try_into().expect("14-byte header"));
    compute_checksum(&buffer) == expected
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::MessageType;

    fn hdr(t: MessageType, payload_len: u32) -> MessageHeader {
        MessageHeader {
            magic: MAGIC,
            msg_type: t,
            payload_length: payload_len,
            checksum: 0,
        }
    }

    #[test]
    fn serialize_roundtrip() {
        let payload = b"hello upix frame";
        let frame = serialize_frame(hdr(MessageType::KeyHello, payload.len() as u32), payload);
        assert_eq!(frame.len(), HEADER_SIZE + payload.len());
        let parsed = parse_header(&frame).expect("parses");
        assert_eq!(parsed.magic, MAGIC);
        assert_eq!(parsed.msg_type, MessageType::KeyHello);
        assert_eq!(parsed.payload_length as usize, payload.len());
        assert!(verify_frame(
            &frame[..HEADER_SIZE].try_into().unwrap(),
            payload
        ));
    }

    #[test]
    fn checksum_slot_zeroed_during_compute() {
        // Deterministic sample: crc of "upix" (from the golden dumper).
        assert_eq!(compute_checksum(b"upix"), 3283471874);
        assert_eq!(compute_checksum(&[]), 0);
    }

    #[test]
    fn verify_rejects_corruption() {
        let payload = [1u8, 2, 3, 4, 5];
        let mut frame = serialize_frame(hdr(MessageType::ClientData, 5), &payload);
        assert!(verify_frame(
            &frame[..HEADER_SIZE].try_into().unwrap(),
            &payload
        ));
        frame[HEADER_SIZE] ^= 0x01;
        let corrupt_payload: Vec<u8> = frame[HEADER_SIZE..].to_vec();
        assert!(!verify_frame(
            &frame[..HEADER_SIZE].try_into().unwrap(),
            &corrupt_payload
        ));
    }

    #[test]
    fn parse_rejects_truncated_and_bad_magic() {
        assert!(parse_header(&[0u8; 8]).is_none());
        assert!(parse_header(&[]).is_none());
        let mut h = hdr(MessageType::TaskCancel, 0);
        h.magic = 0x1234_5678;
        let frame = serialize_frame(h, &[]);
        assert!(parse_header(&frame).is_none());
    }

    #[test]
    fn encrypted_zero_checksum_fails_verify() {
        // Pinned by the `verify_encrypted_zero_checksum` golden vector: an
        // encrypted-style header (checksum 0) must not pass verify_frame.
        let payload = [9u8; 32];
        let mut h = hdr(MessageType::ClientConnectRequest, payload.len() as u32);
        h.checksum = 0;
        let frame = serialize_frame(h, &payload);
        let mut enc_hdr: [u8; HEADER_SIZE] = frame[..HEADER_SIZE].try_into().unwrap();
        enc_hdr[10..14].copy_from_slice(&0u32.to_le_bytes());
        assert!(!verify_frame(&enc_hdr, &payload));
    }
}
