//! The seam between the Host page and the coordinator (`upix-host` + `upix-net`).
//!
//! `HostService::start` runs on a background thread and reports events through an
//! `mpsc::Sender`; approvals flow back through the returned reply channel.

use std::path::PathBuf;
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use base64::Engine as _;
use sha2::{Digest, Sha256};

/// Coordinator settings collected from the Host page.
#[derive(Clone, Debug)]
pub struct HostConfig {
    pub port: u16,
    pub auto_approve: bool,
    pub models_dir: PathBuf,
}

/// One pairing request awaiting user approval.
#[derive(Clone, Debug, PartialEq)]
pub struct ApprovalRequest {
    pub device: String,
    pub fingerprint: String,
}

/// The UI's answer to a pending approval.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApprovalReply {
    Allow,
    Deny,
}

/// Events the host thread streams to the UI.
#[derive(Clone, Debug)]
pub enum HostEvent {
    Started {
        port: u16,
        ips: Vec<String>,
        fingerprint: String,
        pubkey_b64: String,
        /// The v2 pairing payload (exactly what the QR encodes).
        qr_payload: String,
        /// PNG QR of `qr_payload` as a `data:` URI.
        qr_data_uri: String,
    },
    Log(String),
    Approval(ApprovalRequest),
    Stopped,
}

/// Where the UI sends its approval decision, and the stop flag.
#[derive(Clone)]
pub struct HostHandle {
    pub reply: Sender<ApprovalReply>,
    pub stop: Arc<std::sync::atomic::AtomicBool>,
}

/// Handles returned by `HostService::start`.
pub struct HostStarted {
    pub handle: HostHandle,
    pub events: std::sync::mpsc::Receiver<HostEvent>,
}

pub trait HostService: Send + Sync + 'static {
    fn start(&self, cfg: HostConfig) -> HostStarted;
}

/// Builds a v2 pairing payload (`{"v":2,"ip":...}`) and its PNG QR `data:` URI.
pub fn build_qr_for_ip(
    ip: &str,
    port: u16,
    fingerprint: &str,
    pubkey_b64: &str,
) -> (String, String) {
    let payload = format!(
        r#"{{"v":2,"ip":"{ip}","port":{port},"fp":"{fingerprint}","pk":"{pubkey_b64}"}}"#
    );
    let uri = crate::qr::qr_png_data_uri(&payload).unwrap_or_default();
    (payload, uri)
}

/// Reads `~/.upix/trusted_clients` (one standard base64 pubkey per line).
pub fn trust_list() -> Vec<String> {
    let Some(dir) = home_dir() else {
        return Vec::new();
    };
    let path = dir.join(".upix").join("trusted_clients");
    match std::fs::read_to_string(path) {
        Ok(text) => text
            .lines()
            .map(str::trim)
            .filter(|l| !l.is_empty())
            .map(str::to_owned)
            .collect(),
        Err(_) => Vec::new(),
    }
}

/// Clears `~/.upix/trusted_clients`.
pub fn clear_trust_list() -> Result<(), String> {
    let Some(dir) = home_dir() else {
        return Ok(());
    };
    let path = dir.join(".upix").join("trusted_clients");
    if path.exists() {
        std::fs::write(&path, "").map_err(|e| format!("清空信任列表失败: {e}"))?;
    }
    Ok(())
}

/// Removes one entry at `idx` from `~/.upix/trusted_clients`.
pub fn remove_trusted_client(idx: usize) -> Result<(), String> {
    let Some(dir) = home_dir() else {
        return Ok(());
    };
    let path = dir.join(".upix").join("trusted_clients");
    let mut list = trust_list();
    if idx < list.len() {
        list.remove(idx);
        let body = if list.is_empty() {
            String::new()
        } else {
            format!("{}\n", list.join("\n"))
        };
        std::fs::write(&path, body).map_err(|e| format!("更新信任列表失败: {e}"))?;
    }
    Ok(())
}

pub fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
}

/// Device fingerprint: sha256(pubkey) hex, first 16 chars uppercased.
pub fn fingerprint_of(pubkey: &[u8; 32]) -> String {
    let full = Sha256::digest(pubkey);
    let hex: String = full.iter().map(|b| format!("{b:02x}")).collect();
    hex[..16].to_uppercase()
}

