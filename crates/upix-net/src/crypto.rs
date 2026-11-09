//! Sodium-equivalent crypto — Rust port of `engine/utils/crypto_sodium.cpp`.
//!
//! Wire compatibility with libsodium is contractual and proven by golden
//! vectors (tests/golden/):
//! - X25519 shared point → BLAKE2b-256 **keyed** with the 12-byte domain
//!   separator `upix-ecdh-v1` (the C++ passes it as the
//!   `crypto_generichash_blake2b` key, NOT as a personalization string) →
//!   32-byte session key.
//! - Ed25519 detached signatures (RFC 8032, deterministic — libsodium
//!   `crypto_sign_detached` and ed25519-dalek produce identical bytes).
//! - crypto_secretbox (XSalsa20-Poly1305) via the `crypto_secretbox`
//!   crate, byte-compatible with `crypto_secretbox_easy`.

use blake2::digest::consts::U32;
use blake2::digest::{Mac, Update};
use blake2::Blake2bMac;
use crypto_secretbox::aead::{Aead, KeyInit, Payload};
use crypto_secretbox::{Key, Nonce, XSalsa20Poly1305};
use ed25519_dalek::{Signer, Verifier};
use rand_core::{OsRng, RngCore};
use sha2::{Digest, Sha256};
use x25519_dalek::{PublicKey, StaticSecret};

use crate::error::{CryptoError, Result};

/// BLAKE2b key (domain separator) copied byte-for-byte from
/// `crypto_sodium.cpp` (`static const char ctx[] = "upix-ecdh-v1"`).
const ECDH_DOMAIN: &[u8] = b"upix-ecdh-v1";

pub const SESSION_KEY_LEN: usize = 32;
pub const ED25519_PUBKEY_LEN: usize = 32;
/// libsodium Ed25519 secret key format: seed(32) || pk(32).
pub const ED25519_SECRETKEY_LEN: usize = 64;
pub const SECRETBOX_NONCE_LEN: usize = 24;
pub const SECRETBOX_MAC_LEN: usize = 16;

// ── X25519 (Curve25519 ECDH) ────────────────────────────────────────────────

/// Random X25519 keypair (mirrors `x25519_keygen` → `crypto_box_keypair`).
pub fn x25519_keygen() -> ([u8; 32], [u8; 32]) {
    let sk = StaticSecret::random_from_rng(OsRng);
    let pk = PublicKey::from(&sk);
    (*pk.as_bytes(), *sk.as_bytes())
}

/// Public key for a private key (mirrors `crypto_scalarmult_base`; both
/// libsodium and x25519-dalek clamp the scalar per RFC 7748, so the derived
/// public keys are identical — pinned by the session_key golden vector).
pub fn x25519_public_from_private(my_private: &[u8; 32]) -> [u8; 32] {
    let sk = StaticSecret::from(*my_private);
    *PublicKey::from(&sk).as_bytes()
}

/// Derive the session key: raw X25519 shared point hardened through
/// BLAKE2b-256 keyed with the domain separator, usable directly as a
/// secretbox symmetric key (mirrors `x25519_shared`).
///
/// Errors on a non-contributory (low-order) peer point, matching the C++
/// `crypto_scalarmult_curve25519` failure path.
pub fn x25519_shared(
    my_private: &[u8; 32],
    peer_public: &[u8; 32],
) -> Result<[u8; SESSION_KEY_LEN]> {
    let sk = StaticSecret::from(*my_private);
    let pk = PublicKey::from(*peer_public);
    let shared = sk.diffie_hellman(&pk);
    if !shared.was_contributory() {
        return Err(CryptoError::LowOrderPoint);
    }
    let raw = shared.as_bytes();

    // Keyed BLAKE2b-256 — identical to libsodium's
    // crypto_generichash_blake2b(out, 32, raw, 32, ctx, 12).
    let mut mac = <Blake2bMac<U32> as Mac>::new_from_slice(ECDH_DOMAIN)
        .map_err(|_| CryptoError::Blake2bKey)?;
    Update::update(&mut mac, raw);
    let out = Mac::finalize(mac).into_bytes();
    let mut key = [0u8; SESSION_KEY_LEN];
    key.copy_from_slice(&out);
    Ok(key)
}

// ── Ed25519 (signing identity) ──────────────────────────────────────────────

