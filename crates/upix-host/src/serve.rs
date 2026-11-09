//! Rust port of `engine/core/serve/daemon.cpp` — the warm-engine Unix-socket
//! daemon — plus the client-side helpers (`status`/`stop`/`ensure_running`/
//! `request`) that the CLI's `daemon` / first-upscale-auto-start paths use.
//!
//! Semantics preserved from the C++:
//!   * the daemon is the whole pipeline: the client sends PATHS (no image
//!     bytes cross the socket) — read → decode → upscale → encode → write;
//!   * requests are framed with the shared wire protocol (opcodes 0x0601–
//!     0x0604: SERVE_REQUEST / RESPONSE / DATA / PROGRESS), plaintext, header
//!     checksum enforced (encrypted frames never appear on this socket);
//!   * every read AND write is bounded (30s) — a client that connects and
//!     sends nothing must not park a thread forever (and the idle timeout
//!     with it), a client that stops draining must not freeze a send held
//!     under the engine mutex;
//!   * FIFO/socket input refused (a request could hang `daemon stop` forever
//!     inside uninterruptible file IO); character devices stay allowed;
//!   * lazy model switch under the engine lock when a request names a
//!     different model;
//!   * ServeProgress stage 0 (tile counters, throttled to whole-percent
//!     changes) then stage 1 (encode starting);
//!   * output path derivation: client-named path wins; else `<stem>_<model>`
//!     with the input extension when it maps to a known encoder, else .png;
//!   * idle 600s self-exit (a connection in flight always suppresses the
//!     idle tick, and a finished request refreshes the timer);
//!   * teardown: cancel flags first (inference stops at the next tile
//!     boundary), then socket shutdowns wake blocked readers, then joins.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use upix_core::engine::TileConfig;
use upix_img::EncoderOptions;
use upix_net::protocol::{MessageType, HEADER_SIZE, MAX_NETWORK_PAYLOAD};

use crate::coordinator::Framer;
use crate::seams::{Backend, Transport, UdsFactory};
use crate::wireio;

// Path length cap for the path-driven request (client sends absolute paths).
const MAX_SERVE_PATH_BYTES: usize = 4096;
// Payload can never legitimately exceed the fixed struct plus both capped
// paths (the hostile ~4 GB declaration guard).
const MAX_SERVE_PAYLOAD_BYTES: usize = wireio::SERVE_REQUEST_SIZE + 2 * MAX_SERVE_PATH_BYTES;
// Bound every read AND write on a connection (30 s is far beyond any
// legitimate request/response gap on a local socket).
const SERVE_IO_TIMEOUT_MS: i64 = 30 * 1000;
/// CLI default idle exit (`cmd_daemon`), applied when the config passes 0/absent.
pub const DEFAULT_IDLE_TIMEOUT_S: i64 = 600;

