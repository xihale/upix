//! Encrypted session — Rust port of
//! `share/core/distributed/crypto_session.{hpp,cpp}`.
//!
//! Wraps/unwraps secretbox-encrypted frames. Wire format:
//!
//! ```text
//! [MessageHeader(14B, type unchanged, payload_length = nonce+ct length,
//!                checksum forced to 0)]
//! [nonce(24B)][ciphertext(payload_len + 16B MAC)]
//! ```
//!
//! The checksum is zeroed and MUST stay unverified: a plaintext CRC over
//! ciphertext would leak byte information, and the Poly1305 MAC already
//! provides strong integrity. After `activate`, every plaintext
//! `frame::serialize_frame` output goes through `seal` to become wire bytes,
//! and wire bytes go through `open` to come back as plaintext header +
//! payload. The session is symmetric (one shared key both directions) — the
//! client/server roles exist only in the handshake that derives the key.

use zeroize::Zeroize;

use crate::crypto;
use crate::error::{CryptoError, Result};
use crate::frame::parse_header;
use crate::protocol::{MessageHeader, HEADER_SIZE};

pub type SessionKey = [u8; 32];

#[derive(Debug, Default)]
pub struct EncryptedSession {
    active: bool,
    key: SessionKey,
}

impl EncryptedSession {
    /// Activate with the X25519-derived session key. seal/open require this
    /// (the C++ asserts/throws when called before activation).
    pub fn activate(&mut self, key: &SessionKey) {
        self.key = *key;
        self.active = true;
    }

    pub fn active(&self) -> bool {
        self.active
    }

    /// seal: plaintext header + payload → full wire bytes.
    /// Mirrors `EncryptedSession::seal`: payload_length is rewritten to
    /// `24 + payload_len + 16` and checksum is forced to 0; the 14-byte
    /// header is packed little-endian, then `nonce||ct||mac` is appended.
    /// Errors when not activated (C++ throws `std::runtime_error`).
    pub fn seal(&self, header: MessageHeader, plaintext_payload: &[u8]) -> Result<Vec<u8>> {
        if !self.active {
            return Err(CryptoError::SessionNotActive);
        }
        let pt_len = plaintext_payload.len();
        let nonce_ct_len = 24 + pt_len + crypto::SECRETBOX_MAC_LEN;

        // 修改后的 header：payload_length 改为 nonce+ct 长度，checksum 置 0
        // (明文 CRC 会泄露信息，且 Poly1305 MAC 已提供强完整性校验)
        let mut out_hdr = header;
        out_hdr.payload_length = nonce_ct_len as u32;
        out_hdr.checksum = 0;

        let sealed = crypto::secretbox_seal(self.key_ref(), plaintext_payload)?;
        debug_assert_eq!(sealed.len(), nonce_ct_len);

        let mut wire = Vec::with_capacity(HEADER_SIZE + nonce_ct_len);
        wire.extend_from_slice(&out_hdr.magic.to_le_bytes());
        wire.extend_from_slice(&(out_hdr.msg_type as u16).to_le_bytes());
        wire.extend_from_slice(&out_hdr.payload_length.to_le_bytes());
        wire.extend_from_slice(&out_hdr.checksum.to_le_bytes());
        wire.extend_from_slice(&sealed);
        Ok(wire)
    }

    /// Deterministic reference path of `seal` with an explicit nonce. Byte
    /// layout identical to `seal`; used by the golden vectors (fixed key +
    /// nonce) and tests. Production traffic uses random nonces only.
    pub fn seal_with_nonce(
        &self,
        header: MessageHeader,
        plaintext_payload: &[u8],
        nonce: &[u8; 24],
    ) -> Result<Vec<u8>> {
        if !self.active {
            return Err(CryptoError::SessionNotActive);
        }
        let pt_len = plaintext_payload.len();
        let nonce_ct_len = 24 + pt_len + crypto::SECRETBOX_MAC_LEN;

        let mut out_hdr = header;
        out_hdr.payload_length = nonce_ct_len as u32;
        out_hdr.checksum = 0;

        let sealed = crypto::secretbox_seal_with_nonce(self.key_ref(), nonce, plaintext_payload)?;
        debug_assert_eq!(sealed.len(), nonce_ct_len);

        let mut wire = Vec::with_capacity(HEADER_SIZE + nonce_ct_len);
        wire.extend_from_slice(&out_hdr.magic.to_le_bytes());
        wire.extend_from_slice(&(out_hdr.msg_type as u16).to_le_bytes());
        wire.extend_from_slice(&out_hdr.payload_length.to_le_bytes());
        wire.extend_from_slice(&out_hdr.checksum.to_le_bytes());
        wire.extend_from_slice(&sealed);
        Ok(wire)
    }