/// Computes short display fingerprint for a base64-encoded public key line in `trusted_clients`.
pub fn fingerprint_of_b64(b64_line: &str) -> String {
    if let Ok(bytes) = base64::engine::general_purpose::STANDARD.decode(b64_line.trim()) {
        if bytes.len() == 32 {
            let mut arr = [0u8; 32];
            arr.copy_from_slice(&bytes);
            return fingerprint_of(&arr);
        }
    }
    b64_line.chars().take(16).collect()
}

// ── Mock host ────────────────────────────────────────────────────────────

pub struct MockHostService;

impl HostService for MockHostService {
    fn start(&self, cfg: HostConfig) -> HostStarted {
        let (events, rx) = std::sync::mpsc::channel();
        let (reply, reply_rx) = std::sync::mpsc::channel();
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let stop_for_thread = stop.clone();

        thread::spawn(move || {
            run_mock_host(cfg, &events, &reply_rx, &stop_for_thread);
        });

        HostStarted {
            handle: HostHandle { reply, stop },
            events: rx,
        }
    }
}

fn run_mock_host(
    cfg: HostConfig,
    events: &Sender<HostEvent>,
    reply_rx: &std::sync::mpsc::Receiver<ApprovalReply>,
    stop: &Arc<std::sync::atomic::AtomicBool>,
) {
    let mut pubkey = [0u8; 32];
    let seed = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos().to_le_bytes())
        .unwrap_or([0u8; 16]);
    let mut hasher = Sha256::new();
    hasher.update(seed);
    hasher.update(std::process::id().to_le_bytes());
    pubkey.copy_from_slice(&hasher.finalize());
    let fingerprint = fingerprint_of(&pubkey);
    let pk_b64 = base64::engine::general_purpose::STANDARD.encode(pubkey);

    let ip = local_ip_hint();
    let (payload, qr) = build_qr_for_ip(&ip, cfg.port, &fingerprint, &pk_b64);
    let _ = events.send(HostEvent::Started {
        port: cfg.port,
        ips: vec![ip.clone()],
        fingerprint: fingerprint.clone(),
        pubkey_b64: pk_b64,
        qr_payload: payload,
        qr_data_uri: qr,
    });
    let _ = events.send(HostEvent::Log(format!(
        "Coordinator 就绪（模拟）: {ip}:{}, 模型目录 {}",
        cfg.port,
        cfg.models_dir.display()
    )));

    for tick in 0.. {
        if stop.load(std::sync::atomic::Ordering::Relaxed) {
            let _ = events.send(HostEvent::Stopped);
            return;
        }
        thread::sleep(Duration::from_millis(500));
        if tick == 8 && !cfg.auto_approve {
            let _ = events.send(HostEvent::Approval(ApprovalRequest {
                device: "演示设备 (mock)".to_owned(),
                fingerprint: fingerprint.clone(),
            }));
        } else if tick == 8 && cfg.auto_approve {
            let _ = events.send(HostEvent::Log("自动批准已开启: 跳过审批".into()));
        }
        if tick % 20 == 19 {
            let _ = events.send(HostEvent::Log("心跳: 等待客户端连接…".into()));
        }
        while let Ok(_stale) = reply_rx.try_recv() {
            let _ = events.send(HostEvent::Log("审批请求已处理".into()));
        }
    }
}

fn local_ip_hint() -> String {
    #[cfg(target_os = "linux")]
    {
        if let Ok(out) = std::process::Command::new("hostname").arg("-I").output()
            && out.status.success()
        {
            let first = String::from_utf8_lossy(&out.stdout)
                .split_whitespace()
                .next()
                .unwrap_or("")
                .to_owned();
            if !first.is_empty() {
                return first;
            }
        }
    }
    "127.0.0.1".to_owned()
}

// ── Real host (integration feature) ─────────────────────────────────────

#[cfg(feature = "integration")]
pub struct RealHostService;

#[cfg(feature = "integration")]
pub mod real_host {
    use std::path::{Path, PathBuf};
    use std::sync::atomic::AtomicBool;
    use std::sync::Mutex;

    use upix_host::seams::{Backend, CryptoProvider, SessionCodec, Signer, TrustApi};
    use upix_net::frame::{parse_header, serialize_frame, verify_frame};
    use upix_net::protocol::{MessageHeader, MessageType, HEADER_SIZE};
    use upix_net::{crypto, EncryptedSession, TrustStore};

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

    pub struct NetCodec {
        session: EncryptedSession,
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

