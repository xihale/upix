//! Shared test doubles: in-memory mock implementations of the seams traits
//! (crypto/transport/backend), the coordinator client harness (drives the
//! real wire flow through a mock crypto session), and a deterministic
//! MockEngine implementing the real `upix_core::Engine` trait.
//!
//! The mock "crypto" seals frames as plaintext pass-through (header +
//! payload) so the full state machines (pairing gate, dual submit formats,
//! chunked reassembly, cancel, per-task errors) run end-to-end without the
//! sibling crates' crypto stack. Byte-level wire compat is upix-net's
//! golden-vector territory, not this crate's.

#![allow(dead_code)]

use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};

use upix_core::engine::{DeviceType, Engine, EngineError, ProcessingProgress};
use upix_net::protocol::{MessageHeader, MessageType, HEADER_SIZE, MAX_NETWORK_PAYLOAD};

use upix_host::coordinator::Framer;
use upix_host::seams::{
    Backend, CryptoProvider, SessionCodec, Signer, Transport, TrustApi,
};
use upix_host::wireio;

// ── MockEngine (implements the REAL upix-core Engine trait) ────────────────

/// Deterministic x2/x4 nearest-neighbor upscale on `process_u8`, with a
/// cancel gate that stops between "tiles" (rows blocks) so the coordinator's
/// cancel path is exercised realistically.
pub struct MockEngine {
    scale: i32,
    name: String,
    /// Sleep per simulated tile (cancellation is polled between tiles).
    pub tile_delay_ms: u64,
}

impl MockEngine {
    pub fn new(scale: i32, name: &str) -> Self {
        Self {
            scale,
            name: name.to_string(),
            tile_delay_ms: 0,
        }
    }
}

impl Engine for MockEngine {
    fn initialize(&mut self, _preferred: DeviceType) -> Result<(), EngineError> {
        Ok(())
    }
    fn list_devices(&self) -> Vec<upix_core::engine::DeviceCapability> {
        Vec::new()
    }
    /// Scale follows the currently "loaded" model file (Mock-x2 / Mock-x4 —
    /// the daemon's lazy-switch test relies on this reflecting load_model).
    fn load_model(&mut self, param: &Path, _bin: &Path) -> Result<(), EngineError> {
        let name = param
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        self.name = name.clone();
        self.scale = match name.rsplit('x').next().and_then(|s| s.parse::<i32>().ok()) {
            Some(n) if n > 0 => n,
            _ => 2,
        };
        Ok(())
    }
    fn process(
        &self,
        _input: &[f32],
        _output: &mut [f32],
        _width: i32,
        _height: i32,
        _channels: i32,
        _cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        Err(EngineError::new(102, "float path not supported"))
    }
    fn process_u8(
        &self,
        input: &[u8],
        output: &mut [u8],
        width: i32,
        height: i32,
        channels: i32,
        cancel: &AtomicBool,
    ) -> Result<(), EngineError> {
        // Nearest-neighbor scale, simulated in tiles of 8 rows with cancel
        // checks + optional delay between them (tile-boundary abort).
        let rows_per_tile = 8;
        let mut y = 0;
        while y < height {
            if cancel.load(Ordering::Relaxed) {
                return Err(EngineError::new(7, "canceled"));
            }
            if self.tile_delay_ms > 0 {
                std::thread::sleep(std::time::Duration::from_millis(self.tile_delay_ms));
            }
            let y_end = (y + rows_per_tile).min(height);
            for sy in y..y_end {
                let src_y = (sy / self.scale).min(height - 1);
                for dx in 0..(width * self.scale) {
                    let src_x = (dx / self.scale).min(width - 1);
                    for c in 0..channels {
                        let src_i =
                            (src_y * width + src_x) as usize * channels as usize + c as usize;
                        let dst_i = (sy as i64 * width as i64 * self.scale as i64 + dx as i64)
                            as usize
                            * channels as usize
                            + c as usize;
                        if dst_i < output.len() && src_i < input.len() {
                            output[dst_i] = input[src_i];
                        }
                    }
                }
            }
            y = y_end;
        }
        Ok(())
    }
    fn benchmark(&self, _test_tile_size: i32) -> f32 {
        1.0
    }
    fn unload_model(&mut self) {}
    fn current_device(&self) -> DeviceType {
        DeviceType::Cpu
    }
    fn is_initialized(&self) -> bool {
        true
    }
    fn model_name(&self) -> String {
        self.name.clone()
    }
    fn get_scale_factor(&self) -> i32 {
        self.scale
    }
    fn get_recommended_tile_size(&self) -> i32 {
        256
    }
    fn hardware_id(&self) -> String {
        "mock".to_string()
    }
}