/// Deterministic Ed25519 keypair from a 32-byte seed. Returns
/// `(public_key, secret_key)` with the libsodium secret layout
/// `seed(32) || pk(32)` (mirrors `crypto_sign_seed_keypair`).
pub fn ed25519_seed_keypair(seed: &[u8; 32]) -> ([u8; 32], [u8; ED25519_SECRETKEY_LEN]) {
    let sk = ed25519_dalek::SigningKey::from_bytes(seed);
    let pk = sk.verifying_key();
    let mut secret = [0u8; ED25519_SECRETKEY_LEN];
    secret[..32].copy_from_slice(seed);
    secret[32..].copy_from_slice(pk.as_bytes());
    (*pk.as_bytes(), secret)
}

/// Random Ed25519 keypair, same `(pk, sk=seed||pk)` layout as
/// `ed25519_keygen` → `crypto_sign_keypair`.
pub fn ed25519_keygen() -> ([u8; 32], [u8; ED25519_SECRETKEY_LEN]) {
    let mut seed = [0u8; 32];
    OsRng.fill_bytes(&mut seed);
    ed25519_seed_keypair(&seed)
}

/// Ed25519 detached signature (mirrors `ed25519_sign` →
/// `crypto_sign_detached`; RFC 8032 signing is deterministic, so the bytes
/// match libsodium exactly for the same seed + message).
pub fn ed25519_sign(
    private_key: &[u8; ED25519_SECRETKEY_LEN],
    msg: &[u8],
) -> [u8; 64] {
    // libsodium derives the signing scalar from the seed (first 32 bytes of
    // the secret key).
    let mut seed = [0u8; 32];
    seed.copy_from_slice(&private_key[..32]);
    let sk = ed25519_dalek::SigningKey::from_bytes(&seed);
    sk.sign(msg).to_bytes()
}

/// Ed25519 detached verification (mirrors `ed25519_verify`; returns false on
/// any malformed key/signature, matching `crypto_sign_verify_detached != 0`).
pub fn ed25519_verify(
    public_key: &[u8; 32],
    sig: &[u8; 64],
    msg: &[u8],
) -> bool {
    let Ok(vk) = ed25519_dalek::VerifyingKey::from_bytes(public_key) else {
        return false;
    };
    let signature = ed25519_dalek::Signature::from_bytes(sig);
    vk.verify(msg, &signature).is_ok()
}

// ── crypto_secretbox (XSalsa20-Poly1305) ──────────────────────────────────

/// Wire layout mirrors `crypto_secretbox_easy`: nonce(24) || ciphertext ||
/// 16-byte Poly1305 MAC. `aad` is always empty in this protocol. The AEAD
/// never fails for well-formed inputs; the `Result` exists to keep the
/// library panic-free.
fn secretbox_encrypt(
    key: &[u8; 32],
    nonce: &[u8; SECRETBOX_NONCE_LEN],
    plaintext: &[u8],
) -> Result<Vec<u8>> {
    let cipher = XSalsa20Poly1305::new(Key::from_slice(key));
    let payload = Payload {
        msg: plaintext,
        aad: &[],
    };
    cipher
        .encrypt(&Nonce::from_slice(nonce), payload)
        .map_err(|_| CryptoError::Secretbox)
}

/// seal: output `nonce(24) || ciphertext || mac(16)` with a random nonce
/// (mirrors `secretbox_seal`; randombytes_buf → OsRng).
pub fn secretbox_seal(key: &[u8; 32], plaintext: &[u8]) -> Result<Vec<u8>> {
    let mut nonce = [0u8; SECRETBOX_NONCE_LEN];
    OsRng.fill_bytes(&mut nonce);
    let ct = secretbox_encrypt(key, &nonce, plaintext)?;
    let mut out = Vec::with_capacity(SECRETBOX_NONCE_LEN + plaintext.len() + SECRETBOX_MAC_LEN);
    out.extend_from_slice(&nonce);
    out.extend_from_slice(&ct);
    Ok(out)
}