/// Default socket path: `$XDG_CACHE_HOME/upix/serve.sock` or
/// `$HOME/.cache/upix/serve.sock`. Empty if neither env nor HOME is set.
pub fn default_serve_socket_path() -> String {
    if let Ok(cache) = std::env::var("XDG_CACHE_HOME") {
        if !cache.is_empty() {
            return format!("{cache}/upix/serve.sock");
        }
    }
    let home = std::env::var("HOME").unwrap_or_default();
    if home.is_empty() {
        return String::new();
    }
    format!("{home}/.cache/upix/serve.sock")
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServeDevice {
    Cpu,
    Gpu,
}

pub struct ServeConfig {
    pub socket_path: String,
    pub models_dir: PathBuf,
    /// Initial model; empty = first available after scan.
    pub model_name: String,
    pub device: ServeDevice,
    /// 0 disables the idle exit; the CLI default is 600 s.
    pub idle_timeout_s: i64,
    pub backend: Arc<dyn Backend>,
    pub framer: Arc<dyn Framer>,
    pub uds: Box<dyn UdsFactory>,
}

// ── daemon internals ────────────────────────────────────────────────────────

struct EngineSlot {
    engine: Option<Box<dyn upix_core::engine::Engine>>,
    loaded_model: String,
}

struct ConnEntry {
    done: AtomicBool,
    cancel: AtomicBool,
    socket: Arc<dyn Transport>,
    handle: Mutex<Option<std::thread::JoinHandle<()>>>,
}

struct DaemonState {
    stop: AtomicBool,
    engine: Mutex<EngineSlot>,
    active_conns: AtomicI32,
    last_active_ms: AtomicI64,
    conns: Mutex<Vec<Arc<ConnEntry>>>,
    cfg: ServeConfig,
}

fn steady_now_ms() -> i64 {
    use std::time::SystemTime;
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn send_via(sock: &dyn Transport, framer: &dyn Framer, ty: MessageType, payload: &[u8]) -> bool {
    let wire = framer.serialize_frame(ty, payload);
    sock.send_all(&wire).is_ok()
}

fn send_serve_error(sock: &dyn Transport, framer: &dyn Framer, message: &str) {
    let zeros = [0u8; wireio::SERVE_RESPONSE_SIZE];
    let mut r = wireio::dec_serve_response(&zeros).unwrap_or(upix_net::protocol::ServeResponse {
        ok: 0,
        width: 0,
        height: 0,
        channels: 0,
        out_path_length: 0,
        error: [0u8; 128],
        model_name: [0u8; 64],
        skipped: 0,
        reserved0: 0,
    });
    r.ok = 0;
    let msg = message.as_bytes();
    let n = msg.len().min(127);
    r.error[..n].copy_from_slice(&msg[..n]);
    let _ = send_via(sock, framer, MessageType::ServeResponse, &wireio::enc_serve_response(&r));
}

fn send_serve_skipped(sock: &dyn Transport, framer: &dyn Framer) {
    let r = upix_net::protocol::ServeResponse {
        ok: 1,
        width: 0,
        height: 0,
        channels: 0,
        out_path_length: 0,
        error: [0u8; 128],
        model_name: [0u8; 64],
        skipped: 1,
        reserved0: 0,
    };
    let _ = send_via(sock, framer, MessageType::ServeResponse, &wireio::enc_serve_response(&r));
}

fn select_initial_model(backend: &dyn Backend, dir: &Path, requested: &str) -> Result<String, String> {
    backend.scan_models(dir)?;
    if !requested.is_empty() {
        if backend.get_model(requested).is_some() {
            return Ok(requested.to_string());
        }
        return Err(format!("model not found: {requested}"));
    }
    // empty = first available
    backend
        .available_models()
        .first()
        .map(|m| m.name.clone())
        .ok_or_else(|| "no models found".to_string())
}

fn build_state(cfg: ServeConfig) -> Result<Arc<DaemonState>, String> {
    // Engine init + model load (create → scan → select → load recipe).
    // Collapses the distinct failure codes to a single error string; the
    // only callers treat the error opaquely.
    let model_name = select_initial_model(cfg.backend.as_ref(), &cfg.models_dir, &cfg.model_name)?;
    let mut engine = cfg.backend.create_engine()?;
    let model = cfg
        .backend
        .get_model(&model_name)
        .ok_or_else(|| format!("model not found: {model_name}"))?;
    engine
        .load_model(&model.param_path, &model.bin_path)
        .map_err(|e| format!("load_model({model_name}) failed: {}", e.message))?;
    let scale = engine.get_scale_factor();
    let dev = match cfg.device {
        ServeDevice::Gpu => "GPU",
        ServeDevice::Cpu => "CPU",
    };
    log::info!(
        "daemon ready: socket={} model={model_name} scale={scale} device={dev} pid={}",
        cfg.socket_path,
        std::process::id()
    );
    log::info!("`upix process` will now reuse this warm engine");
    Ok(Arc::new(DaemonState {
        stop: AtomicBool::new(false),
        engine: Mutex::new(EngineSlot {
            engine: Some(engine),
            loaded_model: model_name,
        }),
        active_conns: AtomicI32::new(0),
        last_active_ms: AtomicI64::new(steady_now_ms()),
        conns: Mutex::new(Vec::new()),
        cfg,
    }))
}

fn spawn_conn_thread(state: Arc<DaemonState>, sock: Arc<dyn Transport>) -> Result<(), String> {
    let entry = Arc::new(ConnEntry {
        done: AtomicBool::new(false),
        cancel: AtomicBool::new(false),
        socket: sock,
        handle: Mutex::new(None),
    });
    state.conns.lock().unwrap().push(Arc::clone(&entry));
    // The record is handed to the vector BEFORE the thread starts: a spawn
    // failure must never leave a running thread writing e->done through a
    // freed pointer.
    let st2 = Arc::clone(&state);
    let e2 = Arc::clone(&entry);
    let spawned = std::thread::Builder::new()
        .name("upix-daemon-conn".into())
        .spawn(move || {
            handle_connection(st2, Arc::clone(&e2.socket), &e2.cancel);
            e2.done.store(true, Ordering::SeqCst); // reaper may now join us
        });
    match spawned {
        Ok(h) => {
            *entry.handle.lock().unwrap() = Some(h);
            Ok(())
        }
        Err(e) => {
            // Drop the record and give the connection slot back so the idle
            // timeout keeps working.
            state.conns.lock().unwrap().pop();
            state.active_conns.fetch_sub(1, Ordering::SeqCst);
            Err(format!("cannot start connection thread: {e}"))
        }
    }
}

fn reap_finished_connections(state: &DaemonState) {
    // Runs on the accept thread only. join() on a done thread returns
    // immediately; without this, every served connection would hold its
    // thread handle until process exit.
    let mut conns = state.conns.lock().unwrap();
    conns.retain(|e| {
        if e.done.load(Ordering::SeqCst) {
            let h = e.handle.lock().unwrap().take();
            if let Some(h) = h {
                let _ = h.join();
            }
            false
        } else {
            true
        }
    });
}

fn drain_connections(state: &DaemonState) {
    // Cancel first: a request already inside inference stops at the next
    // tile boundary instead of running to completion before the joins.
    let conns: Vec<Arc<ConnEntry>> = {
        let mut c = state.conns.lock().unwrap();
        std::mem::take(&mut *c)
    };
    for c in &conns {
        c.cancel.store(true, Ordering::SeqCst);
    }
    // Interrupt readers before joining: shutdown on the socket wakes a recv()
    // blocked on an unresponsive client.
    for c in &conns {
        c.socket.shutdown();
    }
    for c in conns {
        let h = c.handle.lock().unwrap().take();
        if let Some(h) = h {
            let _ = h.join();
        }
    }
}

fn daemon_accept_loop(state: Arc<DaemonState>) -> Result<(), String> {
    let listener = state.cfg.uds.listen(Path::new(&state.cfg.socket_path))?;
    while !state.stop.load(Ordering::SeqCst) {
        reap_finished_connections(&state);
        match listener.accept() {
            Ok(Some(sock)) => {
                state.active_conns.fetch_add(1, Ordering::SeqCst);
                state.last_active_ms.store(steady_now_ms(), Ordering::SeqCst);
                let sock: Arc<dyn Transport> = Arc::from(sock);
                let _ = spawn_conn_thread(Arc::clone(&state), sock);
            }
            Ok(None) => {
                // Poll-timeout tick: exit when idle. A connection in flight
                // always suppresses this, and a finished request refreshes
                // last_active, so a long request never counts its own
                // runtime as idle time.
                if state.cfg.idle_timeout_s > 0 && state.active_conns.load(Ordering::SeqCst) == 0 {
                    let idle_ms = steady_now_ms() - state.last_active_ms.load(Ordering::SeqCst);
                    if idle_ms / 1000 >= state.cfg.idle_timeout_s {
                        log::info!("idle for {}s, exiting", idle_ms / 1000);
                        break;
                    }
                }
                continue;
            }
            Err(e) => {
                if !state.stop.load(Ordering::SeqCst) {
                    log::warn!("accept error: {e}");
                }
                continue;
            }
        }
    }
    // Same teardown path for stop(), fatal error, and idle exit.
    drain_connections(&state);
    drop(listener);
    let _ = std::fs::remove_file(&state.cfg.socket_path);
    log::info!("daemon shutting down");
    Ok(())
}

/// Signal shutdown from any thread (SIGTERM handler, `daemon stop`, tests).
#[derive(Clone)]
pub struct ServeDaemonHandle {
    state: Arc<DaemonState>,
}

impl ServeDaemonHandle {
    pub fn stop(&self) {
        self.state.stop.store(true, Ordering::SeqCst);
    }
}

/// In-process variant (tests / embedding): engine init happens on this
/// thread, the accept loop runs on its own thread. Returns when the loop
/// exits (stop or idle) via the returned handle's `stop()`.
pub fn run_daemon(cfg: ServeConfig) -> Result<ServeDaemonHandle, String> {
    let state = build_state(cfg)?;
    let handle = ServeDaemonHandle {
        state: Arc::clone(&state),
    };
    std::thread::Builder::new()
        .name("upix-daemon-run".into())
        .spawn(move || {
            if let Err(e) = daemon_accept_loop(state) {
                log::error!("daemon loop failed: {e}");
            }
        })
        .map_err(|e| format!("cannot start daemon thread: {e}"))?;
    Ok(handle)
}

/// Foreground variant with the pinned CONTRACT signature semantics
/// (`serve::run_daemon(cfg)` blocking until stop/idle). Runs the accept loop
/// on the calling thread.
pub fn run_daemon_fg(cfg: ServeConfig) -> Result<(), String> {
    let state = build_state(cfg)?;
    daemon_accept_loop(state)
}

// ── per-connection pipeline ─────────────────────────────────────────────────

fn handle_connection(state: Arc<DaemonState>, sock: Arc<dyn Transport>, cancel: &AtomicBool) {
    // Idle-timeout accounting: a finished request never counts its own
    // runtime toward the idle timer.
    struct ConnGuard(Arc<DaemonState>);
    impl Drop for ConnGuard {
        fn drop(&mut self) {
            self.0.active_conns.fetch_sub(1, Ordering::SeqCst);
            self.0.last_active_ms.store(steady_now_ms(), Ordering::SeqCst);
        }
    }
    let _guard = ConnGuard(Arc::clone(&state));

    match handle_connection_inner(Arc::clone(&state), sock.as_ref(), cancel) {
        Ok(()) => {}
        Err(e) => {
            // The body runs inside this guard so an allocation failure
            // answers the client instead of terminating the daemon (the
            // client may be gone; nothing left to do on send failure).
            log::error!("request failed with error: {e}");
            send_serve_error(
                sock.as_ref(),
                state.cfg.framer.as_ref(),
                "internal error (see daemon log)",
            );
        }
    }
}

fn handle_connection_inner(
    state: Arc<DaemonState>,
    sock: &dyn Transport,
    cancel: &AtomicBool,
) -> Result<(), String> {
    let framer = state.cfg.framer.as_ref();
    sock.set_recv_timeout_ms(SERVE_IO_TIMEOUT_MS);

    // 1. Read request header.
    let mut hdr = [0u8; HEADER_SIZE];
    if sock.receive_exact(&mut hdr).is_err() {
        // EOF/err before header: a quiet client — nothing to answer.
        return Ok(());
    }
    let Some(parsed) = framer.parse_header(&hdr) else {
        send_serve_error(sock, framer, "expected MSG_SERVE_REQUEST");
        return Ok(());
    };
    if parsed.msg_type != MessageType::ServeRequest
        || (parsed.payload_length as usize) < wireio::SERVE_REQUEST_LEGACY_SIZE
        || parsed.payload_length as usize > MAX_SERVE_PAYLOAD_BYTES
    {
        send_serve_error(sock, framer, "expected MSG_SERVE_REQUEST");
        return Ok(());
    }

    // 2. Read payload = [ServeRequest][input path][output path].
    let mut req_buf = vec![0u8; parsed.payload_length as usize];
    if sock.receive_exact(&mut req_buf).is_err() {
        return Ok(());
    }

    // Plaintext frames enforce the header checksum (encrypted frames carry 0
    // and rely on Poly1305 instead — they never appear on this socket).
    // Reject before touching the payload: a mismatch means corruption or a
    // non-upix writer on the socket path.
    let mut hdr14 = [0u8; HEADER_SIZE];
    hdr14.copy_from_slice(&hdr);
    if !framer.verify_frame(&hdr14, &req_buf) {
        send_serve_error(sock, framer, "frame checksum mismatch");
        return Ok(());
    }

    let Some(req) = wireio::dec_serve_request(&req_buf) else {
        send_serve_error(sock, framer, "expected MSG_SERVE_REQUEST");
        return Ok(());
    };
    let paths_bytes = req.input_path_length as usize + req.output_path_length as usize;
    let req_bytes = if req_buf.len() == wireio::SERVE_REQUEST_LEGACY_SIZE + paths_bytes {
        wireio::SERVE_REQUEST_LEGACY_SIZE
    } else {
        req_buf.len().min(wireio::SERVE_REQUEST_SIZE)
    };
    let req = if req_bytes < wireio::SERVE_REQUEST_SIZE {
        let mut r = req;
        r.force = 0;
        r.reserved = [0u8; 7];
        r
    } else {
        req
    };
    if paths_bytes != req_buf.len() - req_bytes
        || req.input_path_length == 0
        || req.input_path_length as usize > MAX_SERVE_PATH_BYTES
        || req.output_path_length as usize > MAX_SERVE_PATH_BYTES
    {
        send_serve_error(sock, framer, "malformed request paths");
        return Ok(());
    }
    let input_path = String::from_utf8_lossy(
        &req_buf[req_bytes..req_bytes + req.input_path_length as usize],
    )
    .into_owned();
    let output_path = String::from_utf8_lossy(&req_buf[req_bytes + req.input_path_length as usize..])
        .into_owned();
    drop(req_buf);

    let backend = state.cfg.backend.as_ref();

    // A client-named output format must be validated BEFORE the (possibly
    // minutes-long) inference: an unsupported value would otherwise burn a
    // full upscale and then die in the encoder. Empty = derive from paths.
    let req_fmt = wireio::rd_bounded_str(&req.output_format, 0, 16);
    if !req_fmt.is_empty() {
        let normalized = backend.normalize_output_format(&req_fmt);
        if !backend.is_supported_output_format(&normalized) {
            send_serve_error(sock, framer, &format!("unsupported output format: {req_fmt}"));
            return Ok(());
        }
    }

    // FIFOs (and sockets) as input would block a connection thread inside
    // file I/O with no deadline — request_stop/shutdown in teardown cannot
    // unblock it, so a single request could hang `daemon stop` forever.
    // Character devices (/dev/null, /dev/zero) read 0 bytes and fail cleanly,
    // so they stay allowed.
    if let Ok(md) = std::fs::metadata(&input_path) {
        use std::os::unix::fs::FileTypeExt;
        let ft = md.file_type();
        if ft.is_fifo() || ft.is_socket() {
            send_serve_error(sock, framer, "load failed: refusing fifo/socket input");
            return Ok(());
        }
    }

    // 3. Full hot-path pipeline: read → decode → upscale → encode → write.
    // The client never touches image bytes; its own footprint stays near
    // zero. Bytes are read once and shared by the EXIF orientation sniff and
    // the decode (portrait JPEGs are baked upright right after decode).
    let raw = match backend.load_raw(Path::new(&input_path)) {
        Ok(r) => r,
        Err(e) => {
            send_serve_error(sock, framer, &format!("load failed: {e}"));
            return Ok(());
        }
    };
    if req.force == 0
        && let Some(marker) = upix_img::sr_marker::probe(&raw)
    {
        log::info!(
            "Skipped (already super-resolved by this tool): {} [model={} scale={}x ts={}]",
            input_path,
            marker.model,
            marker.scale,
            marker.ts
        );
        send_serve_skipped(sock, framer);
        return Ok(());
    }
    let exif_orientation = backend.exif_orientation(&raw);
    let ext = Path::new(&input_path)
        .extension()
        .map(|e| e.to_string_lossy().into_owned())
        .unwrap_or_default();
    let mut image = match backend.load_from_memory(&ext, &raw) {
        Ok(i) => i,
        Err(e) => {
            send_serve_error(sock, framer, &format!("load failed: {e}"));
            return Ok(());
        }
    };
    drop(raw);
    if image.data.is_empty() {
        send_serve_error(sock, framer, "decoded image is empty");
        return Ok(());
    }
    if exif_orientation != 1 && backend.apply_orientation(&mut image, exif_orientation) {
        log::info!("applied EXIF orientation {exif_orientation}");
    }

    let out_w;
    let out_h;
    let used_scale;
    let out_image: Option<crate::seams::Img>;
    {
        // Engine access serialized by engine_mutex_ (Vulkan constraint);
        // lazy model switch when the request names a different model.
        let mut slot = state.engine.lock().unwrap();
        let req_model = wireio::rd_bounded_str(&req.model_name, 0, 64);
        if !req_model.is_empty() && slot.loaded_model != req_model {
            let Some(m) = backend.get_model(&req_model) else {
                send_serve_error(sock, framer, "model load failed");
                return Ok(());
            };
            let engine = slot.engine.as_mut().ok_or("engine missing")?;
            if engine.load_model(&m.param_path, &m.bin_path).is_err() {
                send_serve_error(sock, framer, "model load failed");
                return Ok(());
            }
            slot.loaded_model = req_model;
        }
        let engine = slot.engine.as_ref().ok_or("engine missing")?;
        let scale = engine.get_scale_factor();
        used_scale = scale;

        // 64-bit math, then range-check before narrowing: an overflowed int
        // would size the output from a bogus (often negative → huge) value.
        let out_w64 = image.width as i64 * scale as i64;
        let out_h64 = image.height as i64 * scale as i64;
        let bpp = image.channels.max(1) as u64;
        let overflow =
            out_w64 > 0 && out_h64 > 0 && (out_w64 as u64) > u64::MAX / (out_h64 as u64 * bpp);
        if out_w64 <= 0
            || out_h64 <= 0
            || out_w64 > i32::MAX as i64
            || out_h64 > i32::MAX as i64
            || overflow
        {
            send_serve_error(sock, framer, "upscaled output out of range");
            log::error!(
                "Upscaled output out of range: {}x{} at {}x",
                image.width,
                image.height,
                scale
            );
            return Ok(());
        }
        out_w = out_w64 as i32;
        out_h = out_h64 as i32;

        // Wire tile/overlap are clamped to the same envelope
        // ProcessingConfig::validate enforces on CLI configs (tile ≤ 2048,
        // overlap ≤ tile/2): a hostile overlap ≥ tile would degenerate the
        // grid to step=1 — tens of thousands of full-size tiles on one
        // request (hours of engine time); a huge tile turns a large input
        // into a single giant forward.
        let tile_size = if req.tile_size > 0 {
            req.tile_size.clamp(1, 2048)
        } else {
            // tile_size 0 = "auto": the engine recommendation the inline path
            // uses, so daemon-delegated and inline output stay byte-identical.
            engine.get_recommended_tile_size()
        };
        let tc = TileConfig {
            tile_size,
            overlap: req.tile_overlap.clamp(0, tile_size / 2),
            batch_size: 1,
        };

        // Stream tile progress to the client (same live progress line the
        // inline path shows), throttled to whole-percent changes. A failed
        // progress send is non-fatal (the terminal response reports real
        // errors). `cancel` is owned by this connection's ConnEntry:
        // drain_connections() sets it at shutdown so the remaining tiles are
        // skipped instead of holding teardown for minutes.
        let mut last_sent_pct = -1i32;
        let mut progress_sink = |prog: &upix_core::engine::ProcessingProgress| {
            let pct = prog.percentage() as i32;
            if pct == last_sent_pct {
                return;
            }
            last_sent_pct = pct;
            let msg = upix_net::protocol::ServeProgress {
                completed_tiles: prog.completed_tiles as u32,
                total_tiles: prog.total_tiles as u32,
                stage: 0,
            };
            let _ = send_via(
                sock,
                framer,
                MessageType::ServeProgress,
                &wireio::enc_serve_progress(&msg),
            );
        };

        let res = backend.process_image_u8(
            engine.as_ref(),
            tc,
            &image.data,
            image.width,
            image.height,
            image.channels,
            Some(&mut progress_sink),
            cancel,
            /*enable_alpha_bicubic=*/ true,
        );
        match res {
            Ok(data) => {
                out_image = Some(crate::seams::Img {
                    width: out_w,
                    height: out_h,
                    channels: image.channels,
                    data,
                });
            }
            Err(e) => {
                send_serve_error(sock, framer, &format!("inference failed: {}", e.message));
                return Ok(());
            }
        }
    }

    // Tell the client inference is over and the encode (which can take a
    // couple of seconds for avif) is starting.
    {
        let msg = upix_net::protocol::ServeProgress {
            completed_tiles: 0,
            total_tiles: 0,
            stage: 1,
        };
        let _ = send_via(sock, framer, MessageType::ServeProgress, &wireio::enc_serve_progress(&msg));
    }

    // Model actually used (ensure/switch may have reloaded); read under the
    // lock — it feeds both the derived output name and the response.
    let used_model = state.engine.lock().unwrap().loaded_model.clone();

    // 4. Encode + write. The output format comes from the request (which
    // resolved --format / config file client-side); with none given, keep
    // the input extension when it maps to a known encoder (same rule as the
    // CLI's derive_default_output).
    let eff_out: PathBuf = if !output_path.is_empty() {
        PathBuf::from(&output_path)
    } else {
        let inp = Path::new(&input_path);
        let mut ext = inp
            .extension()
            .map(|e| e.to_string_lossy().into_owned())
            .unwrap_or_default();
        let stem = inp
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        if !backend.is_supported_output_format(&backend.normalize_output_format(&ext)) {
            ext = "png".to_string();
        }
        inp.parent()
            .unwrap_or(Path::new(""))
            .join(format!(
                "{}_{}.{}",
                stem,
                backend.sanitize_model_for_filename(&used_model),
                ext
            ))
    };
    let final_path = backend.resolve_output_path(&eff_out, &req_fmt);
    if let Some(parent) = final_path.parent() {
        if !parent.as_os_str().is_empty() {
            let _ = std::fs::create_dir_all(parent);
        }
    }
    // Same SR marker the inline CLI path writes, so daemon-served outputs
    // are recognized on re-input too.
    let sr_xmp = backend
        .sr_xmp(
            &used_model,
            used_scale,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs() as i64)
                .unwrap_or(0),
        );
    let out_img = out_image.ok_or_else(|| "internal: output missing".to_string())?;
    let encoder_opts: EncoderOptions = wireio::dec_opts_wire(req.encoder);
    let save = backend.save_image(
        &out_img,
        &final_path,
        req.output_quality,
        Some(&sr_xmp),
        &encoder_opts,
    );
    if let Err(e) = save {
        send_serve_error(sock, framer, &format!("save failed: {e}"));
        return Ok(());
    }
    log::info!(
        "served {}x{} -> {}x{} => {}",
        image.width,
        image.height,
        out_w,
        out_h,
        final_path.display()
    );

    // 5. Response: [ServeResponse][output path]. No image bytes follow —
    // the daemon already wrote the file.
    let out_str = final_path.to_string_lossy().into_owned();
    let mut resp = upix_net::protocol::ServeResponse {
        ok: 1,
        width: out_w,
        height: out_h,
        channels: image.channels,
        out_path_length: out_str.len() as u32,
        error: [0u8; 128],
        model_name: [0u8; 64],
        skipped: 0,
        reserved0: 0,
    };
    let mb = used_model.as_bytes();
    let n = mb.len().min(63);
    resp.model_name[..n].copy_from_slice(&mb[..n]);
    let mut head = wireio::enc_serve_response(&resp);
    head.extend_from_slice(out_str.as_bytes());
    let _ = send_via(sock, framer, MessageType::ServeResponse, &head);
    Ok(())
}

// ── client side ─────────────────────────────────────────────────────────────

pub struct ServeRequestArgs {
    pub input_path: PathBuf,
    /// None → daemon derives `<stem>_<model>.<ext>`.
    pub output_path: Option<PathBuf>,
    pub model_name: Option<String>,
    pub output_format: Option<String>,
    pub tile_size: i32,
    pub tile_overlap: i32,
    pub output_quality: i32,
    pub encoder: EncoderOptions,
    pub force: bool,
}

#[derive(Debug, Clone)]
pub struct ServeOutcome {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub out_path: PathBuf,
    pub model_name: String,
    pub skipped: bool,
}

/// One request/response exchange over the daemon socket. Progress frames
/// surface through `on_progress` (stage 0 ticks carry tile counters).
pub fn request(
    uds: &dyn UdsFactory,
    framer: &dyn Framer,
    socket_path: &Path,
    args: &ServeRequestArgs,
    on_progress: Option<&dyn Fn(&upix_net::protocol::ServeProgress)>,
) -> Result<ServeOutcome, String> {
    let sock = uds.connect(socket_path)?;
    sock.set_recv_timeout_ms(SERVE_IO_TIMEOUT_MS);
    let input = args.input_path.to_string_lossy().into_owned().into_bytes();
    let output = args
        .output_path
        .as_ref()
        .map(|p| p.to_string_lossy().into_owned().into_bytes())
        .unwrap_or_default();
    if input.is_empty() || input.len() > MAX_SERVE_PATH_BYTES || output.len() > MAX_SERVE_PATH_BYTES
    {
        return Err("invalid request paths".into());
    }
    let mut req = upix_net::protocol::ServeRequest {
        model_name: [0u8; 64],
        output_format: [0u8; 16],
        tile_size: args.tile_size,
        tile_overlap: args.tile_overlap,
        output_quality: args.output_quality,
        input_path_length: input.len() as u32,
        output_path_length: output.len() as u32,
        encoder: wireio::enc_opts_wire(&args.encoder),
        force: u8::from(args.force),
        reserved: [0u8; 7],
    };
    if let Some(m) = &args.model_name {
        let mb = m.as_bytes();
        let n = mb.len().min(63);
        req.model_name[..n].copy_from_slice(&mb[..n]);
    }
    if let Some(f) = &args.output_format {
        let fb = f.as_bytes();
        let n = fb.len().min(15);
        req.output_format[..n].copy_from_slice(&fb[..n]);
    }
    let mut payload = wireio::enc_serve_request(&req);
    payload.extend_from_slice(&input);
    payload.extend_from_slice(&output);
    let wire = framer.serialize_frame(MessageType::ServeRequest, &payload);
    sock.send_all(&wire).map_err(|e| format!("send: {e}"))?;

    // Read frames until the terminal ServeResponse.
    loop {
        let mut hdr = [0u8; HEADER_SIZE];
        sock.receive_exact(&mut hdr).map_err(|e| format!("recv header: {e}"))?;
        let Some(h) = framer.parse_header(&hdr) else {
            return Err("bad header from daemon".into());
        };
        if h.payload_length > MAX_NETWORK_PAYLOAD {
            return Err("oversized daemon frame".into());
        }
        let mut payload = vec![0u8; h.payload_length as usize];
        if h.payload_length > 0 {
            sock.receive_exact(&mut payload)
                .map_err(|e| format!("recv payload: {e}"))?;
        }
        match h.msg_type {
            MessageType::ServeProgress => {
                if let Some(cb) = on_progress {
                    if let Some(p) = wireio::dec_serve_progress(&payload) {
                        cb(&p);
                    }
                }
            }
            MessageType::ServeData => {
                // Legacy containers that streamed result bytes; the current
                // daemon writes the file itself. Drain and ignore.
            }
            MessageType::ServeResponse => {
                let Some(r) = wireio::dec_serve_response(&payload) else {
                    return Err("bad ServeResponse".into());
                };
                if r.ok != 1 {
                    let msg = wireio::rd_bounded_str(&payload, 20, 128);
                    return Err(if msg.is_empty() {
                        "serve failed".to_string()
                    } else {
                        msg
                    });
                }
                let resp_bytes = if payload.len() == wireio::SERVE_RESPONSE_LEGACY_SIZE + r.out_path_length as usize {
                    wireio::SERVE_RESPONSE_LEGACY_SIZE
                } else {
                    payload.len().min(wireio::SERVE_RESPONSE_SIZE)
                };
                if r.skipped != 0 {
                    return Ok(ServeOutcome {
                        width: 0,
                        height: 0,
                        channels: 0,
                        out_path: PathBuf::new(),
                        model_name: String::new(),
                        skipped: true,
                    });
                }
                // Clamp to the actual payload: a stale/hostile daemon reply
                // with an inflated out_path_length must not slice-panic the
                // client (release is panic=abort).
                let path_end = (resp_bytes
                    .saturating_add(r.out_path_length as usize))
                    .min(payload.len());
                let out_path = String::from_utf8_lossy(&payload[resp_bytes..path_end])
                    .into_owned();
                return Ok(ServeOutcome {
                    width: r.width,
                    height: r.height,
                    channels: r.channels,
                    out_path: PathBuf::from(out_path),
                    model_name: wireio::rd_bounded_str(&payload, 148, 64),
                    skipped: false,
                });
            }
            other => return Err(format!("unexpected daemon frame type {}", other as u16)),
        }
    }
}

// ── process management (status / stop / ensure_running) ────────────────────

/// Read-only daemon status: pid file + liveness + socket reachability.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DaemonStatus {
    pub running: bool,
    pub pid: Option<i32>,
    pub socket_present: bool,
    pub socket_path: String,
}