// ── plumbing doubles ────────────────────────────────────────────────────────

/// Plaintext pass-through "session": seal = serialize frame, open = parse.
/// Same shape as EncryptedSession for the coordinator's logic.
#[derive(Default)]
pub struct PlainSession;

impl SessionCodec for PlainSession {
    fn seal(&self, ty: MessageType, payload: &[u8]) -> Option<Vec<u8>> {
        Some(PlainFramer.serialize_frame(ty, payload))
    }
    fn open(&self, wire: &[u8]) -> Option<(MessageHeader, Vec<u8>)> {
        if wire.len() < HEADER_SIZE {
            return None;
        }
        let hdr = PlainFramer.parse_header(&wire[..HEADER_SIZE])?;
        let payload = wire[HEADER_SIZE..].to_vec();
        if payload.len() as u32 != hdr.payload_length {
            return None;
        }
        Some((hdr, payload))
    }
}

pub struct PlainFramer;

impl Framer for PlainFramer {
    fn serialize_frame(&self, ty: MessageType, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(HEADER_SIZE + payload.len());
        out.extend_from_slice(&upix_net::protocol::MAGIC.to_le_bytes());
        out.extend_from_slice(&(ty as u16).to_le_bytes());
        out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        // checksum: simple fletcher-ish so verify_frame has something to check
        let mut sum: u32 = 0;
        for &b in payload {
            sum = sum.wrapping_mul(31).wrapping_add(b as u32);
        }
        out.extend_from_slice(&sum.to_le_bytes());
        out.extend_from_slice(payload);
        out
    }
    fn parse_header(&self, buf: &[u8]) -> Option<MessageHeader> {
        if buf.len() < HEADER_SIZE {
            return None;
        }
        let magic = u32::from_le_bytes(buf[0..4].try_into().ok()?);
        if magic != upix_net::protocol::MAGIC {
            return None;
        }
        let t = u16::from_le_bytes(buf[4..6].try_into().ok()?);
        Some(MessageHeader {
            magic,
            msg_type: MessageType::from_u16(t)?,
            payload_length: u32::from_le_bytes(buf[6..10].try_into().ok()?),
            checksum: u32::from_le_bytes(buf[10..14].try_into().ok()?),
        })
    }
    fn verify_frame(&self, header: &[u8; HEADER_SIZE], payload: &[u8]) -> bool {
        let sum = u32::from_le_bytes(header[10..14].try_into().unwrap());
        let mut calc: u32 = 0;
        for &b in payload {
            calc = calc.wrapping_mul(31).wrapping_add(b as u32);
        }
        sum == calc
    }
}

/// Deterministic crypto mock: fixed keypairs, verify == signature equality.
pub struct MockCrypto;