    /// open: full wire → `(plaintext header, plaintext payload)`; `None` on
    /// MAC failure, short frame, length mismatch, or when not activated.
    /// Mirrors `EncryptedSession::open`, including restoring the original
    /// header (`payload_length` becomes the plaintext length again).
    pub fn open(&self, wire: &[u8]) -> Option<(MessageHeader, Vec<u8>)> {
        if !self.active {
            return None;
        }
        if wire.len() < HEADER_SIZE {
            return None;
        }

        let hdr = parse_header(wire)?;
        let nonce_ct_len = wire.len() - HEADER_SIZE;
        if hdr.payload_length as usize != nonce_ct_len {
            return None;
        }

        let plaintext = crypto::secretbox_open(self.key_ref(), &wire[HEADER_SIZE..])?;

        // 还原原始 header（payload_length 是明文长度）
        let mut real_hdr = hdr;
        real_hdr.payload_length = plaintext.len() as u32;
        Some((real_hdr, plaintext))
    }

    fn key_ref(&self) -> &[u8; 32] {
        &self.key
    }
}

impl Drop for EncryptedSession {
    fn drop(&mut self) {
        // Mirrors ~EncryptedSession: sodium_memzero(&key_).
        self.key.zeroize();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::MessageType;

    fn hdr(t: MessageType, len: u32) -> MessageHeader {
        MessageHeader {
            magic: crate::protocol::MAGIC,
            msg_type: t,
            payload_length: len,
            checksum: 99, // must be ignored/replaced by seal
        }
    }

    #[test]
    fn seal_open_roundtrip_restores_header() {
        let mut session = EncryptedSession::default();
        assert!(session.open(&[0u8; 64]).is_none());
        assert!(session.seal(hdr(MessageType::ClientSubmit, 4), &[1, 2, 3, 4]).is_err());

        session.activate(&[0x42; 32]);
        let payload = [1u8, 2, 3, 4];
        let wire = session
            .seal(hdr(MessageType::ClientSubmit, payload.len() as u32), &payload)
            .expect("active session");
        // checksum forced to 0 in the wire header.
        assert_eq!(&wire[10..14], &0u32.to_le_bytes());
        assert_eq!(wire.len(), HEADER_SIZE + 24 + payload.len() + 16);

        let (hdr2, pt) = session.open(&wire).expect("opens");
        assert_eq!(hdr2.msg_type, MessageType::ClientSubmit);
        assert_eq!(hdr2.payload_length as usize, payload.len());
        assert_eq!(pt, payload);
    }

    #[test]
    fn open_rejects_length_mismatch_and_garbage() {
        let mut session = EncryptedSession::default();
        session.activate(&[7; 32]);
        let payload = b"hello frame";
        let mut wire = session
            .seal(hdr(MessageType::ClientData, payload.len() as u32), payload)
            .unwrap();

        // Payload length field mismatch → None.
        wire[6] ^= 0x01;
        assert!(session.open(&wire).is_none());
        wire[6] ^= 0x01;

        // MAC corruption → None.
        let last = wire.len() - 1;
        wire[last] ^= 0x01;
        assert!(session.open(&wire).is_none());
        wire[last] ^= 0x01;
        assert!(session.open(&wire).is_some());
    }

    #[test]
    fn chunked_large_payload_roundtrip() {
        // 分块续帧：seal/open roundtrip over many CLIENT_DATA continuation
        // frames, mirroring how coordinator reassembles chunked submits.
        let mut session = EncryptedSession::default();
        session.activate(&[0xAB; 32]);
        let payload: Vec<u8> = (0..256 * 1024).map(|i| (i % 251) as u8).collect();
        let chunk = 64 * 1024usize;

        let mut reassembled = Vec::with_capacity(payload.len());
        for (idx, part) in payload.chunks(chunk).enumerate() {
            let wire = session
                .seal(hdr(MessageType::ClientData, part.len() as u32), part)
                .unwrap();
            assert!(wire.len() <= HEADER_SIZE + 24 + chunk + 16);
            let (h, pt) = session.open(&wire).unwrap();
            assert_eq!(h.msg_type, MessageType::ClientData);
            assert_eq!(h.payload_length as usize, pt.len());
            assert_eq!(pt, part);
            reassembled.extend_from_slice(&pt);
            let _ = idx;
        }
        assert_eq!(reassembled, payload);
    }

    #[test]
    fn seal_with_nonce_is_deterministic() {
        let mut session = EncryptedSession::default();
        session.activate(&[3; 32]);
        let mut nonce = [0u8; 24];
        nonce.copy_from_slice(&(0u32..24).map(|i| i as u8).collect::<Vec<u8>>());
        let payload = b"deterministic";
        let a = session
            .seal_with_nonce(hdr(MessageType::KeyHello, payload.len() as u32), payload, &nonce)
            .unwrap();
        let b = session
            .seal_with_nonce(hdr(MessageType::KeyHello, payload.len() as u32), payload, &nonce)
            .unwrap();
        assert_eq!(a, b);
    }
}