pub fn pid_file_path(socket_path: &str) -> String {
    format!("{socket_path}.pid")
}

/// `pid_is_upix_daemon`: kill(0)-equivalent liveness is implied by readable
/// /proc; the exe symlink is the reliable check — it fails for zombies and
/// names the real binary otherwise. And it must be the daemon subcommand,
/// not an unrelated upix process.
fn pid_alive_upix_daemon(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    let exe = match std::fs::read_link(format!("/proc/{pid}/exe")) {
        Ok(e) => e.to_string_lossy().into_owned(),
        Err(_) => return false,
    };
    if !exe.contains("upix") {
        return false;
    }
    match std::fs::read(format!("/proc/{pid}/cmdline")) {
        Ok(bytes) => String::from_utf8_lossy(&bytes).contains("daemon"),
        Err(_) => false,
    }
}

pub fn status(socket_path: &str) -> DaemonStatus {
    let pid_path = pid_file_path(socket_path);
    let socket_present = Path::new(socket_path).exists();
    let pid = std::fs::read_to_string(&pid_path)
        .ok()
        .and_then(|s| s.trim().parse::<i32>().ok())
        .filter(|&pid| pid_alive_upix_daemon(pid));
    DaemonStatus {
        running: pid.is_some(),
        pid,
        socket_present,
        socket_path: socket_path.to_string(),
    }
}