    pub struct NetTrust {
        pub store: Mutex<TrustStore>,
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

    pub struct NetFramer;

    impl upix_host::Framer for NetFramer {
        fn serialize_frame(&self, ty: MessageType, payload: &[u8]) -> Vec<u8> {
            serialize_frame(
                MessageHeader {
                    magic: upix_net::protocol::MAGIC,
                    msg_type: ty,
                    payload_length: payload.len() as u32,
                    checksum: 0,
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

    pub struct FfiBackendShim {
        pub models_dir: PathBuf,
    }

    impl Backend for FfiBackendShim {
        fn create_engine(&self) -> Result<Box<dyn upix_core::engine::Engine>, String> {
            let prepared = upix_engine_ffi::NcnnEngine::new().map_err(|e| e.message)?;
            let slot = Mutex::new(Some(prepared));
            upix_core::initialize_engine(
                move || {
                    Box::new(slot.lock().unwrap().take().expect("engine factory runs once"))
                        as Box<dyn upix_core::engine::Engine>
                },
                upix_core::EngineStrategy::GpuThenCpu,
            )
        }

        fn process_image_u8(
            &self,
            engine: &dyn upix_core::engine::Engine,
            tc: upix_core::engine::TileConfig,
            input: &[u8],
            width: i32,
            height: i32,
            channels: i32,
            progress: Option<&mut dyn FnMut(&upix_core::engine::ProcessingProgress)>,
            cancel: &AtomicBool,
            enable_alpha_bicubic: bool,
        ) -> Result<Vec<u8>, upix_core::engine::EngineError> {
            let _ = enable_alpha_bicubic;
            let cfg = upix_core::ProcessingConfig {
                tile_size: tc.tile_size,
                tile_overlap: tc.overlap,
                batch_size: tc.batch_size,
                ..upix_core::ProcessingConfig::default()
            };
            struct Cb<'a>(*mut (dyn FnMut(&upix_core::engine::ProcessingProgress) + 'a));
            unsafe impl Send for Cb<'_> {}
            unsafe impl Sync for Cb<'_> {}
            fn erase<'a>(
                f: &'a mut dyn FnMut(&upix_core::engine::ProcessingProgress),
            ) -> *mut (dyn FnMut(&upix_core::engine::ProcessingProgress) + 'a) {
                f
            }
            let cb = progress.map(|f| Cb(erase(f)));
            let gate = Mutex::new(());
            let wrapped = |p: &upix_core::engine::ProcessingProgress| {
                if let Some(cb) = &cb {
                    let _hold = gate.lock().unwrap();
                    unsafe { (*cb.0)(p) };
                }
            };
            upix_core::process_image_u8(
                engine,
                &cfg,
                input,
                width,
                height,
                channels,
                Some(&wrapped),
                cancel,
            )
            .map(|scaled| scaled.data)
        }

        fn auto_tile_size(
            &self,
            engine: &dyn upix_core::engine::Engine,
            model_name: &str,
            min_tile: i32,
            max_tile: i32,
            tile_overlap: i32,
            width: i32,
            height: i32,
        ) -> i32 {
            upix_core::TileSizeOptimizer::find_optimal_tile_size(
                engine,
                &upix_core::tile_optimizer::ModelConstraints {
                    model_name: model_name.to_string(),
                    min_tile_size: min_tile,
                    max_tile_size: max_tile,
                    ..Default::default()
                },
                &upix_core::ProcessingConfig {
                    tile_overlap,
                    ..Default::default()
                },
                width,
                height,
            )
        }

        fn models_dir(&self) -> PathBuf {
            self.models_dir.clone()
        }
        fn scan_models(&self, dir: &Path) -> Result<(), String> {
            let mut manager = upix_core::ModelManager::new(dir.to_path_buf());
            manager.scan_models().map(|_| ()).map_err(|e| e.message)
        }
        fn available_models(&self) -> Vec<upix_host::seams::ModelInfo> {
            let mut manager = upix_core::ModelManager::new(self.models_dir.clone());
            let _ = manager.scan_models();
            manager
                .available_models()
                .iter()
                .map(|m| upix_host::seams::ModelInfo {
                    name: m.name.clone(),
                    scale_factor: m.scale_factor,
                    total_size: m.total_size,
                    param_path: m.param_path.clone(),
                    bin_path: m.bin_path.clone(),
                    input_channels: m.input_channels,
                })
                .collect()
        }
        fn get_model(&self, name: &str) -> Option<upix_host::seams::ModelInfo> {
            self.available_models().into_iter().find(|m| m.name == name)
        }
        fn exif_orientation(&self, bytes: &[u8]) -> i32 {
            upix_img::exif::parse_jpeg_exif_orientation(bytes) as i32
        }
        fn decode_image(
            &self,
            _format: &str,
            bytes: &[u8],
        ) -> Result<upix_host::seams::Img, String> {
            upix_img::decode(bytes)
                .map(|img| upix_host::seams::Img {
                    width: img.width,
                    height: img.height,
                    channels: img.channels,
                    data: img.data,
                })
                .map_err(|e| e.message)
        }
        fn normalize_channels(&self, img: &mut upix_host::seams::Img, expected: i32) -> bool {
            let mut owned = upix_img::Image {
                width: img.width,
                height: img.height,
                channels: img.channels,
                data: std::mem::take(&mut img.data),
            };
            let ok = upix_img::normalize_image_channels(&mut owned, expected, "remote task");
            img.width = owned.width;
            img.height = owned.height;
            img.channels = owned.channels;
            img.data = owned.data;
            ok
        }
        fn apply_orientation(&self, img: &mut upix_host::seams::Img, orientation: i32) -> bool {
            if !(1..=8).contains(&orientation) {
                return false;
            }
            let mut owned = upix_img::Image {
                width: img.width,
                height: img.height,
                channels: img.channels,
                data: std::mem::take(&mut img.data),
            };
            let applied = upix_img::apply_orientation(&mut owned, orientation as u8);
            img.width = owned.width;
            img.height = owned.height;
            img.channels = owned.channels;
            img.data = owned.data;
            applied
        }
        fn encode_image(
            &self,
            img: &upix_host::seams::Img,
            format: &str,
            quality: i32,
            xmp: Option<&str>,
            opts: &upix_img::EncoderOptions,
        ) -> Result<Vec<u8>, String> {
            upix_img::encode(
                &upix_img::Image {
                    width: img.width,
                    height: img.height,
                    channels: img.channels,
                    data: img.data.clone(),
                },
                format,
                quality,
                opts,
                xmp,
            )
            .map_err(|e| e.message)
        }
        fn save_image(
            &self,
            img: &upix_host::seams::Img,
            path: &Path,
            quality: i32,
            xmp: Option<&str>,
            opts: &upix_img::EncoderOptions,
        ) -> Result<(), String> {
            let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("png");
            let bytes = self.encode_image(img, ext, quality, xmp, opts)?;
            std::fs::write(path, bytes).map_err(|e| e.to_string())
        }
        fn load_raw(&self, path: &Path) -> Result<Vec<u8>, String> {
            std::fs::read(path).map_err(|e| e.to_string())
        }
        fn load_from_memory(
            &self,
            _ext: &str,
            bytes: &[u8],
        ) -> Result<upix_host::seams::Img, String> {
            self.decode_image("", bytes)
        }
        fn avif_available(&self) -> (bool, bool) {
            (
                upix_img::avif_decode_available(),
                upix_img::avif_encode_available(),
            )
        }
        fn is_supported_output_format(&self, f: &str) -> bool {
            upix_core::config::is_supported_output_format(f)
        }
        fn normalize_output_format(&self, f: &str) -> String {
            upix_core::config::normalize_output_format(f)
        }
        fn sr_xmp(&self, model: &str, scale: i32, ts: i64) -> String {
            upix_img::sr_marker::build_xmp(model, scale, ts)
        }
        fn sanitize_model_for_filename(&self, name: &str) -> String {
            upix_core::sanitize_model_for_filename(name)
        }
        fn resolve_output_path(&self, requested: &Path, output_format: &str) -> PathBuf {
            upix_core::resolve_output_path(
                requested,
                &upix_core::ProcessingConfig {
                    output_format: output_format.to_string(),
                    ..Default::default()
                },
            )
        }
    }
}

#[cfg(feature = "integration")]
impl HostService for RealHostService {
    fn start(&self, cfg: HostConfig) -> HostStarted {
        use real_host::*;
        use std::sync::atomic::Ordering;
        use std::sync::mpsc::RecvTimeoutError;
        use std::sync::{Arc, Mutex};
        use std::time::Duration;

        let (events_tx, rx) = std::sync::mpsc::channel();
        let (reply, reply_rx) = std::sync::mpsc::channel();
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let handle = HostHandle {
            reply,
            stop: stop.clone(),
        };

        let identity = match upix_net::Identity::load_or_create(
            &upix_net::identity::default_identity_path(),
        ) {
            Ok(v) => v,
            Err(e) => {
                let _ = events_tx.send(HostEvent::Log(format!("身份加载失败: {e:?}")));
                let _ = events_tx.send(HostEvent::Stopped);
                return HostStarted { handle, events: rx };
            }
        };
        let fingerprint = identity.fingerprint();
        let pubkey_b64 = base64::engine::general_purpose::STANDARD.encode(identity.public_key());
        let mut ips: Vec<String> = upix_net::local_ips()
            .into_iter()
            .filter(|addr| !addr.is_loopback())
            .map(|addr| addr.to_string())
            .collect();
        if ips.is_empty() {
            ips.push("127.0.0.1".to_owned());
        }
        let ip = ips[0].clone();
        let payload = upix_host::pairing_qr_payload(
            &ip,
            cfg.port,
            &fingerprint,
            &identity.public_key(),
        );
        let qr = match crate::qr::qr_png_data_uri(&payload) {
            Ok(uri) => uri,
            Err(e) => {
                let _ = events_tx.send(HostEvent::Log(format!("QR 生成失败: {e}")));
                let _ = events_tx.send(HostEvent::Stopped);
                return HostStarted { handle, events: rx };
            }
        };
        let _ = events_tx.send(HostEvent::Started {
            port: cfg.port,
            ips: ips.clone(),
            fingerprint: fingerprint.clone(),
            pubkey_b64,
            qr_payload: payload,
            qr_data_uri: qr,
        });

        let trust_path = upix_net::identity::default_trust_path();
        let config = upix_host::CoordinatorConfig {
            port: cfg.port,
            bind_addr: "0.0.0.0".to_owned(),
            models_dir: cfg.models_dir.clone(),
            auto_approve: cfg.auto_approve,
            backend: Arc::new(FfiBackendShim {
                models_dir: cfg.models_dir.clone(),
            }),
            framer: Arc::new(NetFramer),
            crypto: Arc::new(NetCrypto),
            signer: Arc::new(NetSigner { identity }),
            trust: Some(Arc::new(NetTrust {
                store: Mutex::new(upix_net::TrustStore::load(&trust_path)),
            })),
            listener_factory: Box::new(upix_host::transport_std::TcpFactory),
        };
        let mut coordinator = upix_host::Coordinator::new(config);
        if let Err(e) = coordinator.start() {
            let _ = events_tx.send(HostEvent::Log(format!(
                "Coordinator 启动失败: {}",
                e.message
            )));
            let _ = events_tx.send(HostEvent::Stopped);
            return HostStarted { handle, events: rx };
        }
        let _ = events_tx.send(HostEvent::Log(format!(
            "Coordinator 就绪: {ip}:{}",
            cfg.port
        )));

        let coord = Arc::new(Mutex::new(coordinator));
        let events_loop = events_tx;
        std::thread::spawn(move || {
            let mut inflight: Option<u64> = None;
            loop {
                if stop.load(Ordering::Relaxed) {
                    coord.lock().unwrap().stop();
                    let _ = events_loop.send(HostEvent::Stopped);
                    return;
                }
                if inflight.is_none() {
                    if let Some(req) = coord.lock().unwrap().pop_pending_approval() {
                        let _ = events_loop.send(HostEvent::Approval(ApprovalRequest {
                            device: req.device_name,
                            fingerprint: req.fingerprint,
                        }));
                        inflight = Some(req.id);
                    }
                }
                if let Some(id) = inflight {
                    match reply_rx.recv_timeout(Duration::from_millis(200)) {
                        Ok(reply) => {
                            coord.lock().unwrap().resolve_approval(
                                id,
                                reply == ApprovalReply::Allow,
                            );
                            inflight = None;
                        }
                        Err(RecvTimeoutError::Timeout) => {}
                        Err(RecvTimeoutError::Disconnected) => {
                            coord.lock().unwrap().resolve_approval(id, false);
                            coord.lock().unwrap().stop();
                            let _ = events_loop.send(HostEvent::Stopped);
                            return;
                        }
                    }
                } else {
                    std::thread::sleep(Duration::from_millis(200));
                }
            }
        });

        HostStarted { handle, events: rx }
    }
}