impl CryptoProvider for MockCrypto {
    /// (public, private) — same order as the real `crypto::x25519_keygen`
    /// (and the C++ X25519Keypair struct).
    fn x25519_keygen(&self) -> ([u8; 32], [u8; 32]) {
        static COUNTER: AtomicUsize = AtomicUsize::new(1);
        let n = COUNTER.fetch_add(1, Ordering::SeqCst) as u8;
        let mut privk = [n; 32];
        privk[0] = 0xEE;
        let mut pubk = [n; 32];
        pubk[0] = 0xDD;
        (pubk, privk)
    }
    fn x25519_shared(&self, _secret: &[u8; 32], _peer: &[u8; 32]) -> [u8; 32] {
        [0x5A; 32]
    }
    fn ed25519_verify(&self, public: &[u8; 32], sig: &[u8; 64], msg: &[u8]) -> bool {
        // Deterministic check: sig[0..32] == public XOR msg[0..32]; exercises
        // the rejection path when the client signs the wrong transcript.
        let mut expect = [0u8; 32];
        for i in 0..32 {
            expect[i] = public[i] ^ msg.get(i).copied().unwrap_or(0);
        }
        &sig[0..32] == &expect
    }
    fn new_session(&self, _key: &[u8; 32]) -> Box<dyn SessionCodec> {
        Box::new(PlainSession)
    }
    fn fingerprint(&self, public: &[u8; 32]) -> String {
        public.iter().take(8).map(|b| format!("{b:02X}")).collect()
    }
    fn new_device_id(&self) -> String {
        static COUNTER: AtomicUsize = AtomicUsize::new(1);
        let n = COUNTER.fetch_add(1, Ordering::SeqCst);
        format!("mock-device-{n}")
    }
}

pub struct MockSigner {
    pub key: [u8; 32],
}

impl Signer for MockSigner {
    fn public_key(&self) -> [u8; 32] {
        self.key
    }
    fn sign(&self, msg: &[u8]) -> [u8; 64] {
        let mut sig = [0u8; 64];
        for i in 0..32 {
            sig[i] = self.key[i] ^ msg.get(i).copied().unwrap_or(0);
        }
        sig
    }
    fn fingerprint(&self) -> String {
        self.key.iter().take(8).map(|b| format!("{b:02X}")).collect()
    }
}

#[derive(Default)]
pub struct MockTrust {
    trusted: Mutex<Vec<[u8; 32]>>,
}

impl MockTrust {
    pub fn with_trusted(key: [u8; 32]) -> Self {
        Self {
            trusted: Mutex::new(vec![key]),
        }
    }
    pub fn contains(&self, key: &[u8; 32]) -> bool {
        self.trusted.lock().unwrap().contains(key)
    }
}

impl TrustApi for MockTrust {
    fn is_trusted(&self, key: &[u8; 32]) -> bool {
        self.trusted.lock().unwrap().contains(key)
    }
    fn approve_and_save(&self, key: &[u8; 32]) -> bool {
        self.trusted.lock().unwrap().push(*key);
        true
    }
}

/// Full-duplex in-memory pipe between a client test harness and the server
/// side. Receive timeouts + shutdown semantics mirror the socket adapters.
pub struct TestPipe {
    inner: Arc<PipeInner>,
}

struct PipeInner {
    rx: Mutex<VecDeque<u8>>,
    cv: Condvar,
    timeout: Mutex<i64>,
    closed: AtomicBool,
    peer: Mutex<Option<Arc<PipeInner>>>,
}

impl TestPipe {
    pub fn pair() -> (TestPipe, TestPipe) {
        let a = Arc::new(PipeInner {
            rx: Mutex::new(VecDeque::new()),
            cv: Condvar::new(),
            timeout: Mutex::new(0),
            closed: AtomicBool::new(false),
            peer: Mutex::new(None),
        });
        let b = Arc::new(PipeInner {
            rx: Mutex::new(VecDeque::new()),
            cv: Condvar::new(),
            timeout: Mutex::new(0),
            closed: AtomicBool::new(false),
            peer: Mutex::new(None),
        });
        *a.peer.lock().unwrap() = Some(Arc::clone(&b));
        *b.peer.lock().unwrap() = Some(Arc::clone(&a));
        (
            TestPipe { inner: a },
            TestPipe { inner: b },
        )
    }

    pub fn write(&self, bytes: &[u8]) {
        let peer = self.inner.peer.lock().unwrap().clone().expect("peer");
        peer.rx.lock().unwrap().extend(bytes.iter().copied());
        peer.cv.notify_all();
    }
}