/// `daemon stop`: read the pid file, validate the pid (untrusted input —
/// signalling whatever a stale file names would be an arbitrary-pid SIGTERM
/// primitive), SIGTERM. The daemon's loop observes the flag on its next poll
/// tick and unlinks the pid file; a short wait confirms exit.
pub fn stop(socket_path: &str) -> Result<(), String> {
    let pid_path = pid_file_path(socket_path);
    let text = std::fs::read_to_string(&pid_path)
        .map_err(|_| format!("stop: daemon not running (no pid file at {pid_path})"))?;
    let pid: i32 = text
        .trim()
        .parse()
        .map_err(|_| format!("stop: corrupt pid file at {pid_path}"))?;
    if !pid_alive_upix_daemon(pid) {
        let _ = std::fs::remove_file(&pid_path);
        return Err(format!(
            "stop: pid {pid} is not a live upix daemon — removing stale pid file"
        ));
    }
    let rc = unsafe { libc::kill(pid, libc::SIGTERM) };
    if rc != 0 {
        let err = std::io::Error::last_os_error();
        if err.raw_os_error() == Some(libc::ESRCH) {
            let _ = std::fs::remove_file(&pid_path);
            return Err(format!("stop: pid {pid} vanished"));
        }
        return Err(format!("stop: kill pid {pid} failed: {err}"));
    }
    Ok(())
}