/// seal with an explicit nonce: identical bytes to `secretbox_seal` modulo
/// the nonce source. Exists for the golden vectors (deterministic seal bytes)
/// and for tests; production traffic must use `secretbox_seal` (fresh random
/// nonces), matching the C++ behavior of always calling `randombytes_buf`.
pub fn secretbox_seal_with_nonce(
    key: &[u8; 32],
    nonce: &[u8; SECRETBOX_NONCE_LEN],
    plaintext: &[u8],
) -> Result<Vec<u8>> {
    let ct = secretbox_encrypt(key, nonce, plaintext)?;
    let mut out = Vec::with_capacity(SECRETBOX_NONCE_LEN + plaintext.len() + SECRETBOX_MAC_LEN);
    out.extend_from_slice(nonce);
    out.extend_from_slice(&ct);
    Ok(out)
}

/// open: input `nonce(24) || ciphertext || mac(16)`, plaintext on success;
/// `None` on MAC failure or a too-short input (mirrors `secretbox_open`,
/// minimum `NONCE + MAC` bytes).
pub fn secretbox_open(key: &[u8; 32], nonce_ct: &[u8]) -> Option<Vec<u8>> {
    if nonce_ct.len() < SECRETBOX_NONCE_LEN + SECRETBOX_MAC_LEN {
        return None;
    }
    let mut nonce = [0u8; SECRETBOX_NONCE_LEN];
    nonce.copy_from_slice(&nonce_ct[..SECRETBOX_NONCE_LEN]);
    let cipher = XSalsa20Poly1305::new(key.into());
    let payload = Payload {
        msg: &nonce_ct[SECRETBOX_NONCE_LEN..],
        aad: &[],
    };
    cipher.decrypt(&Nonce::from_slice(&nonce), payload).ok()
}

// ── misc (engine/utils/crypto.cpp) ───────────────────────────────────────────

/// SHA-256, lowercase hex (mirrors `crypto::sha256_hex`).
pub fn sha256_hex(data: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let hash = Sha256::digest(data);
    let mut out = String::with_capacity(hash.len() * 2);
    for b in hash {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0xF) as usize] as char);
    }
    out
}

/// RFC 4122 v4 UUID, 36 chars (mirrors `crypto::generate_uuid`).
pub fn generate_uuid() -> String {
    let mut bytes = [0u8; 16];
    OsRng.fill_bytes(&mut bytes);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixed32(base: u8) -> [u8; 32] {
        let mut k = [0u8; 32];
        for (i, b) in k.iter_mut().enumerate() {
            *b = base + i as u8;
        }
        k
    }

    #[test]
    fn session_keys_agree() {
        let c = fixed32(0xA0);
        let s = fixed32(0xC0);
        let c_pub = x25519_public_from_private(&c);
        let s_pub = x25519_public_from_private(&s);
        let ck = x25519_shared(&c, &s_pub).expect("contributory");
        let sk = x25519_shared(&s, &c_pub).expect("contributory");
        assert_eq!(ck, sk);
    }

    #[test]
    fn secretbox_roundtrip_and_reject() {
        let key = fixed32(0x01);
        let sealed = secretbox_seal(&key, b"attack at dawn").expect("seal");
        assert_eq!(sealed.len(), SECRETBOX_NONCE_LEN + 14 + SECRETBOX_MAC_LEN);
        assert_eq!(secretbox_open(&key, &sealed).as_deref(), Some(&b"attack at dawn"[..]));
        // Bit flip → MAC failure → None.
        let mut corrupt = sealed.clone();
        let last = corrupt.len() - 1;
        corrupt[last] ^= 0x01;
        assert_eq!(secretbox_open(&key, &corrupt), None);
        // Too short → None.
        assert_eq!(secretbox_open(&key, &[0u8; 10]), None);
        // Wrong key → None.
        assert_eq!(secretbox_open(&fixed32(0x02), &sealed), None);
    }

    #[test]
    fn ed25519_deterministic_and_verified() {
        let (pk, sk) = ed25519_seed_keypair(&fixed32(0x30));
        let sig = ed25519_sign(&sk, b"transcript");
        assert_eq!(sig.len(), 64);
        assert!(ed25519_verify(&pk, &sig, b"transcript"));
        assert!(!ed25519_verify(&pk, &sig, b"tampered"));
        let mut bad = sig;
        bad[0] ^= 1;
        assert!(!ed25519_verify(&pk, &bad, b"transcript"));
    }

    #[test]
    fn uuid_shape() {
        let id = generate_uuid();
        assert_eq!(id.len(), 36);
        assert_eq!(id.as_bytes()[14], b'4'); // version nibble
        let variant = id.as_bytes()[19];
        assert!((b'8'..=b'f').contains(&variant));
    }
}
