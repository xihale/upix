//! `upix host {start, info}` — the coordinator phones pair with.
//!
//! start: launch a Coordinator on `port`, print the connection card (IP/QR/key),
//!        and poll until Ctrl+C. Each submitted image is upscaled on the PC's own
//!        engine (GPU, CPU fallback). Auto-generates an identity key if none.
//! info:  just print the connection card for the given port/key (no server).

use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;

use upix_host::coordinator::{Coordinator, CoordinatorConfig, PendingApproval};
use upix_host::net_adapters::{NetCrypto, NetFramer, NetSigner, NetTrust};
use upix_host::seams::{Backend, Listener, ListenerFactory};
use upix_net::identity::{default_identity_path, default_trust_path};
use upix_net::TrustStore;

use crate::args::HostArgs;
use crate::common::{
    build_connection_card, emit_json_line, json_escape, print_connection_card,
    read_line_interruptible, G_RUNNING,
};
use crate::logging;

/// The coordinator's Backend bundle: engine factory over the ncnn FFI
/// plus the shared core/img pipeline calls, model scanning over
/// upix_core::ModelManager.
pub struct CliBackend {
    pub models_dir: PathBuf,
}

impl Backend for CliBackend {
    fn create_engine(&self) -> Result<Box<dyn upix_core::engine::Engine>, String> {
        upix_core::initialize_engine(
            || match upix_engine_ffi::NcnnEngine::new() {
                Ok(e) => Box::new(e) as Box<dyn upix_core::engine::Engine>,
                Err(e) => panic!("engine create failed: {}", e.message),
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
        cancel: &std::sync::atomic::AtomicBool,
        enable_alpha_bicubic: bool,
    ) -> Result<Vec<u8>, upix_core::engine::EngineError> {
        let _ = enable_alpha_bicubic; // the core decides internally
        let cfg = upix_core::ProcessingConfig {
            tile_size: tc.tile_size,
            tile_overlap: tc.overlap,
            batch_size: tc.batch_size,
            ..upix_core::ProcessingConfig::default()
        };
        // Adapt the FnMut callback to the Fn(+Send+Sync)-bound pipeline API.
        // SAFETY: the coordinator serializes task execution per session and
        // the pipeline invokes the progress callback from its proc stage
        // thread only, so the &mut dyn FnMut behind the pointer is never
        // called concurrently (the same single-caller contract the C++
        // coordinator relied on when handing `std::function&` across its
        // worker).
        struct ProgressShim<'a>(Option<&'a mut (dyn FnMut(&upix_core::engine::ProcessingProgress) + 'a)>);
        unsafe impl Send for ProgressShim<'_> {}
        unsafe impl Sync for ProgressShim<'_> {}
        let cell = std::sync::Mutex::new(ProgressShim(progress));
        let wrapped = move |p: &upix_core::engine::ProcessingProgress| {
            if let Ok(mut slot) = cell.lock() {
                if let Some(f) = slot.0.as_mut() {
                    f(p);
                }
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
        .map(|s| s.data)
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
        manager
            .scan_models()
            .map(|_| ())
            .map_err(|e| e.message)
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
        let mut manager = upix_core::ModelManager::new(self.models_dir.clone());
        let _ = manager.scan_models();
        manager.get_model(name).map(|m| upix_host::seams::ModelInfo {
            name: m.name.clone(),
            scale_factor: m.scale_factor,
            total_size: m.total_size,
            param_path: m.param_path.clone(),
            bin_path: m.bin_path.clone(),
            input_channels: m.input_channels,
        })
    }
    fn exif_orientation(&self, bytes: &[u8]) -> i32 {
        upix_img::exif::parse_jpeg_exif_orientation(bytes) as i32
    }
    fn decode_image(&self, _format: &str, bytes: &[u8]) -> Result<upix_host::seams::Img, String> {
        upix_img::decode(bytes)
            .map(|i| upix_host::seams::Img {
                width: i.width,
                height: i.height,
                channels: i.channels,
                data: i.data,
            })
            .map_err(|e| e.message)
    }
    fn normalize_channels(&self, img: &mut upix_host::seams::Img, expected_channels: i32) -> bool {
        let mut conv = upix_img::Image {
            width: img.width,
            height: img.height,
            channels: img.channels,
            data: std::mem::take(&mut img.data),
        };
        let ok = upix_img::normalize_image_channels(&mut conv, expected_channels, "remote task");
        img.width = conv.width;
        img.height = conv.height;
        img.channels = conv.channels;
        img.data = conv.data;
        ok
    }
    fn apply_orientation(&self, img: &mut upix_host::seams::Img, orientation: i32) -> bool {
        let mut conv = upix_img::Image {
            width: img.width,
            height: img.height,
            channels: img.channels,
            data: std::mem::take(&mut img.data),
        };
        let changed = upix_img::apply_orientation(&mut conv, orientation as u8);
        img.width = conv.width;
        img.height = conv.height;
        img.channels = conv.channels;
        img.data = conv.data;
        changed
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
        let bytes = self.encode_image(img, &path.extension().map(|e| e.to_string_lossy().into_owned()).unwrap_or_default(), quality, xmp, opts)?;
        std::fs::write(path, bytes).map_err(|e| format!("write {}: {e}", path.display()))
    }
    fn load_raw(&self, path: &Path) -> Result<Vec<u8>, String> {
        std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))
    }
    fn load_from_memory(&self, _ext: &str, bytes: &[u8]) -> Result<upix_host::seams::Img, String> {
        upix_img::decode(bytes)
            .map(|i| upix_host::seams::Img {
                width: i.width,
                height: i.height,
                channels: i.channels,
                data: i.data,
            })
            .map_err(|e| e.message)
    }
    fn avif_available(&self) -> (bool, bool) {
        (upix_img::avif_decode_available(), upix_img::avif_encode_available())
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

/// TCP listener factory over std (same locking discipline as the C++
/// StreamSocket; see transport_std.rs).
pub struct StdListenerFactory;

impl ListenerFactory for StdListenerFactory {
    fn listen(&self, port: u16) -> Result<Box<dyn Listener>, String> {
        use std::net::TcpListener as StdTcpListener;
        let listener = StdTcpListener::bind(("0.0.0.0", port))
            .map_err(|e| format!("bind port {port}: {e}"))?;
        Ok(Box::new(StdListener { listener: Arc::new(listener) }))
    }
}

struct StdListener {
    listener: Arc<std::net::TcpListener>,
}

impl Listener for StdListener {
    fn accept(&self) -> Result<Option<Box<dyn upix_host::seams::Transport>>, String> {
        use upix_host::transport_std::StdTcpSocket;
        // Poll-tick accept: the accept loop re-checks the stop flag at least
        // every ~500 ms.
        self.listener.set_nonblocking(true).map_err(|e| e.to_string())?;
        match self.listener.accept() {
            Ok((stream, peer)) => {
                let sock = StdTcpSocket::new(stream, peer)?;
                Ok(Some(Box::new(sock)))
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => Ok(None),
            Err(e) => Err(e.to_string()),
        }
    }
    fn shutdown(&self) {
        // Closing the listener wakes accept(); dropping the Arc happens on
        // stop(). Use a libc shutdown on the raw fd for prompt wakeup.
        use std::os::fd::AsRawFd;
        // SAFETY: shutdown on a valid listening fd only wakes pending accepts.
        unsafe {
            libc::shutdown(self.listener.as_raw_fd(), libc::SHUT_RDWR);
        }
    }
}

/// Card document: one-shot for `info --json`, "t":"card" event for start.
fn emit_card(card: &crate::common::ConnectionCard, event: bool) {
    let mut ips = String::new();
    for (i, addr) in card.addresses.iter().enumerate() {
        if i > 0 {
            ips.push(',');
        }
        ips.push_str(&format!(r#""{}""#, json_escape(addr)));
    }
    let head = if event { r#"{"t":"card","# } else { "{" };
    emit_json_line(format!(
        r#"{head}"ips":[{ips}],"port":{},"fingerprint":"{}","qr":"{}"}}"#,
        card.port,
        json_escape(&card.fingerprint),
        json_escape(&card.qr_payload)
    ));
}

fn load_identity() -> upix_net::Identity {
    upix_net::Identity::load_or_create(&default_identity_path())
        .expect("identity load/create")
}

pub fn run(a: &HostArgs) -> i32 {
    // Machine event mode before the first LOG_* so stdout stays parseable.
    if a.json {
        logging::enable_machine_output();
    }

    if a.subcommand == "info" {
        let identity = load_identity();
        if a.json {
            let card = build_connection_card(a.port, &identity.public_key(), &identity.fingerprint());
            emit_card(&card, false);
        } else {
            print_connection_card(a.port, &identity.public_key(), &identity.fingerprint());
        }
        return 0;
    }

    // ── start ───────────────────────────────────────────────────────────────
    let identity = load_identity();
    log::info!(
        target: "Host",
        "PC identity fingerprint: {}",
        identity.fingerprint()
    );

    crate::install_signal_handlers();
    G_RUNNING.store(true, Ordering::SeqCst);

    let trust = Arc::new(NetTrust::new(TrustStore::load(&default_trust_path())));
    let cfg = CoordinatorConfig {
        port: a.port,
        bind_addr: "0.0.0.0".to_string(),
        models_dir: PathBuf::from(&a.models_dir),
        auto_approve: a.auto_approve,
        backend: Arc::new(CliBackend {
            models_dir: PathBuf::from(&a.models_dir),
        }),
        framer: Arc::new(NetFramer),
        crypto: Arc::new(NetCrypto),
        signer: Arc::new(NetSigner {
            identity: load_identity(),
        }),
        trust: Some(trust),
        listener_factory: Box::new(StdListenerFactory),
    };
    let mut coordinator = Coordinator::new(cfg);

    // Task progress: 0-100% events on the machine stream, human log line
    // otherwise (same shape as cmd_host.cpp's on_task_progress).
    let machine = a.json;
    coordinator.set_on_task_progress(Some(Box::new(move |progress: f32| {
        thread_local! {
            static LAST_PCT: std::cell::Cell<i32> = const { std::cell::Cell::new(-1) };
        }
        let pct = (progress.clamp(0.0, 1.0) * 100.0) as i32;
        let previous = LAST_PCT.with(|c| c.replace(pct));
        if previous == pct {
            return;
        }
        if machine {
            emit_json_line(format!(r#"{{"t":"task","pct":{pct}}}"#));
        } else {
            log::info!(target: "Host", "Task progress: {pct}%");
        }
    })));

    if let Err(e) = coordinator.start() {
        log::error!(target: "Host", "Failed to start coordinator: {}", e.message);
        return 1;
    }

    log::info!(target: "Host", "Coordinator running. Press Ctrl+C to stop.");

    if machine {
        let card = build_connection_card(a.port, &identity.public_key(), &identity.fingerprint());
        emit_card(&card, true);
    } else {
        print_connection_card(a.port, &identity.public_key(), &identity.fingerprint());
    }

    while G_RUNNING.load(Ordering::SeqCst) && coordinator.is_running() {
        // 服务待批准的配对请求(仅首次连接的手机;已信任的自动通过)。
        // --auto-approve 跳过 stdin [y/n] 提示,直接批准 — 供自动化测试/CI。
        // 否则分片读 stdin:Ctrl+C/SIGTERM 能在一个 tick 内结束等待。
        // --json 下 GUI 消费 pairing 事件,并向 stdin 写入同一条 "y\n"/"n\n" wire。
        if let Some(req) = coordinator.pop_pending_approval() {
            let mut approved = false;
            if machine {
                emit_json_line(format!(
                    r#"{{"t":"pairing","device":"{}","fingerprint":"{}"}}"#,
                    json_escape(&req.device_name),
                    json_escape(&req.fingerprint)
                ));
            }
            let still_waiting = || G_RUNNING.load(Ordering::SeqCst) && coordinator.is_running();
            let is_yes = |line: &str| matches!(line, "y" | "Y" | "yes" | "YES");
            if a.auto_approve {
                approved = true;
                log::info!(
                    target: "Host",
                    "自动批准配对(--auto-approve): {} ({})",
                    req.device_name, req.fingerprint
                );
            } else if machine {
                let mut line = String::new();
                if read_line_interruptible(&mut line, &still_waiting) {
                    approved = is_yes(&line);
                }
                // stdin EOF(例如后台运行)或停机 — 默认拒绝,避免无限等待。
            } else {
                print!(
                    "\n┌─ 配对请求 ───────────────────────────────────────\n\
                     │ 设备: {}\n\
                     │ 指纹: {}\n\
                     \n└──────────────────────────────────────────────────┘\n\
                     允许此设备连接? [y/n]: ",
                    req.device_name, req.fingerprint
                );
                use std::io::Write;
                let _ = std::io::stdout().flush();
                let mut line = String::new();
                if read_line_interruptible(&mut line, &still_waiting) {
                    approved = is_yes(&line);
                } else {
                    // stdin EOF(例如后台运行)或收到停机信号 — 默认拒绝。
                    println!("(stdin 不可读或已中断,默认拒绝)");
                }
            }
            coordinator.resolve_approval(req.id, approved);
            if approved {
                log::info!(
                    target: "Host",
                    "已批准配对: {} ({}) — 已加入 TrustStore",
                    req.device_name, req.fingerprint
                );
            } else {
                log::warn!(target: "Host", "已拒绝配对: {}", req.device_name);
            }
        }

        std::thread::sleep(std::time::Duration::from_millis(200));
    }

    log::info!(target: "Host", "Shutting down coordinator...");
    coordinator.stop();
    0
}

// PendingApproval is re-exported for parity with the C++ header surface.
#[allow(unused)]
fn _unused(_p: PendingApproval) {}