impl Transport for TestPipe {
    fn send_all(&self, buf: &[u8]) -> Result<(), String> {
        self.write(buf);
        Ok(())
    }

    fn set_recv_timeout_ms(&self, ms: i64) {
        *self.inner.timeout.lock().unwrap() = ms;
    }

    fn receive(&self, out: &mut [u8]) -> Result<usize, upix_host::seams::RecvErr> {
        let timeout = *self.inner.timeout.lock().unwrap();
        let mut g = self.inner.rx.lock().unwrap();
        while g.is_empty() {
            if self.inner.closed.load(Ordering::SeqCst)
                || self
                    .inner
                    .peer
                    .lock()
                    .unwrap()
                    .as_ref()
                    .map(|p| p.closed.load(Ordering::SeqCst))
                    .unwrap_or(false)
            {
                return Err(upix_host::seams::RecvErr::Dead("closed".into()));
            }
            if timeout <= 0 {
                // Block until data or close.
                let (ng, _) = self
                    .inner
                    .cv
                    .wait_timeout(g, std::time::Duration::from_millis(200))
                    .unwrap();
                g = ng;
                continue;
            }
            let (ng, wait) = self
                .inner
                .cv
                .wait_timeout(g, std::time::Duration::from_millis(timeout as u64))
                .unwrap();
            g = ng;
            if g.is_empty() {
                if wait.timed_out() {
                    return Err(upix_host::seams::RecvErr::WouldBlock);
                }
            }
        }
        let n = out.len().min(g.len());
        for b in out.iter_mut().take(n) {
            *b = g.pop_front().unwrap();
        }
        Ok(n)
    }

    fn shutdown(&self) {
        self.inner.closed.store(true, Ordering::SeqCst);
        self.inner.cv.notify_all();
        if let Some(p) = self.inner.peer.lock().unwrap().as_ref() {
            p.cv.notify_all();
        }
    }
}

/// Client harness over a pipe: frames with PlainFramer + PlainSession so it
/// speaks exactly the wire format the coordinator produces.
pub struct TestClient {
    pub pipe: Arc<TestPipe>,
    pub rx: Mutex<VecDeque<u8>>,
}

impl TestClient {
    pub fn new(pipe: TestPipe) -> (Arc<Self>, Arc<TestPipe>) {
        let pipe = Arc::new(pipe);
        let c = Arc::new(Self {
            pipe: Arc::clone(&pipe),
            rx: Mutex::new(VecDeque::new()),
        });
        (c, pipe)
    }

    /// Drain one frame from the coordinator (with timeout).
    pub fn recv_frame(&self, timeout_ms: i64) -> Option<(MessageHeader, Vec<u8>)> {
        let deadline = std::time::Instant::now() + std::time::Duration::from_millis(timeout_ms as u64);
        let mut hdr = [0u8; HEADER_SIZE];
        let mut got = 0;
        while got < HEADER_SIZE {
            let remain = (deadline - std::time::Instant::now()).as_millis() as i64;
            if remain <= 0 {
                return None;
            }
            self.pipe.set_recv_timeout_ms(remain);
            match self.pipe.receive(&mut hdr[got..]) {
                Ok(0) => return None,
                Ok(n) => got += n,
                Err(upix_host::seams::RecvErr::Dead(_)) => return None,
                Err(upix_host::seams::RecvErr::WouldBlock) => continue,
            }
        }
        let Some(h) = PlainFramer.parse_header(&hdr) else {
            return None;
        };
        let mut payload = vec![0u8; h.payload_length as usize];
        let mut got = 0;
        while got < payload.len() {
            self.pipe.set_recv_timeout_ms(timeout_ms);
            match self.pipe.receive(&mut payload[got..]) {
                Ok(0) => return None,
                Ok(n) => got += n,
                Err(upix_host::seams::RecvErr::Dead(_)) => return None,
                Err(upix_host::seams::RecvErr::WouldBlock) => continue,
            }
        }
        Some((h, payload))
    }

