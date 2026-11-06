//! Portability seams — the only place `upix-host` touches the sibling crates'
//! infrastructure APIs (transport, crypto session, tile pipeline, model
//! library, image codec).
//!
//! Rationale: upix-net / upix-core / upix-img are being ported in parallel.
//! `upix-host` codes against these narrow traits (whose shapes follow the
//! CONTRACT §5/§3/§4 pins) and ships std-based transport adapters plus mock
//! implementations for its own state-machine tests. When the sibling crates'
//! modules land, thin adapters in `adapters.rs` wire the traits to
//! `upix_net::{frame, crypto, session, identity, TrustStore, tcp, uds}` and
//! `upix_core::{process_image_u8, TileSizeOptimizer, ModelManager, config}`,
//! `upix_img::{decode, exif, sr_marker}` — no coordinator/serve logic changes.
//!
//! Contract deviations are tracked in the delivery report.

use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use upix_core::engine::{Engine, EngineError, ProcessingProgress, TileConfig};
use upix_net::protocol::{MessageHeader, MessageType};

pub use upix_net::protocol::{CLIENT_DATA_CHUNK_BYTES, HEADER_SIZE, MAX_NETWORK_PAYLOAD};

// ── transport ───────────────────────────────────────────────────────────────

/// Receive failure classification. `WouldBlock` maps to EAGAIN/EWOULDBLOCK:
/// a poll-tick timeout, not a failure (the coordinator's poll loop depends on
/// the distinction — a `WouldBlock` with zero bytes consumed is benign).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RecvErr {
    WouldBlock,
    Dead(String),
}

/// One connection. Interior-mutability + `&self` so the session pump thread
/// and the per-task workers can share it (C++ relies on StreamSocket's
/// separate rx/tx mutexes for exactly this sharing).
pub trait Transport: Send + Sync {
    fn send_all(&self, buf: &[u8]) -> Result<(), String>;
    fn set_recv_timeout_ms(&self, ms: i64);
    /// Single-shot receive: takes the rx lock once, so a worker's concurrent
    /// send interleaves between polls (the ~10 s convoy fix).
    fn receive(&self, out: &mut [u8]) -> Result<usize, RecvErr>;
    /// Loop `receive` until `out` is full or the peer dies. A `WouldBlock`
    /// with partial progress keeps going (the read timeout restarts per call
    /// — mirrors receive_all on a SO_RCVTIMEO socket).
    fn receive_exact(&self, out: &mut [u8]) -> Result<(), String> {
        let mut done = 0;
        while done < out.len() {
            match self.receive(&mut out[done..]) {
                Ok(0) => return Err("peer closed".into()),
                Ok(n) => done += n,
                Err(RecvErr::WouldBlock) => continue,
                Err(RecvErr::Dead(e)) => return Err(e),
            }
        }
        Ok(())
    }
    /// Wake a handler parked in a blocking receive (stop() path).
    fn shutdown(&self);
}

/// Listener poll result: `Ok(None)` = poll-tick timeout with no connection
/// (the accept loop re-checks the stop flag at least every ~500 ms).
pub trait Listener: Send + Sync {
    fn accept(&self) -> Result<Option<Box<dyn Transport>>, String>;
    /// Close the listening socket (accept() then errors promptly).
    fn shutdown(&self);
}

pub trait ListenerFactory: Send + Sync {
    fn listen(&self, port: u16) -> Result<Box<dyn Listener>, String>;
}

/// UDS listener factory (serve daemon). Separate trait: the daemon never
/// binds TCP.
pub trait UdsFactory: Send + Sync {
    fn listen(&self, socket_path: &Path) -> Result<Box<dyn Listener>, String>;
    fn connect(&self, socket_path: &Path) -> Result<Box<dyn Transport>, String>;
}

// ── crypto / session ────────────────────────────────────────────────────────

/// Authenticated frame codec (mirror of `EncryptedSession`): seal/open whole
/// wire frames. Stateless per call (secretbox nonces are random per seal) so
/// `&self` is shareable across the session's worker threads.
pub trait SessionCodec: Send + Sync {
    fn seal(&self, ty: MessageType, payload: &[u8]) -> Option<Vec<u8>>;
    fn open(&self, wire: &[u8]) -> Option<(MessageHeader, Vec<u8>)>;
}

pub trait CryptoProvider: Send + Sync {
    /// Fresh X25519 keypair `(secret, public)`.
    fn x25519_keygen(&self) -> ([u8; 32], [u8; 32]);
    fn x25519_shared(&self, secret: &[u8; 32], peer_public: &[u8; 32]) -> [u8; 32];
    fn ed25519_verify(&self, public: &[u8; 32], sig: &[u8; 64], msg: &[u8]) -> bool;
    /// BLAKE2b(personalized) session key derivation + codec construction —
    /// byte-exactness is upix-net's golden-vector contract.
    fn new_session(&self, key: &[u8; 32]) -> Box<dyn SessionCodec>;
    /// Peer fingerprint: `sha256(pubkey)[..8]` uppercase hex — 16 chars.
    fn fingerprint(&self, public: &[u8; 32]) -> String;
    /// Per-connection diagnostic id (C++: `generate_uuid().substr(0, 35)`).
    fn new_device_id(&self) -> String;
}