/// CLI pid-file duty (cmd_daemon): write after a successful start so `stop`
/// can find us; the start path unlinks it on shutdown.
pub fn write_pid_file(socket_path: &str) -> Result<(), String> {
    std::fs::write(pid_file_path(socket_path), std::process::id().to_string())
        .map_err(|e| format!("cannot write pid file: {e}"))
}

/// First-upscale auto-start: probe the socket; when the daemon is absent,
/// spawn a detached daemon child (the current executable re-invoked with
/// daemon args — the CLI integration maps them onto `upix daemon --fg`),
/// then wait for the socket to appear.
///
/// Spawned argv: `[exe, --upix-internal-daemon, socket, models_dir, model_or_empty, device, idle_timeout]`.
/// Returns Ok(true) when a daemon was started, Ok(false) when one was
/// already reachable.
pub fn ensure_running(
    uds: &dyn UdsFactory,
    socket_path: &str,
    models_dir: &Path,
    model_name: &str,
    device: ServeDevice,
    idle_timeout_s: i64,
    wait: Duration,
) -> Result<bool, String> {
    // Liveness alone can race with shutdown; the socket is the real contract.
    if uds.connect(Path::new(socket_path)).is_ok() {
        return Ok(false);
    }
    // pid-file-without-live-daemon → stale, remove so status won't lie.
    let st = status(socket_path);
    if !st.running {
        let _ = std::fs::remove_file(pid_file_path(socket_path));
    }
    let exe = std::env::current_exe().map_err(|e| format!("cannot locate self: {e}"))?;
    let dev = match device {
        ServeDevice::Gpu => "gpu",
        ServeDevice::Cpu => "cpu",
    };
    use std::process::Stdio;
    std::process::Command::new(exe)
        .args([
            "--upix-internal-daemon",
            socket_path,
            &models_dir.to_string_lossy(),
            model_name,
            dev,
            &idle_timeout_s.to_string(),
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| format!("cannot spawn daemon: {e}"))?;

    let deadline = std::time::Instant::now() + wait;
    while std::time::Instant::now() < deadline {
        if uds.connect(Path::new(socket_path)).is_ok() {
            return Ok(true);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Err("daemon did not come up in time".into())
}