    pub fn send(&self, ty: MessageType, payload: &[u8]) {
        let wire = PlainFramer.serialize_frame(ty, payload);
        self.pipe.write(&wire);
    }
}

/// Client-side half of the handshake against the coordinator's
/// (mock-crypto) server: returns the session codec used for sealed frames.
pub fn client_handshake(client: &TestClient, id_pub: [u8; 32]) -> Result<(), String> {
    // CLIENT KEY_HELLO: eph, id, sig(zeros at this stage — the mock verify
    // only checks the transcript signature on the SERVER hello).
    let (_c_pub, c_eph) = MockCrypto.x25519_keygen();  // (public, private)
    // Sign the transcript exactly like a real client: needs the server hello
    // first. Protocol order: client sends KEY_HELLO (sig zeroed? no — the
    // C++ client signs with its identity AFTER building the same transcript;
    // but the client sends KEY_HELLO BEFORE seeing the server hello, so its
    // sig covers a transcript where s_eph/s_id are zero. The AUTH_PROOF then
    // carries the full-transcript signature. Mirror that: zero-filled tail.
    let mut transcript = [0u8; 128];
    transcript[0..32].copy_from_slice(&c_eph);
    transcript[32..64].copy_from_slice(&id_pub);
    let sig = MockSigner { key: id_pub }.sign(&transcript);
    client.send(
        MessageType::KeyHello,
        &wireio::enc_key_hello(&upix_net::protocol::KeyHelloMessage {
            eph_pub: c_eph,
            id_pub,
            sig,
        }),
    );

    // SERVER KEY_HELLO (plaintext).
    let (hdr, payload) = client
        .recv_frame(2000)
        .ok_or("no server KEY_HELLO")?;
    assert_eq!(hdr.msg_type, MessageType::KeyHello);
    let s_hello = wireio::dec_key_hello(&payload).ok_or("bad KEY_HELLO")?;

    // AUTH_PROOF (sealed): sign the full transcript.
    let mut full = [0u8; 128];
    full[0..32].copy_from_slice(&c_eph);
    full[32..64].copy_from_slice(&id_pub);
    full[64..96].copy_from_slice(&s_hello.eph_pub);
    full[96..128].copy_from_slice(&s_hello.id_pub);
    let proof_sig = MockSigner { key: id_pub }.sign(&full);
    let wire = PlainSession.seal(MessageType::AuthProof, &wireio::enc_auth_proof(&proof_sig)).unwrap();
    let hdr = PlainFramer
        .parse_header(&wire[..HEADER_SIZE])
        .ok_or("seal")?;
    // write raw sealed frame (already framed by seal)
    client.pipe.write(&wire);
    let _ = hdr;

    // AUTH_RESPONSE (sealed).
    let (hdr, payload) = client.recv_frame(2000).ok_or("no AUTH_RESPONSE")?;
    assert_eq!(hdr.msg_type, MessageType::AuthResponse);
    let resp = wireio::dec_auth_response(&payload).ok_or("bad AUTH_RESPONSE")?;
    if resp.authenticated != 1 {
        return Err("client not authenticated".into());
    }
    Ok(())
}

// ── MockBackend ─────────────────────────────────────────────────────────────

pub struct MockModel {
    pub name: String,
    pub scale: i32,
    pub input_channels: i32,
}

pub struct MockBackend {
    pub dir: PathBuf,
    pub models: Vec<MockModel>,
    pub engine_scale: i32,
    /// How long each simulated "tile" takes (cancellation test).
    pub tile_delay_ms: u64,
    /// When set, process_image_u8 blocks until released (interrupt tests).
    pub gate: Option<Arc<(Mutex<bool>, Condvar)>>,
    /// Counters for assertions.
    pub process_calls: AtomicUsize,
    pub auto_tile_calls: AtomicUsize,
    pub auto_tile_result: AtomicI32,
}