/// The coordinator's long-term Ed25519 identity (signs the handshake
/// transcript; `fingerprint()` feeds the connection card).
pub trait Signer: Send + Sync {
    fn public_key(&self) -> [u8; 32];
    fn sign(&self, msg: &[u8]) -> [u8; 64];
    fn fingerprint(&self) -> String;
}

/// TrustStore of explicitly approved phone identities. `is_trusted` consults
/// the loaded store (a missing store leaves everything untrusted);
/// `approve_and_save` is only called for affirmative, delivered approvals.
pub trait TrustApi: Send + Sync {
    fn is_trusted(&self, key: &[u8; 32]) -> bool;
    fn approve_and_save(&self, key: &[u8; 32]) -> bool;
}

// ── image / model / pipeline backend ────────────────────────────────────────

/// Interleaved u8 image (mirror of `upix_img::Image`; kept local so mocks can
/// construct it without depending on img's decode stack).
#[derive(Debug, Clone)]
pub struct Img {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct ModelInfo {
    pub name: String,
    pub scale_factor: i32,
    pub total_size: u64,
    pub param_path: PathBuf,
    pub bin_path: PathBuf,
    pub input_channels: i32,
}

/// Bundle of everything the host needs from upix-core / upix-img. One trait
/// object per Coordinator/ServeDaemon keeps the constructors pinned
/// (CONTRACT §6: `Coordinator::new(bind_addr, port, models_dir,
/// engine_factory, auto_approve)` — the factory rides in the backend).
pub trait Backend: Send + Sync {
    // engine factory (engine_setup: GPU_THEN_CPU for the coordinator)
    fn create_engine(&self) -> Result<Box<dyn Engine>, String>;

    /// TileProcessor::process_image_u8 — the shared three-stage pipeline
    /// (GPU fast path → OOM tile ladder → generic pipeline). `input` is
    /// width*height*channels u8; returns the scaled raw pixels
    /// (out_w*out_h*channels). `enable_alpha_bicubic` mirrors the C++ flag
    /// (RGBA + 3-channel model → bicubic the alpha plane host-side).
    #[allow(clippy::too_many_arguments)]
    fn process_image_u8(
        &self,
        engine: &dyn Engine,
        tc: TileConfig,
        input: &[u8],
        width: i32,
        height: i32,
        channels: i32,
        progress: Option<&mut dyn FnMut(&ProcessingProgress)>,
        cancel: &AtomicBool,
        enable_alpha_bicubic: bool,
    ) -> Result<Vec<u8>, EngineError>;

    /// TileSizeOptimizer probe for tile_size=0 remote tasks. Implementations
    /// keep the process-level per-model cache (each model measured once per
    /// process). Returns ≤0 when the probe cannot produce a usable size (the
    /// caller falls back to the engine recommendation).
    #[allow(clippy::too_many_arguments)]
    fn auto_tile_size(
        &self,
        engine: &dyn Engine,
        model_name: &str,
        min_tile: i32,
        max_tile: i32,
        tile_overlap: i32,
        width: i32,
        height: i32,
    ) -> i32;

    // model library (ModelManager)
    fn models_dir(&self) -> PathBuf;
    /// (Re)scan the given directory (coordinator_admin's
    /// set_models_directory semantics: the effective dir is passed in by the
    /// caller, adapters must not cache a stale one).
    fn scan_models(&self, dir: &Path) -> Result<(), String>;
    fn available_models(&self) -> Vec<ModelInfo>;
    fn get_model(&self, name: &str) -> Option<ModelInfo>;

    // image ops (upix-img)
    fn exif_orientation(&self, bytes: &[u8]) -> i32;
    fn decode_image(&self, format: &str, bytes: &[u8]) -> Result<Img, String>;
    /// Normalize gray/other channel counts toward the model's input channels.
    /// Returns false on failure ("channel conversion failed").
    fn normalize_channels(&self, img: &mut Img, expected_channels: i32) -> bool;
    /// EXIF orientation bake (rotations 5-8 swap w/h). Returns false for
    /// unsupported/identity orientations.
    fn apply_orientation(&self, img: &mut Img, orientation: i32) -> bool;
    fn encode_image(
        &self,
        img: &Img,
        format: &str,
        quality: i32,
        xmp: Option<&str>,
        opts: &upix_img::EncoderOptions,
    ) -> Result<Vec<u8>, String>;
    fn save_image(
        &self,
        img: &Img,
        path: &Path,
        quality: i32,
        xmp: Option<&str>,
        opts: &upix_img::EncoderOptions,
    ) -> Result<(), String>;
    fn load_raw(&self, path: &Path) -> Result<Vec<u8>, String>;
    fn load_from_memory(&self, ext: &str, bytes: &[u8]) -> Result<Img, String>;
    /// Cached dlopen probes: (avif_decode, avif_encode).
    fn avif_available(&self) -> (bool, bool);
    fn is_supported_output_format(&self, f: &str) -> bool;
    fn normalize_output_format(&self, f: &str) -> String;
    fn sr_xmp(&self, model: &str, scale: i32, ts: i64) -> String;

    // config helpers (upix-core::config)
    fn sanitize_model_for_filename(&self, name: &str) -> String;
    /// Output-path derivation (`resolve_output_path`): missing extension /
    /// "output dir" defaults applied exactly like the CLI.
    fn resolve_output_path(&self, requested: &Path, output_format: &str) -> PathBuf;
}
