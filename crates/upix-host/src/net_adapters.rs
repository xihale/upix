//! Real-crypto adapters — thin `upix_net` implementations of the [`seams`]
//! traits. One copy shared by `upix-cli` (`host start`), `upix-gui`
//! (integration feature) and the crypto E2E tests; the seams crate stays
//! transport-agnostic, this module owns the production wiring:
//! - framing: checksummed plaintext frames pre-handshake, MAC'd sealed
//!   frames after ([`NetFramer`] + [`NetCodec`]);
//! - primitives: X25519 + Ed25519 + crypto_secretbox, libsodium wire format
//!   proven by `upix-net` golden vectors ([`NetCrypto`]);
//! - identity + trust store at `~/.upix/` ([`NetSigner`], [`NetTrust`]).

use std::sync::{Arc, Mutex};

use upix_net::frame::{parse_header, serialize_frame, verify_frame};
use upix_net::protocol::{MessageHeader, MessageType, HEADER_SIZE};
use upix_net::{crypto, EncryptedSession, TrustStore};

use crate::coordinator::Framer;
use crate::seams::{CryptoProvider, SessionCodec, Signer, TrustApi};

/// Wire framing over the net crate (plaintext frames enforce the header
/// checksum; sealed frames skip it — Poly1305 covers integrity).
#[derive(Debug, Clone, Copy, Default)]
pub struct NetFramer;

impl Framer for NetFramer {
    fn serialize_frame(&self, ty: MessageType, payload: &[u8]) -> Vec<u8> {
        serialize_frame(
            MessageHeader {
                magic: upix_net::protocol::MAGIC,
                msg_type: ty,
                payload_length: payload.len() as u32,
                checksum: 0, // filled by serialize
            },
            payload,
        )
    }
    fn parse_header(&self, buf: &[u8]) -> Option<MessageHeader> {
        parse_header(buf)
    }
    fn verify_frame(&self, header: &[u8; HEADER_SIZE], payload: &[u8]) -> bool {
        verify_frame(header, payload)
    }
}

/// Crypto over the net crate's golden-vector functions.
#[derive(Debug, Clone, Copy, Default)]
pub struct NetCrypto;

impl CryptoProvider for NetCrypto {
    fn x25519_keygen(&self) -> ([u8; 32], [u8; 32]) {
        crypto::x25519_keygen()
    }
    fn x25519_shared(&self, secret: &[u8; 32], peer_public: &[u8; 32]) -> [u8; 32] {
        crypto::x25519_shared(secret, peer_public).unwrap_or([0u8; 32])
    }
    fn ed25519_verify(&self, public: &[u8; 32], sig: &[u8; 64], msg: &[u8]) -> bool {
        crypto::ed25519_verify(public, sig, msg)
    }
    fn new_session(&self, key: &[u8; 32]) -> Box<dyn SessionCodec> {
        let mut session = EncryptedSession::default();
        session.activate(key);
        Box::new(NetCodec { session })
    }
    fn fingerprint(&self, public: &[u8; 32]) -> String {
        upix_net::fingerprint(public)
    }
    fn new_device_id(&self) -> String {
        crypto::generate_uuid().chars().take(35).collect()
    }
}

/// Session codec over `EncryptedSession` (same wire: [14B header]
/// [nonce 24B][ciphertext+MAC]).
pub struct NetCodec {
    session: EncryptedSession,
}

impl NetCodec {
    pub fn new(session: EncryptedSession) -> Self {
        Self { session }
    }
}

impl SessionCodec for NetCodec {
    fn seal(&self, ty: MessageType, payload: &[u8]) -> Option<Vec<u8>> {
        let header = MessageHeader {
            magic: upix_net::protocol::MAGIC,
            msg_type: ty,
            payload_length: payload.len() as u32,
            checksum: 0,
        };
        self.session.seal(header, payload).ok()
    }
    fn open(&self, wire: &[u8]) -> Option<(MessageHeader, Vec<u8>)> {
        self.session.open(wire)
    }
}

/// The host's Ed25519 identity (default file: `~/.upix/identity`).
pub struct NetSigner {
    pub identity: upix_net::Identity,
}

impl Signer for NetSigner {
    fn public_key(&self) -> [u8; 32] {
        self.identity.public_key()
    }
    fn sign(&self, msg: &[u8]) -> [u8; 64] {
        self.identity.sign(msg)
    }
    fn fingerprint(&self) -> String {
        self.identity.fingerprint()
    }
}

/// TrustApi over the net TrustStore (default file: `~/.upix/trusted_clients`).
pub struct NetTrust {
    pub store: Mutex<TrustStore>,
}

impl NetTrust {
    pub fn new(store: TrustStore) -> Self {
        Self { store: Mutex::new(store) }
    }
}

impl TrustApi for NetTrust {
    fn is_trusted(&self, key: &[u8; 32]) -> bool {
        self.store.lock().unwrap().contains(key)
    }
    fn approve_and_save(&self, key: &[u8; 32]) -> bool {
        let mut store = self.store.lock().unwrap();
        store.add(key);
        store.save().is_ok()
    }
}

/// Convenience bundle for callers that want the full production crypto
/// stack (identity + trust store on disk, crypto providers in memory).
pub struct NetCryptoStack {
    pub framer: Arc<NetFramer>,
    pub crypto: Arc<NetCrypto>,
    pub signer: NetSigner,
    pub trust: Arc<NetTrust>,
}

impl NetCryptoStack {
    /// Loads (or creates) the host identity + trust store under `dir`
    /// (production callers pass `~/.upix`; tests pass a temp dir).
    pub fn open(dir: &std::path::Path) -> Result<Self, upix_net::IdentityError> {
        let identity = upix_net::Identity::load_or_create(&dir.join("identity"))?;
        let trust = TrustStore::load(&dir.join("trusted_clients"));
        Ok(Self {
            framer: Arc::new(NetFramer),
            crypto: Arc::new(NetCrypto),
            signer: NetSigner { identity },
            trust: Arc::new(NetTrust::new(trust)),
        })
    }
}