impl MockBackend {
    pub fn new(dir: PathBuf, models: Vec<MockModel>, engine_scale: i32) -> Self {
        Self {
            dir,
            models,
            engine_scale,
            tile_delay_ms: 0,
            gate: None,
            process_calls: AtomicUsize::new(0),
            auto_tile_calls: AtomicUsize::new(0),
            auto_tile_result: AtomicI32::new(256),
        }
    }

    pub fn install_model_files(name: &str, dir: &Path) {
        let _ = std::fs::create_dir_all(dir);
        std::fs::write(dir.join(format!("{name}.param")), b"param").unwrap();
        std::fs::write(dir.join(format!("{name}.bin")), b"bin").unwrap();
    }
}

impl Backend for MockBackend {
    fn create_engine(&self) -> Result<Box<dyn Engine>, String> {
        let mut e = MockEngine::new(self.engine_scale, "mock");
        e.tile_delay_ms = self.tile_delay_ms;
        Ok(Box::new(e))
    }

    fn process_image_u8(
        &self,
        engine: &dyn Engine,
        tc: upix_core::engine::TileConfig,
        input: &[u8],
        width: i32,
        height: i32,
        channels: i32,
        mut progress: Option<&mut dyn FnMut(&ProcessingProgress)>,
        cancel: &AtomicBool,
        _enable_alpha_bicubic: bool,
    ) -> Result<Vec<u8>, EngineError> {
        self.process_calls.fetch_add(1, Ordering::SeqCst);
        let scale = engine.get_scale_factor();
        let out_len = width as usize * scale as usize * height as usize * scale as usize * channels as usize;
        let mut out = vec![0u8; out_len];

        // Simulate tiles of 8 rows; publish progress per tile; honor cancel
        // and the optional gate between tiles.
        let _ = &tc;
        let total_tiles = (height + 7) / 8;
        let mut done: i32 = 0;
        let mut y = 0i32;
        while y < height {
            if let Some(gate) = &self.gate {
                let (lock, cv) = (&gate.0, &gate.1);
                let mut open = lock.lock().unwrap();
                while !*open {
                    if cancel.load(Ordering::Relaxed) {
                        break;
                    }
                    let (g, _) = cv
                        .wait_timeout(open, std::time::Duration::from_millis(50))
                        .unwrap();
                    open = g;
                }
            }
            if cancel.load(Ordering::Relaxed) {
                return Err(EngineError::new(7, "canceled"));
            }
            let y_end = (y + 8).min(height);
            engine
                .process_u8(input, &mut out, width, height, channels, cancel)
                ?;
            y = y_end;
            done += 1;
            if let Some(cb) = progress.as_deref_mut() {
                cb(&ProcessingProgress {
                    completed_tiles: done,
                    total_tiles,
                    current_tile_x: 0,
                    current_tile_y: done as u32,
                });
            }
        }
        Ok(out)
    }

    fn auto_tile_size(
        &self,
        _engine: &dyn Engine,
        _model: &str,
        _min: i32,
        _max: i32,
        _overlap: i32,
        _w: i32,
        _h: i32,
    ) -> i32 {
        self.auto_tile_calls.fetch_add(1, Ordering::SeqCst);
        self.auto_tile_result.load(Ordering::SeqCst)
    }

    fn models_dir(&self) -> PathBuf {
        self.dir.clone()
    }

    fn scan_models(&self, dir: &Path) -> Result<(), String> {
        let _ = dir;
        Ok(())
    }

    fn available_models(&self) -> Vec<upix_host::seams::ModelInfo> {
        self.models
            .iter()
            .map(|m| upix_host::seams::ModelInfo {
                name: m.name.clone(),
                scale_factor: m.scale,
                total_size: 4 + 3,
                param_path: self.dir.join(format!("{}.param", m.name)),
                bin_path: self.dir.join(format!("{}.bin", m.name)),
                input_channels: m.input_channels,
            })
            .collect()
    }

    fn get_model(&self, name: &str) -> Option<upix_host::seams::ModelInfo> {
        self.available_models().into_iter().find(|m| m.name == name)
    }

    fn exif_orientation(&self, _bytes: &[u8]) -> i32 {
        1
    }

    fn decode_image(&self, format: &str, bytes: &[u8]) -> Result<upix_host::seams::Img, String> {
        if format == "mock" {
            // [w u32][h u32][c u32][raw]
            if bytes.len() < 12 {
                return Err("truncated".into());
            }
            let w = u32::from_le_bytes(bytes[0..4].try_into().unwrap()) as i32;
            let h = u32::from_le_bytes(bytes[4..8].try_into().unwrap()) as i32;
            let c = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as i32;
            let data = bytes[12..].to_vec();
            if data.len() != (w * h * c) as usize {
                return Err("size mismatch".into());
            }
            return Ok(upix_host::seams::Img {
                width: w,
                height: h,
                channels: c,
                data,
            });
        }
        // Real formats fall through to the png decoder (test PNGs).
        decode_png(bytes).map(|(w, h, c, data)| upix_host::seams::Img {
            width: w,
            height: h,
            channels: c,
            data,
        })
        .map_err(|e| e.to_string())
    }

    fn normalize_channels(&self, img: &mut upix_host::seams::Img, expected: i32) -> bool {
        if expected > 0 && img.channels != expected {
            return false;
        }
        true
    }

    fn apply_orientation(&self, _img: &mut upix_host::seams::Img, _o: i32) -> bool {
        false
    }

    fn encode_image(
        &self,
        img: &upix_host::seams::Img,
        format: &str,
        _quality: i32,
        _xmp: Option<&str>,
        _opts: &upix_img::EncoderOptions,
    ) -> Result<Vec<u8>, String> {
        if format == "png" {
            return encode_png(img);
        }
        if format == "jpg" {
            // Not a real JPEG; deterministic blob with the pixel count for
            // roundtrip assertions.
            let mut v = Vec::new();
            v.extend_from_slice(&[0xFF, 0xD8]); // JPEG SOI marker
            v.extend_from_slice(&(img.data.len() as u64).to_le_bytes());
            v.extend_from_slice(&img.data);
            return Ok(v);
        }
        Err(format!("unsupported format {format}"))
    }

    fn save_image(
        &self,
        img: &upix_host::seams::Img,
        path: &Path,
        quality: i32,
        xmp: Option<&str>,
        opts: &upix_img::EncoderOptions,
    ) -> Result<(), String> {
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().into_owned())
            .unwrap_or_default();
        let bytes = self.encode_image(img, &ext, quality, xmp, opts)?;
        std::fs::write(path, bytes).map_err(|e| e.to_string())
    }

    fn load_raw(&self, path: &Path) -> Result<Vec<u8>, String> {
        std::fs::read(path).map_err(|e| e.to_string())
    }

    fn load_from_memory(&self, ext: &str, bytes: &[u8]) -> Result<upix_host::seams::Img, String> {
        self.decode_image(ext, bytes)
    }

    fn avif_available(&self) -> (bool, bool) {
        (false, false)
    }

    fn is_supported_output_format(&self, f: &str) -> bool {
        matches!(f, "jpg" | "jpeg" | "png" | "webp" | "avif")
    }

    fn normalize_output_format(&self, f: &str) -> String {
        match f.to_lowercase().as_str() {
            "jpeg" => "jpg".to_string(),
            other => other.to_string(),
        }
    }

    fn sr_xmp(&self, _model: &str, _scale: i32, _ts: i64) -> String {
        "<xmp/>".to_string()
    }

    fn sanitize_model_for_filename(&self, name: &str) -> String {
        name.to_string()
    }

    fn resolve_output_path(&self, requested: &Path, _format: &str) -> PathBuf {
        requested.to_path_buf()
    }
}

// ── minimal PNG encode/decode for tests (png crate) ─────────────────────────

pub fn encode_png(img: &upix_host::seams::Img) -> Result<Vec<u8>, String> {
    let color = match img.channels {
        1 => png::ColorType::Grayscale,
        2 => png::ColorType::GrayscaleAlpha,
        3 => png::ColorType::Rgb,
        4 => png::ColorType::Rgba,
        _ => return Err("channels".into()),
    };
    let mut out = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut out, img.width as u32, img.height as u32);
        enc.set_color(color);
        enc.set_depth(png::BitDepth::Eight);
        let mut w = enc
            .write_header()
            .map_err(|e| e.to_string())?;
        w.write_image_data(&img.data).map_err(|e| e.to_string())?;
    }
    Ok(out)
}

pub fn decode_png(bytes: &[u8]) -> Result<(i32, i32, i32, Vec<u8>), png::DecodingError> {
    let dec = png::Decoder::new(std::io::Cursor::new(bytes));
    let mut reader = dec.read_info()?;
    let mut buf = vec![0u8; reader.output_buffer_size().unwrap_or(1 << 20)];
    let info = reader.next_frame(&mut buf)?;
    buf.truncate(info.buffer_size());
    Ok((
        info.width as i32,
        info.height as i32,
        match info.color_type {
            png::ColorType::Grayscale => 1,
            png::ColorType::GrayscaleAlpha => 2,
            png::ColorType::Rgb => 3,
            png::ColorType::Rgba => 4,
            png::ColorType::Indexed => 3,
        },
        buf,
    ))
}

/// Wire a tiny RGB PNG for submissions.
pub fn make_test_png(w: i32, h: i32) -> Vec<u8> {
    let mut data = Vec::with_capacity((w * h * 3) as usize);
    for y in 0..h {
        for x in 0..w {
            data.push((x * 7 % 255) as u8);
            data.push((y * 13 % 255) as u8);
            data.push(((x + y) * 3 % 255) as u8);
        }
    }
    encode_png(&upix_host::seams::Img {
        width: w,
        height: h,
        channels: 3,
        data,
    })
    .unwrap()
}

/// Build the payload for a legacy single-frame CLIENT_SUBMIT.
pub fn encoder_wire() -> upix_net::protocol::EncoderOptionsWire {
    upix_net::protocol::EncoderOptionsWire {
        png_level: -1,
        jpeg_subsampling: 420,
        webp_method: -1,
        webp_lossless: 0,
        reserved: [0; 3],
        avif_speed: 6,
        avif_subsampling: 420,
        avif_threads: 0,
    }
}

pub fn legacy_submit_payload(
    model: &str,
    input_format: &str,
    output_format: &str,
    data: &[u8],
) -> Vec<u8> {
    let task = upix_net::protocol::ClientTaskSubmit {
        model_name: fixed_str::<64>(model),
        input_format: fixed_str::<16>(input_format),
        output_format: fixed_str::<16>(output_format),
        tile_size: 0,
        tile_overlap: 16,
        output_quality: 90,
        input_data_length: data.len() as u32,
        encoder: encoder_wire(),
    };
    let mut v = wireio::enc_task_submit(&task);
    v.extend_from_slice(data);
    v
}

/// Struct-only payload for a chunked CLIENT_SUBMIT.
pub fn chunked_submit_header(
    model: &str,
    input_format: &str,
    output_format: &str,
    total_len: u32,
) -> Vec<u8> {
    let task = upix_net::protocol::ClientTaskSubmit {
        model_name: fixed_str::<64>(model),
        input_format: fixed_str::<16>(input_format),
        output_format: fixed_str::<16>(output_format),
        tile_size: 0,
        tile_overlap: 16,
        output_quality: 90,
        input_data_length: total_len,
        encoder: encoder_wire(),
    };
    wireio::enc_task_submit(&task)
}

pub fn fixed_str<const N: usize>(s: &str) -> [u8; N] {
    let mut v = [0u8; N];
    let b = s.as_bytes();
    let n = b.len().min(N.saturating_sub(1));
    v[..n].copy_from_slice(&b[..n]);
    v
}


/// Guard against accidental oversize sends in tests.
pub fn assert_fits_payload(n: usize) {
    assert!(n <= MAX_NETWORK_PAYLOAD as usize);
}
