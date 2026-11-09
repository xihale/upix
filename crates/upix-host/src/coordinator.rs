//! Rust port of `legacy/share/core/distributed/coordinator.cpp` (+ `coordinator_admin.cpp`):
//! the PC-side coordinator of the phone↔PC offload protocol.
//!
//! Semantics preserved from the C++ (comments below cite the C++ reasoning
//! where a behavior looks arbitrary — those are measured-pit comments, not
//! style):
//!   * connection cap 8 (kMaxConcurrentClients), handshake-phase deadline 10s,
//!     session frame-idle timeout 60s, in-task frame poll 200ms;
//!   * every peer must send CONNECT_REQUEST through the pairing gate: trusted
//!     peers auto-approve, first-time peers block on an interactive approval
//!     (`auto_approve` skips the prompt; GUI mode routes requests through a
//!     channel instead of stdin), a denied peer may reconnect and must be
//!     approved again, `stop()` rejects all pending approvals;
//!   * CLIENT_SUBMIT dual format (legacy single-frame vs struct-only +
//!     MSG_CLIENT_DATA continuations, preallocated by `input_data_length`,
//!     peak ≈ total + 2×8MB chunk buffers);
//!   * a chunked submit interrupted mid-stream is answered with a per-task
//!     error and the session stays usable;
//!   * progress travels through atomic slots polled by the session pump —
//!     workers never write the socket directly while a task runs (the C++
//!     1900× convoy fix);
//!   * task-level cancellation (~200ms poll, stops at the next tile boundary,
//!     late CANCEL tolerated, `success=0` / `"canceled"` reply);
//!   * encoded results larger than MAX_NETWORK_PAYLOAD stream as
//!     MSG_CLIENT_DATA continuations;
//!   * decode/encode failures answer a per-task error without dropping the
//!     connection;
//!   * model list / upload / download ops (upload staging + backup rollback);
//!   * `pipeline_depth = 2` in CONNECT_RESPONSE (one computing + one fully
//!     staged; the pump actually runs up to 3 concurrent tasks).

use std::collections::{HashMap, VecDeque};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use upix_core::engine::{Engine, TileConfig};
use upix_img::EncoderOptions;
use upix_net::protocol::{
    AuthResponse, ClientConnectResponse, ClientTaskResponse,
    ClientTaskSubmit, MessageHeader, MessageType, TaskProgress, HEADER_SIZE, MAX_NETWORK_PAYLOAD,
};

use crate::seams::{
    Backend, CryptoProvider, Listener, ListenerFactory, RecvErr, SessionCodec, Signer, Transport,
    TrustApi, CLIENT_DATA_CHUNK_BYTES,
};
use crate::wireio;

// ── constants (verbatim from coordinator.cpp) ───────────────────────────────

/// Raw upscaled pixels are a transient buffer (encoded to jpg/png before
/// transport), so they may exceed MAX_NETWORK_PAYLOAD. Bound PC memory instead.
const MAX_RAW_OUTPUT_BYTES: u64 = 512 * 1024 * 1024;

/// Idle timeout between frames of a persistent phone session: submit N+1 only
/// leaves the phone after output N has been written to SAF and the checkpoint
/// saved, so the gap must tolerate file IO, not just framing.
const SESSION_FRAME_TIMEOUT_MS: i64 = 60 * 1000;

/// Deadline for the whole pre-session phase of an accepted connection: waiting
/// for the first frame, the ECDH handshake, and the pairing request. Without
/// this a peer that connects and never writes pins a handler thread in a
/// blocking recv() forever — the accept loop would spawn threads without
/// bound, and stop() (which joins the handlers) would never return.
const HANDSHAKE_TIMEOUT_MS: i64 = 10 * 1000;

/// While a task runs on its worker thread, the connection handler polls for
/// new frames with this receive timeout instead of blocking for the full
/// session timeout — a MSG_TASK_CANCEL (or a dead peer, or a stop() request)
/// is noticed within one tick, and the io lock is released between polls so
/// the worker's progress/response sends can interleave.
const TASK_FRAME_POLL_MS: i64 = 200;

const CONNECT_TIMEOUT_MS: i64 = 5000;
const ROLE_FRAME_TIMEOUT_MS: i64 = 30 * 1000;
/// Ceiling on simultaneous connections (see coordinator.hpp).
const MAX_CONCURRENT_CLIENTS: usize = 8;
/// Submit-ahead capture bound: the pump keeps at most this many captured
/// CLIENT_SUBMITs alive at once.
const MAX_AHEAD_CAPTURES: usize = 4;
/// Concurrent task workers per session: task N's encode/transfer overlaps
/// with task N+1's inference (engine lock released before encoding).
const MAX_INFLIGHT_TASKS: usize = 3;
/// Tail field of ClientConnectResponse (contract §6: pipeline_depth = 2).
const PIPELINE_DEPTH: u8 = 2;
/// secretbox nonce (24) + Poly1305 MAC (16) ride inside every sealed frame.
const SEAL_OVERHEAD: usize = 24 + 16;

/// Single-frame response cap: a RESPONSE frame is bounded by what the
/// receiver accepts on the wire (it rejects any frame whose payload_length
/// exceeds MAX_NETWORK_PAYLOAD) plus the seal overhead; everything past the
/// leading slice streams as MSG_CLIENT_DATA.
fn max_single_frame_plaintext() -> usize {
    MAX_NETWORK_PAYLOAD as usize - wireio::CLIENT_TASK_RESPONSE_SIZE - SEAL_OVERHEAD
}

#[derive(Debug, Clone)]
pub struct CoordinatorError {
    pub code: i32,
    pub message: String,
}

impl std::fmt::Display for CoordinatorError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "[{}] {}", self.code, self.message)
    }
}

/// A pending first-time pairing request (mirror of `PendingApproval`).
#[derive(Clone)]
pub struct PendingApproval {
    pub id: u64,
    pub device_name: String,
    pub fingerprint: String,
    pub identity_pub: [u8; 32],
    verdict: Arc<ApprovalVerdict>,
}

struct ApprovalVerdict {
    cell: Mutex<Option<u8>>, // None = open, Some(0)=denied, Some(1)=approved
    cv: Condvar,
}

impl ApprovalVerdict {
    fn fulfill(&self, v: u8) {
        let mut g = self.cell.lock().unwrap();
        if g.is_none() {
            *g = Some(v);
        }
        self.cv.notify_all();
    }
    fn is_resolved(&self) -> bool {
        self.cell.lock().unwrap().is_some()
    }
    fn value(&self) -> u8 {
        self.cell.lock().unwrap().unwrap_or(0)
    }
}

// ── configuration ───────────────────────────────────────────────────────────

pub struct CoordinatorConfig {
    pub port: u16,
    pub bind_addr: String,
    pub models_dir: PathBuf,
    /// Skip the interactive pairing prompt for first-time peers (the CLI's
    /// `--auto-approve`). The C++ coordinator had no such flag — the approval
    /// source was always the PC main loop; here the flag is on the coordinator
    /// so the GUI/CLI harnesses don't have to pre-open a channel just to
    /// rubber-stamp every request.
    pub auto_approve: bool,
    pub backend: Arc<dyn Backend>,
    pub framer: Arc<dyn Framer>,
    pub crypto: Arc<dyn CryptoProvider>,
    pub signer: Arc<dyn Signer>,
    pub trust: Option<Arc<dyn TrustApi>>,
    pub listener_factory: Box<dyn ListenerFactory>,
}

/// Frame header codec (upix-net `frame` pin). Split from [`CryptoProvider`]
/// because the KEY_HELLO exchange is plaintext while everything after it is
/// sealed.
pub trait Framer: Send + Sync {
    fn serialize_frame(&self, ty: MessageType, payload: &[u8]) -> Vec<u8>;
    fn parse_header(&self, buf: &[u8]) -> Option<MessageHeader>;
    /// Plaintext frames enforce the header checksum (sealed frames skip it —
    /// a plaintext CRC over ciphertext leaks bytes and Poly1305 already
    /// covers integrity; the session codec enforces that rule itself).
    fn verify_frame(&self, header: &[u8; HEADER_SIZE], payload: &[u8]) -> bool;
}

// ── shared coordinator state ────────────────────────────────────────────────

struct EngineState {
    engine: Option<Box<dyn Engine>>,
    initialized: bool,
    loaded_model: String,
}

struct ApprovalState {
    queue: VecDeque<PendingApproval>,
    /// Requests already popped by the UI, resolvable by id.
    open: HashMap<u64, Arc<ApprovalVerdict>>,
    next_id: u64,
}

struct ClientEntry {
    done: AtomicBool,
    /// Session wake handle: stop() shuts the descriptor down because a
    /// handler parked in a blocking socket call only observes the stop flag
    /// after its receive timeout (up to the 60s session window).
    wake: Mutex<Option<Arc<dyn Transport>>>,
    handle: Mutex<Option<std::thread::JoinHandle<()>>>,
}

struct Shared {
    cfg: CoordinatorConfig,
    running: AtomicBool,
    listener: Mutex<Option<Arc<dyn Listener>>>,
    engine: Mutex<EngineState>,
    models_mutex: Mutex<()>,
    trust_store_mutex: Mutex<()>,
    approvals: Mutex<ApprovalState>,
    approval_sink: Mutex<Option<mpsc::Sender<PendingApproval>>>,
    clients: Mutex<Vec<Arc<ClientEntry>>>,
    stop_mutex: Mutex<()>,
    on_task_progress: Mutex<Option<Box<dyn Fn(f32) + Send + Sync>>>,
    /// coordinator_admin.cpp::set_models_directory — an override applied at
    /// runtime (the base dir stays in cfg, which lives behind the shared Arc).
    models_dir_override: Mutex<Option<PathBuf>>,
}

impl Shared {
    fn is_running(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }
}

// ── per-connection state ────────────────────────────────────────────────────

struct Conn {
    socket: Arc<dyn Transport>,
    framer: Arc<dyn Framer>,
    device_id: String,
    session: Option<Arc<dyn SessionCodec>>,
    peer_id_pub: [u8; 32],
    peer_trusted: bool,
}

impl Conn {
    /// Blocking encrypted receive with the current read timeout.
    fn recv_encrypted(&self, sh: &Shared) -> Option<(MessageHeader, Vec<u8>)> {
        let session = self.session.as_ref()?;
        let mut hdr = [0u8; HEADER_SIZE];
        self.socket.receive_exact(&mut hdr).ok()?;
        let parsed = sh.cfg.framer.parse_header(&hdr)?;
        if parsed.payload_length > MAX_NETWORK_PAYLOAD {
            return None;
        }
        let mut wire = Vec::with_capacity(HEADER_SIZE + parsed.payload_length as usize);
        wire.extend_from_slice(&hdr);
        if parsed.payload_length > 0 {
            let mut payload = vec![0u8; parsed.payload_length as usize];
            self.socket.receive_exact(&mut payload).ok()?;
            wire.extend_from_slice(&payload);
        }
        session.open(&wire)
    }
}

/// Encrypted-frame helper: seal + send in one step.
fn send_sealed(conn: &Conn, ty: MessageType, payload: &[u8]) -> bool {
    let Some(session) = &conn.session else {
        return false;
    };
    match session.seal(ty, payload) {
        Some(wire) => conn.socket.send_all(&wire).is_ok(),
        None => false,
    }
}

enum FramePoll {
    Frame(MessageHeader, Vec<u8>),
    Timeout,
    Error,
}

/// Polled variant used by the in-task frame loop: waits at most `poll_ms` for
/// the FIRST byte of a frame; once bytes are in, the rest of the frame is
/// read with the full session timeout so a poll tick can never abandon a
/// partially received frame while the peer is merely slow (that would corrupt
/// the stream framing).
fn recv_encrypted_poll(conn: &Conn, poll_ms: i64) -> FramePoll {
    let Some(session) = &conn.session else {
        return FramePoll::Error;
    };
    conn.socket.set_recv_timeout_ms(poll_ms);
    let mut hdr = [0u8; HEADER_SIZE];
    let mut got = 0usize;
    let mut first = true;
    while got < HEADER_SIZE {
        match conn.socket.receive(&mut hdr[got..]) {
            Ok(0) => return FramePoll::Error,
            Ok(n) => {
                got += n;
                if got > 0 && first {
                    // First byte of the header is in: finish the frame with
                    // the full session timeout.
                    conn.socket.set_recv_timeout_ms(SESSION_FRAME_TIMEOUT_MS);
                    first = false;
                }
            }
            Err(RecvErr::WouldBlock) => {
                if got == 0 {
                    return FramePoll::Timeout;
                }
                // WouldBlock with partial progress: read timeout restarts
                // per call — but the first-byte window is over, so keep
                // waiting under the session timeout.
                continue;
            }
            Err(RecvErr::Dead(_)) => return FramePoll::Error,
        }
    }
    let Some(parsed) = conn.framer.parse_header(&hdr) else {
        return FramePoll::Error;
    };
    let mut wire = Vec::with_capacity(HEADER_SIZE + parsed.payload_length as usize);
    wire.extend_from_slice(&hdr);
    if parsed.payload_length > 0 {
        let mut payload = vec![0u8; parsed.payload_length as usize];
        if conn.socket.receive_exact(&mut payload).is_err() {
            return FramePoll::Error;
        }
        wire.extend_from_slice(&payload);
    }
    match session.open(&wire) {
        Some((h, p)) => FramePoll::Frame(h, p),
        None => FramePoll::Error,
    }
}



// ── per-task run state ──────────────────────────────────────────────────────

/// Shared state between the connection handler (frame reader) and the
/// per-task worker it spawns. See coordinator.hpp ClientTaskRun — including
/// the measured-pit comment: progress frames used to be sent directly by the
/// worker and convoyed against the handler's polled reads on the single io
/// mutex (~10 s per progress send, a 1900× slowdown). The worker now only
/// publishes into these atomics; the pump owns every socket write while a
/// task is in flight.
struct ClientTaskRun {
    cancel: AtomicBool,
    connection_lost: AtomicBool,
    done: AtomicBool,
    progress_pct: AtomicI32,  // -1 = nothing new to send
    progress_eta: AtomicU32,
    /// Submit-order gate for this run's socket writes: responses must reach
    /// the phone in submit order, so every send blocks until the pump flips
    /// send_ready for the oldest unfinished run.
    send_order: Mutex<SendOrder>,
    send_order_cv: Condvar,
}

struct SendOrder {
    ready: bool,
}

impl ClientTaskRun {
    fn new() -> Self {
        Self {
            cancel: AtomicBool::new(false),
            connection_lost: AtomicBool::new(false),
            done: AtomicBool::new(false),
            progress_pct: AtomicI32::new(-1),
            progress_eta: AtomicU32::new(0),
            send_order: Mutex::new(SendOrder { ready: false }),
            send_order_cv: Condvar::new(),
        }
    }

    fn canceled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }

    /// Wait until this run owns the socket-write turn (or cancel lands — by
    /// then the peer is going away either way). Polling fallback so a missed
    /// wakeup can never park a worker forever (the classic lost wakeup).
    fn await_send_turn(&self) {
        let deadline = Duration::from_millis(250);
        let mut g = self.send_order.lock().unwrap();
        while !g.ready && !self.canceled() {
            let (ng, _) = self
                .send_order_cv
                .wait_timeout(g, deadline)
                .unwrap();
            g = ng;
        }
    }

    fn open_send_gate(&self) {
        let mut g = self.send_order.lock().unwrap();
        g.ready = true;
        self.send_order_cv.notify_all();
    }
}

struct Inflight {
    run: Arc<ClientTaskRun>,
    handle: Option<std::thread::JoinHandle<()>>,
}

fn placeholder_join() -> Option<std::thread::JoinHandle<()>> {
    None
}

/// A CLIENT_SUBMIT captured off the session stream while a task was still
/// running (submit-ahead). For a chunked submit the struct frame arrives
/// first and its MSG_CLIENT_DATA continuations trickle in during later poll
/// ticks — staged_data parks those bytes so they are not read again from the
/// socket.
struct CapturedSubmit {
    payload: Vec<u8>, // full SUBMIT frame payload (struct [+ inline input])
    staged_data: Vec<u8>,
    ahead: Option<Arc<AheadDecode>>,
}

/// Submit-ahead prefetch state: once the HEAD queued entry's chunks are fully
/// staged while a task is still computing, a helper thread starts decoding it
/// (EXIF → decode → normalize → orientation bake) so the GPU gap between task
/// N and N+1 shrinks by the whole input-prep time.
struct AheadDecode {
    inner: Mutex<AheadInner>,
    cv: Condvar,
}

struct AheadInner {
    ready: bool,
    ok: bool,
    error: String,
    pixels: Vec<u8>,
    width: i32,
    height: i32,
    channels: i32,
}

// ── coordinator handle ──────────────────────────────────────────────────────

pub struct Coordinator {
    sh: Arc<Shared>,
    accept_handle: Mutex<Option<std::thread::JoinHandle<()>>>,
}

impl Coordinator {
    pub fn new(cfg: CoordinatorConfig) -> Self {
        Self {
            sh: Arc::new(Shared {
                running: AtomicBool::new(false),
                listener: Mutex::new(None),
                engine: Mutex::new(EngineState {
                    engine: None,
                    initialized: false,
                    loaded_model: String::new(),
                }),
                models_mutex: Mutex::new(()),
                trust_store_mutex: Mutex::new(()),
                approvals: Mutex::new(ApprovalState {
                    queue: VecDeque::new(),
                    open: HashMap::new(),
                    next_id: 1,
                }),
                approval_sink: Mutex::new(None),
                clients: Mutex::new(Vec::new()),
                stop_mutex: Mutex::new(()),
                on_task_progress: Mutex::new(None),
                models_dir_override: Mutex::new(None),
                cfg,
            }),
            accept_handle: Mutex::new(None),
        }
    }

    /// GUI mode: approval requests are forwarded to this channel as they are
    /// queued; the UI resolves them via [`Coordinator::resolve_approval`].
    /// (CLI mode instead runs the stdin prompt loop — see
    /// [`run_stdin_approval_loop`].)
    pub fn set_approval_sink(&self, tx: mpsc::Sender<PendingApproval>) {
        *self.sh.approval_sink.lock().unwrap() = Some(tx);
    }

    pub fn set_on_task_progress(&self, cb: Option<Box<dyn Fn(f32) + Send + Sync>>) {
        *self.sh.on_task_progress.lock().unwrap() = cb;
    }

    /// coordinator_admin.cpp::set_models_directory.
    pub fn set_models_directory(&self, dir: PathBuf) {
        *self.sh.models_dir_override.lock().unwrap() = Some(dir);
    }


    pub fn is_running(&self) -> bool {
        self.sh.is_running()
    }

    pub fn port(&self) -> u16 {
        self.sh.cfg.port
    }

    pub fn start(&mut self) -> Result<(), CoordinatorError> {
        if self.is_running() {
            return Ok(());
        }
        let listener = self
            .sh
            .cfg
            .listener_factory
            .listen(self.sh.cfg.port)
            .map_err(|e| CoordinatorError {
                code: 1,
                message: format!("Failed to start server: {e}"),
            })?;
        *self.sh.listener.lock().unwrap() = Some(Arc::from(listener));
        self.sh.running.store(true, Ordering::SeqCst);
        let sh = Arc::clone(&self.sh);
        let handle = std::thread::Builder::new()
            .name("upix-coord-accept".into())
            .spawn(move || accept_loop(sh))
            .map_err(|e| CoordinatorError {
                code: 1,
                message: format!("cannot start accept thread: {e}"),
            })?;
        *self.accept_handle.lock().unwrap() = Some(handle);
        log::info!("Coordinator started on port {}", self.sh.cfg.port);
        Ok(())
    }

    /// Teardown order mirrors coordinator.cpp::stop(): reject pending
    /// approvals → wake the accept loop → join it → drop the listener →
    /// shutdown every registered session socket → join client threads.
    pub fn stop(&mut self) {
        let _stop_lock = self.sh.stop_mutex.lock().unwrap();
        let listener = self.sh.listener.lock().unwrap().take();
        if !self.is_running() && listener.is_none() {
            return;
        }
        log::info!("Stopping coordinator...");
        {
            // Closing registration before waking waiters prevents orphan
            // promises; the registry includes queued AND popped requests.
            let mut ap = self.sh.approvals.lock().unwrap();
            self.sh.running.store(false, Ordering::SeqCst);
            for v in ap.open.values() {
                v.fulfill(0);
            }
            ap.open.clear();
            ap.queue.clear();
        }

        // 1+2: request the accept loop's stop (running=false re-checked at the
        // top of every iteration), wake + drop the listener, join.
        if let Some(l) = &listener {
            l.shutdown();
        }
        let handle = self.accept_handle.lock().unwrap().take();
        if let Some(h) = handle {
            let _ = h.join();
        }

        // 3: wake the handlers parked in blocking socket calls. A handler
        // that has not registered yet sees running=false on its
        // post-registration check and drops the connection itself.
        let clients: Vec<Arc<ClientEntry>> = {
            let mut c = self.sh.clients.lock().unwrap();
            std::mem::take(&mut *c)
        };
        for e in &clients {
            if let Some(sock) = e.wake.lock().unwrap().as_ref() {
                sock.shutdown();
            }
        }
        for e in clients {
            let h = e.handle.lock().unwrap().take();
            if let Some(h) = h {
                let _ = h.join();
            }
        }
        log::info!("Coordinator stopped");
    }

    /// Pop a pending first-time pairing request if one is waiting (FIFO).
    /// Disconnected or already-resolved requests never re-enter the UI.
    pub fn pop_pending_approval(&self) -> Option<PendingApproval> {
        let mut ap = self.sh.approvals.lock().unwrap();
        while let Some(req) = ap.queue.pop_front() {
            if ap.open.contains_key(&req.id) {
                return Some(req);
            }
        }
        None
    }

    /// Resolve a popped request. Stale/disconnected/stop()-canceled ids are
    /// no-ops; a resolve racing stop() answers 0 (stop already rejected).
    pub fn resolve_approval(&self, id: u64, approved: bool) {
        let mut ap = self.sh.approvals.lock().unwrap();
        let Some(v) = ap.open.remove(&id) else {
            return;
        };
        let value = u8::from(approved && self.sh.is_running());
        v.fulfill(value);
    }
}

impl Drop for Coordinator {
    fn drop(&mut self) {
        self.stop();
    }
}

/// CLI-mode approval loop: polls the queue, prints the request, blocks stdin
/// on [y/n] (the C++ behavior). GUI harnesses use `set_approval_sink` +
/// `resolve_approval` instead.
pub fn run_stdin_approval_loop(coord: &Coordinator, running: &AtomicBool) {
    while running.load(Ordering::SeqCst) && coord.is_running() {
        match coord.pop_pending_approval() {
            Some(req) => {
                println!(
                    "Pairing request from {} (fingerprint {})",
                    req.device_name, req.fingerprint
                );
                print!("Approve? [y/N]: ");
                let _ = std::io::stdout().flush();
                let mut line = String::new();
                let ok = std::io::stdin().read_line(&mut line).is_ok();
                let approved = ok && matches!(line.trim(), "y" | "Y" | "yes" | "Yes");
                coord.resolve_approval(req.id, approved);
            }
            None => std::thread::sleep(Duration::from_millis(200)),
        }
    }
}

/// GUI-mode forwarder: pushes popped requests into a channel.
pub fn spawn_approval_forwarder(coord: Arc<Coordinator>, tx: mpsc::Sender<PendingApproval>) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || loop {
        if !coord.is_running() {
            return;
        }
        match coord.pop_pending_approval() {
            Some(req) => {
                if tx.send(req).is_err() {
                    return;
                }
            }
            None => std::thread::sleep(Duration::from_millis(100)),
        }
    })
}

// ── accept loop ─────────────────────────────────────────────────────────────

fn accept_loop(sh: Arc<Shared>) {
    loop {
        if !sh.is_running() {
            return;
        }
        reap_finished_clients(&sh);
        let listener = sh.listener.lock().unwrap().clone();
        let Some(listener) = listener else {
            return;
        };
        match listener.accept() {
            Ok(Some(sock)) => {
                if sh.clients.lock().unwrap().len() >= MAX_CONCURRENT_CLIENTS {
                    log::warn!(
                        "Refusing connection: {} clients already connected",
                        MAX_CONCURRENT_CLIENTS
                    );
                    continue; // the refused socket closes with the box
                }
                let entry = Arc::new(ClientEntry {
                    done: AtomicBool::new(false),
                    wake: Mutex::new(None),
                    handle: Mutex::new(None),
                });
                sh.clients.lock().unwrap().push(Arc::clone(&entry));
                let sh2 = Arc::clone(&sh);
                let e2 = Arc::clone(&entry);
                let spawned = std::thread::Builder::new()
                    .name("upix-coord-client".into())
                    .spawn(move || {
                        // A handler must never take the process down: hostile
                        // input can panic before authentication, so the whole
                        // session is wrapped (C++ caught exceptions here).
                        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                            handle_client(sh2, sock, &e2)
                        }));
                        e2.done.store(true, Ordering::SeqCst);
                    });
                match spawned {
                    Ok(h) => *entry.handle.lock().unwrap() = Some(h),
                    Err(e) => {
                        log::error!("cannot start client thread: {e}");
                        sh.clients.lock().unwrap().pop();
                    }
                }
            }
            Ok(None) => continue, // poll tick: re-check the stop flag
            Err(e) => {
                if sh.is_running() {
                    log::error!("Accept failed: {e}");
                }
                continue;
            }
        }
    }
}

/// Join and drop connection threads whose handler already returned — keeps
/// the vector bounded by live connections instead of lifetime connection
/// count (a legacy phone that reconnects per image would otherwise leave one
/// unjoined thread behind per connection until stop()).
fn reap_finished_clients(sh: &Shared) {
    let mut clients = sh.clients.lock().unwrap();
    clients.retain(|e| {
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

// ── session wake registration ───────────────────────────────────────────────

/// Registers a connection's descriptor for the whole handler scope and clears
/// it on every exit path BEFORE the socket is dropped, so stop() can never
/// shutdown() a closed (and possibly recycled) descriptor.
struct SessionWakeRegistration<'a> {
    entry: &'a ClientEntry,
    socket: Arc<dyn Transport>,
}

impl<'a> SessionWakeRegistration<'a> {
    fn new(entry: &'a ClientEntry, socket: Arc<dyn Transport>) -> Self {
        *entry.wake.lock().unwrap() = Some(Arc::clone(&socket));
        Self { entry, socket }
    }
}

impl Drop for SessionWakeRegistration<'_> {
    fn drop(&mut self) {
        *self.entry.wake.lock().unwrap() = None;
        // Keep self.socket alive until after the clear (destructor order).
        let _ = &self.socket;
    }
}

// ── client session ──────────────────────────────────────────────────────────

fn handle_client(sh: Arc<Shared>, socket: Box<dyn Transport>, entry: &ClientEntry) {
    let socket: Arc<dyn Transport> = Arc::from(socket);
    let _wake = SessionWakeRegistration::new(entry, Arc::clone(&socket));

    // Shutdown raced with an accepted connection: drop it immediately.
    // Checked AFTER registering — stop() requests the stop flag before it
    // wakes registered sessions.
    if !sh.is_running() {
        return;
    }

    // Bound the whole pre-session phase (first header + KEY_HELLO handshake +
    // CONNECT_REQUEST). Accepted sockets carry no timeout of their own.
    socket.set_recv_timeout_ms(HANDSHAKE_TIMEOUT_MS);

    let mut hdr = [0u8; HEADER_SIZE];
    if socket.receive_exact(&mut hdr).is_err() {
        return;
    }
    let Some(first) = (sh.cfg.framer).parse_header(&hdr) else {
        return;
    };
    let device_id = sh.cfg.crypto.new_device_id();

    if first.msg_type != MessageType::KeyHello {
        return; // anything else is a protocol error → drop
    }
    if first.payload_length != wireio::KEY_HELLO_SIZE as u32 {
        return;
    }
    let mut hello_payload = vec![0u8; wireio::KEY_HELLO_SIZE];
    if socket.receive_exact(&mut hello_payload).is_err() {
        return;
    }
    let Some(c_hello) = wireio::dec_key_hello(&hello_payload) else {
        return;
    };

    let mut conn = Conn {
        socket,
        framer: Arc::clone(&sh.cfg.framer),
        device_id,
        session: None,
        peer_id_pub: [0u8; 32],
        peer_trusted: false,
    };

    if !run_handshake(sh.as_ref(), &mut conn, &c_hello) {
        return;
    }

    // Every peer must open the pairing gate before sending business frames.
    conn.socket.set_recv_timeout_ms(CONNECT_TIMEOUT_MS);
    let Some((frame_hdr, frame_payload)) = conn.recv_encrypted(&sh) else {
        log::warn!(
            "Peer {} did not send CONNECT_REQUEST within timeout",
            conn.device_id
        );
        return;
    };
    if frame_hdr.msg_type != MessageType::ClientConnectRequest || !sh.is_running() {
        log::warn!("Peer skipped CONNECT_REQUEST or server stopped");
        return;
    }
    let Some(creq) = wireio::dec_connect_request(&frame_payload) else {
        log::error!("CONNECT_REQUEST payload too short");
        return;
    };
    let req_device_name = wireio::rd_bounded_str(&frame_payload, 0, 64);
    let _ = creq; // fingerprint field ignored: the PC derives it from the pubkey
    let req_fingerprint = sh.cfg.crypto.fingerprint(&conn.peer_id_pub);

    // Auto-approve already-trusted peers; first-time peers block on an
    // interactive approval served by the PC main loop (or the channel sink /
    // auto_approve flag).
    let approved = if conn.peer_trusted {
        log::info!(
            "Auto-approved trusted peer {req_device_name} ({req_fingerprint})"
        );
        true
    } else if sh.cfg.auto_approve {
        log::info!(
            "Auto-approve flag set: approving first-time peer {req_device_name} ({req_fingerprint})"
        );
        true
    } else {
        match queue_and_wait_approval(sh.as_ref(), &conn, &req_device_name, &req_fingerprint) {
            ApprovalWait::Approved => true,
            ApprovalWait::Denied | ApprovalWait::Aborted => false,
        }
    };

    let cresp = ClientConnectResponse {
        approved: u8::from(approved),
        trusted: u8::from(conn.peer_trusted),
        // Cached dlopen probes: the phone gates AVIF input/output on these
        // instead of discovering it through task failures.
        avif_decode: u8::from(sh.cfg.backend.avif_available().0),
        avif_encode: u8::from(sh.cfg.backend.avif_available().1),
        // Submit-ahead depth this session tolerates: the in-task frame loop
        // keeps up to pipeline_depth+1 CLIENT_SUBMITs alive at once. The
        // multi-worker pump actually runs depth+1 concurrent tasks.
        pipeline_depth: PIPELINE_DEPTH,
    };
    if !send_sealed(&conn, MessageType::ClientConnectResponse, &wireio::enc_connect_response(&cresp)) {
        log::error!("Failed to send CONNECT_RESPONSE");
        return;
    }
    if !approved {
        log::warn!("Pairing denied for {req_device_name}");
        return;
    }
    // Persist only an affirmative, delivered approval. Serialize with stop()
    // so shutdown cannot turn a canceled prompt into trust.
    if !conn.peer_trusted {
        {
            let ap = sh.approvals.lock().unwrap();
            if !sh.is_running() {
                return;
            }
            let _trust = sh.trust_store_mutex.lock().unwrap();
            if let Some(trust) = &sh.cfg.trust {
                if trust.approve_and_save(&conn.peer_id_pub) {
                    log::info!("Peer added to TrustStore: {req_fingerprint}");
                }
            }
            drop(ap);
        }
        conn.peer_trusted = true;
    }

    // Now read the first session frame: the phone sends MSG_CLIENT_SUBMIT
    // with the image to upscale (or, historically, a model-library op).
    conn.socket.set_recv_timeout_ms(ROLE_FRAME_TIMEOUT_MS);
    let Some((frame_hdr, frame_payload)) = conn.recv_encrypted(&sh) else {
        log::warn!(
            "Peer {} closed after CONNECT_RESPONSE without a CLIENT_SUBMIT",
            conn.device_id
        );
        return;
    };
    session_frame_loop(sh, &conn, frame_hdr, frame_payload);
}

pub enum ApprovalWait {
    Approved,
    Denied,
    Aborted,
}

fn queue_and_wait_approval(
    sh: &Shared,
    conn: &Conn,
    device_name: &str,
    fingerprint: &str,
) -> ApprovalWait {
    let (pa, verdict) = {
        let mut ap = sh.approvals.lock().unwrap();
        if !sh.is_running() {
            return ApprovalWait::Aborted;
        }
        let id = ap.next_id;
        ap.next_id += 1;
        let pa = PendingApproval {
            id,
            device_name: device_name.to_string(),
            fingerprint: fingerprint.to_string(),
            identity_pub: conn.peer_id_pub,
            verdict: Arc::new(ApprovalVerdict {
                cell: Mutex::new(None),
                cv: Condvar::new(),
            }),
        };
        ap.open.insert(id, Arc::clone(&pa.verdict));
        ap.queue.push_back(pa.clone());
        // Forward to the GUI sink if one is attached.
        if let Some(tx) = sh.approval_sink.lock().unwrap().as_ref() {
            let _ = tx.send(pa.clone());
        }
        (pa.clone(), Arc::clone(&pa.verdict))
    };
    log::info!(
        "Pairing request queued from {} ({fingerprint}) — waiting for PC approval",
        pa.device_name
    );

    // While the UI decides, detect EOF or premature business traffic without
    // consuming a partial frame or blocking stop().
    conn.socket.set_recv_timeout_ms(100);
    loop {
        if verdict.is_resolved() {
            break;
        }
        if !sh.is_running() {
            resolve_and_drop(sh, &pa, false);
            return ApprovalWait::Aborted;
        }
        let mut byte = [0u8; 1];
        match conn.socket.receive(&mut byte) {
            Ok(0) | Err(RecvErr::Dead(_)) => {
                resolve_and_drop(sh, &pa, false);
                return ApprovalWait::Aborted;
            }
            Ok(_) => {
                // Premature business traffic while unapproved: treat like EOF.
                resolve_and_drop(sh, &pa, false);
                return ApprovalWait::Aborted;
            }
            Err(RecvErr::WouldBlock) => {}
        }
    }
    let approved = verdict.value() != 0;
    resolve_and_drop(sh, &pa, approved); // idempotent for already-resolved
    if approved {
        conn.socket.set_recv_timeout_ms(CONNECT_TIMEOUT_MS);
        ApprovalWait::Approved
    } else {
        ApprovalWait::Denied
    }
}

/// Remove the request from the resolution registry (a resolved request must
/// not re-enter the UI or accept a second verdict).
fn resolve_and_drop(sh: &Shared, pa: &PendingApproval, value: bool) {
    let mut ap = sh.approvals.lock().unwrap();
    if ap.open.remove(&pa.id).is_some() && !pa.verdict.is_resolved() {
        pa.verdict.fulfill(u8::from(value));
    }
}

// ── handshake ───────────────────────────────────────────────────────────────

fn run_handshake(sh: &Shared, conn: &mut Conn, c_hello: &upix_net::protocol::KeyHelloMessage) -> bool {
    // (1) Server ephemeral keypair. x25519_keygen returns (public, private)
    // — same order as the C++ X25519Keypair struct (crypto_sodium.hpp:15).
    let (s_eph_pub, s_eph_priv) = sh.cfg.crypto.x25519_keygen();
    let s_id_pub = sh.cfg.signer.public_key();

    // (2) Transcript c_eph||c_id||s_eph||s_id (128 bytes).
    let mut transcript = [0u8; 128];
    transcript[0..32].copy_from_slice(&c_hello.eph_pub);
    transcript[32..64].copy_from_slice(&c_hello.id_pub);
    transcript[64..96].copy_from_slice(&s_eph_pub);
    transcript[96..128].copy_from_slice(&s_id_pub);

    // (3) Sign transcript, send server KEY_HELLO (plaintext).
    let s_sig = sh.cfg.signer.sign(&transcript);
    let s_hello = wireio::enc_key_hello(&upix_net::protocol::KeyHelloMessage {
        eph_pub: s_eph_pub,
        id_pub: s_id_pub,
        sig: s_sig,
    });
    let wire = sh.cfg.framer.serialize_frame(MessageType::KeyHello, &s_hello);
    if conn.socket.send_all(&wire).is_err() {
        log::warn!("Handshake aborted: failed to send server KEY_HELLO");
        return false;
    }

    // (4) Derive session key + activate. From here on everything is encrypted.
    let session_key = sh.cfg.crypto.x25519_shared(&s_eph_priv, &c_hello.eph_pub);
    conn.session = Some(Arc::from(sh.cfg.crypto.new_session(&session_key)));

    // (5) Receive AUTH_PROOF (encrypted) + verify the client signature.
    let Some((proof_hdr, proof_payload)) = conn.recv_encrypted(sh) else {
        log::warn!("Handshake aborted: no AUTH_PROOF / MAC failed");
        return false;
    };
    if proof_hdr.msg_type != MessageType::AuthProof {
        log::warn!("Handshake aborted: malformed AUTH_PROOF");
        return false;
    }
    let Some(c_sig) = wireio::dec_auth_proof(&proof_payload) else {
        log::warn!("Handshake aborted: malformed AUTH_PROOF");
        return false;
    };
    if !sh
        .cfg
        .crypto
        .ed25519_verify(&c_hello.id_pub, &c_sig, &transcript)
    {
        log::warn!("Handshake aborted: client signature verification failed");
        // Still reply so the client doesn't block, but mark unauthenticated.
        let resp = AuthResponse {
            authenticated: 0,
            device_id: [0u8; 36],
            trusted: 0,
        };
        let _ = send_sealed(conn, MessageType::AuthResponse, &wireio::enc_auth_response(&resp));
        return false;
    }

    conn.peer_id_pub = c_hello.id_pub;
    // Authentication proves key ownership; only affirmative pairing approval
    // may add this identity to the TrustStore.
    conn.peer_trusted = sh
        .cfg
        .trust
        .as_ref()
        .map(|t| t.is_trusted(&conn.peer_id_pub))
        .unwrap_or(false);

    // (6) Send AUTH_RESPONSE (encrypted).
    let mut resp = AuthResponse {
        authenticated: 1,
        device_id: [0u8; 36],
        trusted: u8::from(conn.peer_trusted), // trust requires prior approval
    };
    let id_bytes = conn.device_id.as_bytes();
    let n = id_bytes.len().min(35);
    resp.device_id[..n].copy_from_slice(&id_bytes[..n]);
    if !send_sealed(conn, MessageType::AuthResponse, &wireio::enc_auth_response(&resp)) {
        log::warn!("Handshake aborted: failed to send AUTH_RESPONSE");
        return false;
    }
    log::info!(
        "Client handshake ok (device_id={}, trusted={})",
        conn.device_id,
        if conn.peer_trusted {
            "1".to_string()
        } else {
            "0 (approval required)".to_string()
        }
    );
    true
}


// ── session frame loop ──────────────────────────────────────────────────────

fn session_frame_loop(sh: Arc<Shared>, conn: &Conn, hdr: MessageHeader, payload: Vec<u8>) {
    let mut frame = (hdr, payload);
    loop {
        let (fhdr, fpayload) = frame;
        match fhdr.msg_type {
            MessageType::ClientSubmit => {
                let incoming = CapturedSubmit {
                    payload: fpayload,
                    staged_data: Vec::new(),
                    ahead: None,
                };
                if !pump_session_submits(Arc::clone(&sh), conn, incoming) {
                    return;
                }
                conn.socket.set_recv_timeout_ms(SESSION_FRAME_TIMEOUT_MS);
                let Some(next) = conn.recv_encrypted(sh.as_ref()) else {
                    return; // clean close, idle timeout, or transport failure
                };
                frame = next;
            }
            MessageType::TaskCancel => {
                // A cancel whose task already completed (the response beat the
                // cancel to the phone): no task running — ignore, session
                // stays usable.
                log::info!(
                    "Stray task cancel from peer {} (no task running); ignored",
                    conn.device_id
                );
                conn.socket.set_recv_timeout_ms(SESSION_FRAME_TIMEOUT_MS);
                let Some(next) = conn.recv_encrypted(sh.as_ref()) else {
                    return;
                };
                frame = next;
            }
            MessageType::ModelListRequest => {
                // These ops send a (possibly large) reply and then end the
                // session, so only the receive window is shortened (the C++
                // set_timeout also shrank SO_SNDTIMEO, aborting a transfer of
                // tens of MB — the port keeps send timeouts out of this path).
                conn.socket.set_recv_timeout_ms(120);
                handle_model_list(sh.as_ref(), conn);
                return;
            }
            MessageType::ModelUpload => {
                conn.socket.set_recv_timeout_ms(300);
                handle_model_upload(sh.as_ref(), conn, &fpayload);
                return;
            }
            MessageType::ModelDownloadRequest => {
                conn.socket.set_recv_timeout_ms(300);
                handle_model_download(sh.as_ref(), conn, &fpayload);
                return;
            }
            other => {
                log::warn!(
                    "Peer {} sent an unexpected session frame (type {}); dropping",
                    conn.device_id,
                    other as u16
                );
                return;
            }
        }
    }
}

// ── submit pump ─────────────────────────────────────────────────────────────

/// Declared chunked length for a captured submit (0 = legacy single-frame /
/// invalid declaration). DATA continuations are matched against this.
fn declared_chunk_len(cap: &CapturedSubmit) -> u32 {
    if cap.payload.len() != wireio::CLIENT_TASK_SUBMIT_SIZE {
        return 0;
    }
    let Some(p) = wireio::dec_task_submit(&cap.payload) else {
        return 0;
    };
    if p.input_data_length == 0 || p.input_data_length > MAX_NETWORK_PAYLOAD {
        return 0;
    }
    p.input_data_length
}

/// True when a captured submit has its whole input locally available and can
/// be handed to a worker without touching the socket again.
fn capture_input_complete(cap: &CapturedSubmit) -> bool {
    if cap.payload.len() == wireio::CLIENT_TASK_SUBMIT_SIZE {
        // Chunked framing: complete once staged bytes reach the declared len.
        let want = declared_chunk_len(cap);
        return want > 0 && cap.staged_data.len() >= want as usize;
    }
    if cap.payload.len() < wireio::CLIENT_TASK_SUBMIT_SIZE {
        return false; // malformed; the pump replies an error when served
    }
    // Legacy single-frame framing: struct + input inline.
    true
}

/// Copy the compressed input out of a completed capture. Never reads the
/// socket. `Ok(None)` with err set = structurally invalid submit (caller
/// replies a per-task error and keeps the session); `Err(())` = broken legacy
/// framing (caller drops the connection).
fn assemble_capture(
    cap: &CapturedSubmit,
    task_out: &mut ClientTaskSubmit,
    input_out: &mut Vec<u8>,
) -> Result<(), Option<String>> {
    let Some(p) = wireio::dec_task_submit(&cap.payload) else {
        return Err(Some("submit struct unparsable".to_string()));
    };
    *task_out = p;
    let data_len = task_out.input_data_length;

    if cap.payload.len() == wireio::CLIENT_TASK_SUBMIT_SIZE {
        // Chunked: staged_data must hold exactly the declared payload.
        if data_len == 0 || data_len > MAX_NETWORK_PAYLOAD {
            return Err(Some("invalid chunked submit size".to_string()));
        }
        // Preallocated by input_data_length — no growth doubling (peak
        // ≈ total + 2×8MB chunk buffers, never ~2× the total).
        input_out.clear();
        input_out.reserve_exact(data_len as usize);
        input_out.extend_from_slice(&cap.staged_data[..data_len as usize]);
        return Ok(());
    }

    // Legacy single-frame: struct + bytes tail. A structurally broken frame
    // here means the peer violated framing — caller drops the connection.
    if data_len > MAX_NETWORK_PAYLOAD
        || wireio::CLIENT_TASK_SUBMIT_SIZE + data_len as usize > cap.payload.len()
    {
        return Err(None);
    }
    *input_out = cap.payload[wireio::CLIENT_TASK_SUBMIT_SIZE..].to_vec();
    Ok(())
}

/// Prefetch trigger for the head entry: fires when its full input is present
/// on the queue — legacy inline payloads are complete on arrival; chunked
/// completes once staged bytes reach the declared length.
fn maybe_launch_head_prefetch(sh: &Shared, q: &mut VecDeque<CapturedSubmit>) {
    if q.front().map(|h| h.ahead.is_some()).unwrap_or(true)
        || q.front().map(|h| h.payload.len() < wireio::CLIENT_TASK_SUBMIT_SIZE).unwrap_or(true)
    {
        return;
    }
    let head_len = q.front().unwrap().payload.len();
    let (want, have) = if head_len == wireio::CLIENT_TASK_SUBMIT_SIZE {
        let h = q.front().unwrap();
        (declared_chunk_len(h) as usize, h.staged_data.len())
    } else {
        let Some(p) = wireio::dec_task_submit(&q.front().unwrap().payload) else {
            return;
        };
        (
            p.input_data_length as usize,
            head_len - wireio::CLIENT_TASK_SUBMIT_SIZE,
        )
    };
    if want == 0 || have < want || head_len <= wireio::CLIENT_TASK_SUBMIT_SIZE {
        return;
    }
    let task = match wireio::dec_task_submit(&q.front().unwrap().payload) {
        Some(t) => t,
        None => return,
    };
    let source: Vec<u8> = {
        let head = q.front().unwrap();
        if head.staged_data.is_empty() && head.payload.len() > wireio::CLIENT_TASK_SUBMIT_SIZE {
            head.payload[wireio::CLIENT_TASK_SUBMIT_SIZE..].to_vec()
        } else {
            head.staged_data.clone()
        }
    };
    let ahead = Arc::new(AheadDecode {
        inner: Mutex::new(AheadInner {
            ready: false,
            ok: false,
            error: String::new(),
            pixels: Vec::new(),
            width: 0,
            height: 0,
            channels: 0,
        }),
        cv: Condvar::new(),
    });
    q.front_mut().unwrap().ahead = Some(Arc::clone(&ahead));
    let backend = Arc::clone(&sh.cfg.backend);
    std::thread::Builder::new()
        .name("upix-ahead-decode".into())
        .spawn(move || {
            let mut img = ImgScratch::default();
            let err = prepare_task_input(backend.as_ref(), &task, &source, &mut img);
            let mut g = ahead.inner.lock().unwrap();
            match err {
                Some(e) => {
                    g.ok = false;
                    g.error = e;
                }
                None => {
                    g.ok = true;
                    g.pixels = std::mem::take(&mut img.data);
                    g.width = img.width;
                    g.height = img.height;
                    g.channels = img.channels;
                }
            }
            g.ready = true;
            ahead.cv.notify_all();
        })
        .ok();
    log::info!("Submit-ahead prefetch decode started for {want} bytes");
}

#[derive(Default)]
struct ImgScratch {
    data: Vec<u8>,
    width: i32,
    height: i32,
    channels: i32,
}

/// EXIF sniff → decode → channel normalize → orientation bake. Byte-identical
/// input state for the worker's inline path and the prefetch thread. Returns
/// Some(error text) on failure (identical wording to the C++ replies).
fn prepare_task_input(
    backend: &dyn Backend,
    task: &ClientTaskSubmit,
    compressed: &[u8],
    out: &mut ImgScratch,
) -> Option<String> {
    // Sniff EXIF orientation from the encoded bytes before they are freed:
    // portrait JPEGs (orientation 5-8) decode unrotated, and the bake-in
    // below keeps the remote path upright like the phone's local path.
    let exif_orientation = backend.exif_orientation(compressed);
    let input_format = wireio::rd_bounded_str(&task.input_format, 0, 16);
    let mut img = match backend.decode_image(&input_format, compressed) {
        Ok(i) => i,
        Err(e) => return Some(format!("input decode failed: {e}")),
    };
    // Normalize gray and other model inputs before inference. RGBA stays
    // intact for RGB models: TileProcessor splits off alpha and merges it
    // after inference instead of compositing it away here. The model's
    // input_channels come from the model library (0 = no expectation).
    let expected = expected_channels_of(backend, task);
    if !backend.normalize_channels(&mut img, expected) {
        return Some("channel conversion failed".to_string());
    }
    if exif_orientation != 1 && backend.apply_orientation(&mut img, exif_orientation) {
        log::info!("applied EXIF orientation {exif_orientation}");
    }
    out.width = img.width;
    out.height = img.height;
    out.channels = img.channels;
    out.data = std::mem::take(&mut img.data);
    None
}

fn expected_channels_of(backend: &dyn Backend, task: &ClientTaskSubmit) -> i32 {
    let name = wireio::rd_bounded_str(&task.model_name, 0, 64);
    backend.get_model(&name).map(|m| m.input_channels).unwrap_or(0)
}

/// Session task pump (see coordinator.hpp for the full contract). Returns
/// false only when the connection must be dropped.
fn pump_session_submits(sh: Arc<Shared>, conn: &Conn, first: CapturedSubmit) -> bool {
    let mut captures: VecDeque<CapturedSubmit> = VecDeque::new();
    captures.push_back(first);
    let mut inflight: Vec<Inflight> = Vec::new();
    let transport_ok = true;

    // Every exit path joins workers BEFORE returning — they reference conn.
    // After flipping cancel, gate-parked workers must be notified or a run
    // waiting for its send turn would hang forever on join (lost wakeup).
    fn cancel_all_and_join(inflight: &mut Vec<Inflight>) {
        for t in inflight.iter() {
            t.run.cancel.store(true, Ordering::SeqCst);
            t.run.open_send_gate(); // notify_all under the gate lock
        }
        for t in inflight.drain(..) {
            if let Some(h) = t.handle {
                let _ = h.join();
            }
        }
    }

    fn grant_front_send(inflight: &[Inflight]) {
        if let Some(front) = inflight.first() {
            front.run.open_send_gate();
        }
    }

    loop {
        // Shutdown must not orphan workers spinning on a dead socket: bail
        // out as soon as a poll-tick wake reaches here.
        if !sh.is_running() {
            cancel_all_and_join(&mut inflight);
            return false;
        }

        // A head capture shorter than the submit struct can never become
        // servable — treat it as the framing violation it is.
        if let Some(front) = captures.front() {
            if front.payload.len() < wireio::CLIENT_TASK_SUBMIT_SIZE {
                log::warn!(
                    "Peer {} sent a CLIENT_SUBMIT shorter than the submit struct ({} bytes); dropping",
                    conn.device_id,
                    front.payload.len()
                );
                cancel_all_and_join(&mut inflight);
                return false;
            }
        }

        // Spawn new workers from complete head captures while slots last.
        while !captures.is_empty()
            && inflight.len() < MAX_INFLIGHT_TASKS
            && capture_input_complete(captures.front().unwrap())
        {
            let mut cap = captures.pop_front().unwrap();
            let mut task = placeholder_task();
            let mut input = Vec::new();
            match assemble_capture(&cap, &mut task, &mut input) {
                Err(Some(err)) => {
                    reply_task_error(conn, &err);
                    continue; // malformed sizes: answered, keep serving
                }
                Err(None) => {
                    cancel_all_and_join(&mut inflight);
                    return false; // broken legacy framing: drop the connection
                }
                Ok(()) => {}
            }
            log::info!("Serving staged submit: {} bytes", input.len());
            let run = Arc::new(ClientTaskRun::new());
            let predecoded = cap.ahead.take();
            let socket = Arc::clone(&conn.socket);
            let session = conn.session.as_ref().map(Arc::clone);
            let sh2 = Arc::clone(&sh);
            let run2 = Arc::clone(&run);
            let input2 = std::mem::take(&mut input);
            let handle = std::thread::Builder::new()
                .name("upix-task-worker".into())
                .spawn(move || {
                    run_client_task(sh2, socket, session, task, input2, run2, predecoded);
                });
            let Ok(handle) = handle else {
                cancel_all_and_join(&mut inflight);
                return false;
            };
            inflight.push(Inflight { run, handle: Some(handle) });
        }

        // Reap finished tasks and exit once nothing is left to serve.
        inflight.retain_mut(|t| {
            if t.run.done.load(Ordering::SeqCst) {
                let handle = std::mem::replace(&mut t.handle, placeholder_join());
                if let Some(h) = handle {
                    let _ = h.join();
                }
                false
            } else {
                true
            }
        });
        grant_front_send(&inflight);
        if inflight.is_empty() && captures.is_empty() {
            // Batch lull: hand control back to the session loop's blocking
            // read; a fresh submit re-enters the pump within microseconds.
            return transport_ok;
        }

        // Progress frames are sent for the OLDEST unfinished run only —
        // TaskProgress carries no task id, and the phone keys percent/ETA on
        // the task it is currently awaiting (service order = arrival order).
        if transport_ok && !inflight.is_empty() {
            let front = inflight.first().unwrap();
            let pending_pct = front.run.progress_pct.swap(-1, Ordering::Relaxed);
            if pending_pct >= 0 {
                let msg = TaskProgress {
                    stage: 0,
                    percent: pending_pct as u32,
                    eta_seconds: front.run.progress_eta.load(Ordering::Relaxed),
                };
                if !send_sealed(conn, MessageType::TaskProgress, &wireio::enc_task_progress(&msg)) {
                    log::warn!("Progress send failed (peer gone); canceling");
                    for t in &inflight {
                        t.run.connection_lost.store(true, Ordering::SeqCst);
                    }
                    cancel_all_and_join(&mut inflight);
                    return false;
                }
            }
        }

        let frame = recv_encrypted_poll(conn, TASK_FRAME_POLL_MS);
        let frame = match frame {
            FramePoll::Timeout => continue,
            FramePoll::Error => {
                log::warn!("Connection lost while task(s) were running; canceling");
                for t in &inflight {
                    t.run.connection_lost.store(true, Ordering::SeqCst);
                }
                cancel_all_and_join(&mut inflight);
                return false;
            }
            FramePoll::Frame(h, p) => (h, p),
        };

        if !sh.is_running() {
            cancel_all_and_join(&mut inflight);
            return false;
        }

        // ── Reassembly gate for an in-flight chunked upload ────────────────
        // If the HEAD capture is a chunked submit whose bytes are still
        // trickling in, this frame must be its next MSG_CLIENT_DATA
        // continuation. Any other frame means staging will never finish:
        // answer a PER-TASK error and abandon only that capture so the peer
        // can fall back to a legacy single-frame submit on this live session.
        // Never drop the connection here.
        if let Some(head) = captures.front_mut() {
            let want = declared_chunk_len(head) as usize;
            let have = head.staged_data.len();
            if want > 0 && have < want {
                if frame.0.msg_type == MessageType::ClientData
                    && frame.1.len() <= want - have
                {
                    head.staged_data.extend_from_slice(&frame.1);
                    maybe_launch_head_prefetch(sh.as_ref(), &mut captures);
                    continue;
                }
                if frame.0.msg_type == MessageType::ClientData {
                    reply_task_error(conn, "chunked submit overflow");
                } else {
                    reply_task_error(
                        conn,
                        "chunked submit interrupted: expected CLIENT_DATA",
                    );
                }
                captures.pop_front();
                continue;
            }
        }

        match frame.0.msg_type {
            MessageType::TaskCancel => {
                log::info!(
                    "Task cancel requested by peer {} (canceling all in-flight tasks)",
                    conn.device_id
                );
                for t in &inflight {
                    t.run.cancel.store(true, Ordering::SeqCst);
                    t.run.open_send_gate();
                }
                continue;
            }
            MessageType::ClientSubmit => {
                if captures.len() >= MAX_AHEAD_CAPTURES {
                    log::warn!(
                        "Peer {} exceeded the submit-ahead capture bound; dropping",
                        conn.device_id
                    );
                    cancel_all_and_join(&mut inflight);
                    return false;
                }
                let cap = CapturedSubmit {
                    payload: frame.1,
                    staged_data: Vec::new(),
                    ahead: None,
                };
                captures.push_back(cap);
                // An inline submit is complete the moment it lands — its
                // whole input can start decoding while the GPU keeps working.
                maybe_launch_head_prefetch(sh.as_ref(), &mut captures);
                continue;
            }
            MessageType::ClientData => {
                // Tail entry fully staged while the GPU may still be busy:
                // start the prefetch decode now. Only completions get one.
                if let Some(tail) = captures.back_mut() {
                    let want = declared_chunk_len(tail) as usize;
                    if want > 0 && frame.1.len() <= want - tail.staged_data.len() {
                        tail.staged_data.extend_from_slice(&frame.1);
                        maybe_launch_head_prefetch(sh.as_ref(), &mut captures);
                        continue;
                    }
                }
                // DATA with nothing staged or past the declared length:
                // protocol violation → drop.
                log::warn!(
                    "Peer {} sent an unexpected frame; dropping (type={} len={} queued={} inflight={})",
                    conn.device_id,
                    MessageType::ClientData as u16,
                    frame.1.len(),
                    captures.len(),
                    inflight.len()
                );
                cancel_all_and_join(&mut inflight);
                return false;
            }
            other => {
                log::warn!(
                    "Peer {} sent an unexpected frame; dropping (type={} len={} queued={} inflight={})",
                    conn.device_id,
                    other as u16,
                    frame.1.len(),
                    captures.len(),
                    inflight.len()
                );
                cancel_all_and_join(&mut inflight);
                return false;
            }
        }
    }
}

fn placeholder_task() -> ClientTaskSubmit {
    ClientTaskSubmit {
        model_name: [0u8; 64],
        input_format: [0u8; 16],
        output_format: [0u8; 16],
        tile_size: 0,
        tile_overlap: 0,
        output_quality: 0,
        input_data_length: 0,
        encoder: upix_net::protocol::EncoderOptionsWire {
            png_level: -1,
            jpeg_subsampling: 420,
            webp_method: -1,
            webp_lossless: 0,
            reserved: [0; 3],
            avif_speed: 6,
            avif_subsampling: 420,
            avif_threads: 0,
        },
    }
}

fn reply_task_error(conn: &Conn, message: &str) {
    let mut response = ClientTaskResponse {
        success: 0,
        error_message: [0u8; 128],
        result_data_length: 0,
    };
    let msg = message.as_bytes();
    let n = msg.len().min(127);
    response.error_message[..n].copy_from_slice(&msg[..n]);
    log::error!("Client submit rejected: {message}");
    let _ = send_sealed(conn, MessageType::ClientResponse, &wireio::enc_task_response(&response));
}

// ── per-task worker ─────────────────────────────────────────────────────────

/// Worker body of one CLIENT_SUBMIT: decode (or await the submit-ahead
/// prefetch) → process_locally (cancel-aware) → terminal response. All
/// mid-task sends happen here so they never race the handler's reads.
#[allow(clippy::too_many_arguments)]
fn run_client_task(
    sh: Arc<Shared>,
    socket: Arc<dyn Transport>,
    session: Option<Arc<dyn SessionCodec>>,
    task: ClientTaskSubmit,
    compressed_input: Vec<u8>,
    run: Arc<ClientTaskRun>,
    predecoded: Option<Arc<AheadDecode>>,
) {
    // done is set on every exit path (the C++ DoneGuard).
    struct DoneGuard(Arc<ClientTaskRun>);
    impl Drop for DoneGuard {
        fn drop(&mut self) {
            self.0.done.store(true, Ordering::SeqCst);
        }
    }
    let _done_guard = DoneGuard(Arc::clone(&run));

    let conn_for_sends = SendCtx {
        socket,
        session,
    };

    // Serialize this run's socket writes behind any still-unfinished earlier
    // task of the same session (the phone reads terminal responses FIFO in
    // submit order). The gate always opens once cancel lands.
    let ordered_send = |ty: MessageType, payload: &[u8]| -> bool {
        run.await_send_turn();
        match &conn_for_sends.session {
            Some(s) => match s.seal(ty, payload) {
                Some(wire) => conn_for_sends.socket.send_all(&wire).is_ok(),
                None => false,
            },
            None => false,
        }
    };

    // Per-task error reply that keeps the session alive for the next submit.
    let reply_error = |message: &str| {
        let mut response = ClientTaskResponse {
            success: 0,
            error_message: [0u8; 128],
            result_data_length: 0,
        };
        let msg = message.as_bytes();
        let n = msg.len().min(127);
        response.error_message[..n].copy_from_slice(&msg[..n]);
        let _ = ordered_send(MessageType::ClientResponse, &wireio::enc_task_response(&response));
        log::error!("Client task failed: {message}");
    };

    // Wire char[] fields are not guaranteed NUL-terminated (bounded_string).
    let model_name = wireio::rd_bounded_str(&task.model_name, 0, 64);
    let mut output_format = wireio::rd_bounded_str(&task.output_format, 0, 16);
    if output_format.is_empty() {
        output_format = "jpg".to_string();
    }
    let output_quality: i32 = if task.output_quality == 0 {
        90
    } else {
        task.output_quality as i32
    };
    let encoder_opts: EncoderOptions = task.encoder.into();

    // Cancel raced the upload/parse: don't even decode.
    if run.canceled() {
        reply_error("canceled");
        return;
    }

    // Input prep: either await the submit-ahead prefetch or do it inline —
    // prepare_task_input is the single shared implementation, so both paths
    // build byte-identical inputs and reply identical errors.
    let mut scratch = ImgScratch::default();
    let mut compressed = compressed_input;
    match &predecoded {
        Some(ahead) => {
            let mut g = ahead.inner.lock().unwrap();
            while !g.ready {
                g = ahead.cv.wait(g).unwrap();
            }
            if !g.ok {
                let err = std::mem::take(&mut g.error);
                reply_error(&err);
                return;
            }
            scratch.width = g.width;
            scratch.height = g.height;
            scratch.channels = g.channels;
            scratch.data = std::mem::take(&mut g.pixels);
        }
        None => {
            if let Some(err) =
                prepare_task_input(sh.cfg.backend.as_ref(), &task, &compressed, &mut scratch)
            {
                compressed.clear();
                compressed.shrink_to_fit();
                reply_error(&err);
                return;
            }
            compressed.clear();
            compressed.shrink_to_fit();
        }
    }

    log::info!(
        "Received authenticated client task: {}x{} model={}",
        scratch.width,
        scratch.height,
        model_name
    );

    // Stream progress to the phone during the upscale (NAT keepalive + real
    // progress), throttled to whole-percent changes. The callback runs on
    // this worker thread, so last_sent_pct needs no locking. Publish-only:
    // the pump thread transmits progress frames from its poll loop.
    let task_start = Instant::now();
    // Throttled to whole-percent changes; the callback runs only on this
    // worker thread (process_locally invokes it synchronously), so the
    // last-sent cache needs no locking beyond the Mutex for Send+Sync.
    let progress_sink: Arc<dyn Fn(f32) + Send + Sync> = {
        let last = std::sync::Mutex::new(-1i32);
        let run3 = Arc::clone(&run);
        let start = task_start;
        Arc::new(move |frac: f32| {
            let pct = (frac.clamp(0.0, 1.0) * 100.0) as i32;
            let mut l = last.lock().unwrap();
            if pct == *l {
                return;
            }
            *l = pct;
            run3.progress_pct.store(pct, Ordering::Relaxed);
            if pct > 0 && pct < 100 {
                let elapsed = start.elapsed().as_secs_f64();
                run3.progress_eta.store(
                    ((elapsed / pct as f64) * ((100 - pct) as f64)) as u32,
                    Ordering::Relaxed,
                );
            }
        })
    };

    let cfg = TaskParams {
        tile_size: task.tile_size as i32,
        tile_overlap: task.tile_overlap as i32,
        min_tile_size: 64,
        max_tile_size: 512,
    };

    let process_result = process_locally(
        &sh,
        &scratch.data,
        scratch.width,
        scratch.height,
        scratch.channels,
        &model_name,
        &cfg,
        &output_format,
        output_quality,
        &encoder_opts,
        Some(progress_sink.as_ref()),
        &run.cancel,
    );

    // Whatever the engine reported, a cancel-flagged task answers "canceled"
    // — that is the string the phone keys its cancel path on.
    let process_result = match process_result {
        Err(_) if run.canceled() => {
            log::info!("Task aborted: inference stopped after cancel");
            Err(CoordinatorError { code: 7, message: "canceled".into() })
        }
        other => other,
    };

    // Label the encode phase so the phone's UI can show "encoding...".
    if process_result.is_ok() {
        let enc = TaskProgress {
            stage: 1,
            percent: 0,
            eta_seconds: 0,
        };
        let _ = ordered_send(MessageType::TaskProgress, &wireio::enc_task_progress(&enc));
    }

    let mut response = ClientTaskResponse {
        success: 0,
        error_message: [0u8; 128],
        result_data_length: 0,
    };
    if let Ok(bytes) = &process_result {
        response.success = 1;
        response.result_data_length = bytes.len() as u32;
    } else if let Err(e) = &process_result {
        let msg = e.message.as_bytes();
        let n = msg.len().min(127);
        response.error_message[..n].copy_from_slice(&msg[..n]);
    }

    // Response header + result data in one sealed frame; results above the
    // single-frame cap stream as MSG_CLIENT_DATA continuations. Leave room
    // for the seal overhead or a result near the cap would be sent as a
    // first frame the phone rejects — exactly the case the chunked path
    // exists for.
    let no_result: Vec<u8> = Vec::new();
    let result_bytes = match &process_result {
        Ok(b) => b,
        Err(_) => &no_result,
    };
    let leading = result_bytes.len().min(max_single_frame_plaintext());
    let mut resp_payload = Vec::with_capacity(wireio::CLIENT_TASK_RESPONSE_SIZE + leading);
    resp_payload.extend_from_slice(&wireio::enc_task_response(&response));
    resp_payload.extend_from_slice(&result_bytes[..leading]);
    let mut sent_ok = ordered_send(MessageType::ClientResponse, &resp_payload);
    let mut offset = leading;
    while sent_ok && offset < result_bytes.len() {
        let end = (offset + CLIENT_DATA_CHUNK_BYTES as usize).min(result_bytes.len());
        sent_ok = ordered_send(MessageType::ClientData, &result_bytes[offset..end]);
        offset = end;
    }
    if !sent_ok {
        log::error!("Failed to send authenticated client response");
        return;
    }
    match &process_result {
        Ok(b) => log::info!("Task completed: sent {} bytes", b.len()),
        Err(e) => log::error!("Authenticated client task failed: {}", e.message),
    }
}

struct SendCtx {
    socket: Arc<dyn Transport>,
    session: Option<Arc<dyn SessionCodec>>,
}

#[allow(clippy::too_many_arguments)]
struct TaskParams {
    tile_size: i32,
    tile_overlap: i32,
    min_tile_size: i32,
    max_tile_size: i32,
}

/// Upscale a submitted image on the PC's own engine (mirror of
/// `process_locally`). Inference is serialized under the engine mutex
/// (Vulkan is not concurrency-safe); the post-inference encode deliberately
/// runs OUTSIDE the lock: it is pure CPU over local buffers and can take
/// seconds on large outputs.
#[allow(clippy::too_many_arguments)]
fn process_locally(
    sh: &Shared,
    image_data: &[u8],
    width: i32,
    height: i32,
    channels: i32,
    model_name: &str,
    cfg: &TaskParams,
    output_format: &str,
    output_quality: i32,
    encoder_opts: &EncoderOptions,
    progress_sink: Option<&dyn Fn(f32)>,
    cancel_flag: &AtomicBool,
) -> Result<Vec<u8>, CoordinatorError> {
    let canceled = || cancel_flag.load(Ordering::Relaxed);

    ensure_engine_ready(sh, model_name).map_err(|e| CoordinatorError {
        code: 2,
        message: format!("Engine/model not ready: {model_name}: {e}"),
    })?;
    if canceled() {
        return Err(CoordinatorError { code: 7, message: "canceled".into() });
    }

    let out_bytes: Vec<u8>;
    let mut effective_model = model_name.to_string();
    let scale;
    {
        let engine_slot = sh.engine.lock().unwrap();
        // Re-check after the (possibly long) engine lock wait: another
        // connection's task may have run while this one queued behind it.
        if canceled() {
            return Err(CoordinatorError { code: 7, message: "canceled".into() });
        }
        let engine = engine_slot.engine.as_ref().ok_or_else(|| CoordinatorError {
            code: 2,
            message: "engine missing".into(),
        })?;
        scale = engine.get_scale_factor();
        let out_w = width as i64 * scale as i64;
        let out_h = height as i64 * scale as i64;
        let out_bytes_n = out_w.max(0) as u64 * out_h.max(0) as u64 * channels.max(0) as u64;
        if out_w <= 0 || out_h <= 0 || out_bytes_n > MAX_RAW_OUTPUT_BYTES {
            return Err(CoordinatorError { code: 3, message: "Invalid output dimensions".into() });
        }

        // tile_size 0 = remote auto: the phone did not pin a size, so the PC
        // picks one with the same TileSizeOptimizer probe the CLI's
        // --auto-tile uses (process-level per-model cache). The engine
        // recommendation is the fallback if the probe cannot produce a
        // usable size.
        let mut tile_size = cfg.tile_size;
        if tile_size == 0 {
            let model_for_opt = if model_name.is_empty() {
                engine_slot.loaded_model.clone()
            } else {
                model_name.to_string()
            };
            tile_size = sh.cfg.backend.auto_tile_size(
                engine.as_ref(),
                &model_for_opt,
                cfg.min_tile_size,
                cfg.max_tile_size,
                cfg.tile_overlap,
                width,
                height,
            );
            if tile_size <= 0 {
                tile_size = engine.get_recommended_tile_size();
            }
            log::info!("Auto-selected tile size {tile_size} for remote task (tile_size=0)");
        }

        if model_name.is_empty() {
            effective_model = engine_slot.loaded_model.clone();
        }
        log::info!(
            "Processing locally: {}x{} model={effective_model} tile={tile_size}",
            width,
            height
        );

        let tc = TileConfig {
            tile_size,
            overlap: cfg.tile_overlap.max(0),
            batch_size: 1,
        };

        let mut progress_adapter = progress_sink.map(|f| {
            move |p: &upix_core::engine::ProcessingProgress| {
                f(p.percentage() / 100.0);
                if let Some(cb) = sh.on_task_progress.lock().unwrap().as_ref() {
                    cb(p.percentage() / 100.0);
                }
            }
        });
        let res = sh.cfg.backend.process_image_u8(
            engine.as_ref(),
            tc,
            image_data,
            width,
            height,
            channels,
            progress_adapter
                .as_mut()
                .map(|f| f as &mut dyn FnMut(&upix_core::engine::ProcessingProgress)),
            cancel_flag,
            /*enable_alpha_bicubic=*/ true,
        );
        match res {
            Ok(bytes) => out_bytes = bytes,
            Err(e) => {
                return Err(CoordinatorError {
                    code: 4,
                    message: e.message,
                })
            }
        }
    }

    // Canceled between the last tile and the encode: skip the multi-second,
    // un-interruptible output encode — nobody is waiting for the bytes.
    if canceled() {
        return Err(CoordinatorError { code: 7, message: "canceled".into() });
    }

    // Encode before transport: a camera photo at x2 is hundreds of MB raw but
    // only a few MB as jpg/webp.
    //   png — lossless; the zlib level only trades encode CPU for transport
    //   SIZE. Level 9 was measured as a pure pessimization (111 s CPU to save
    //   ~9 MB vs level 1 on a 7000x7200 photo); quality 61 → zlib level 4,
    //   ~24x faster, identical decoded pixels.
    let mut transport_quality = output_quality;
    let mut transport_opts = *encoder_opts;
    if output_format == "png" {
        transport_quality = 61;
        // The transport encode is a bandwidth knob, not the user's output
        // setting: force the derived level so an explicit --png-level cannot
        // re-inflate the transport stream.
        transport_opts.png_level = -1;
    }
    let out_image = crate::seams::Img {
        width: (width as i64 * scale as i64) as i32,
        height: (height as i64 * scale as i64) as i32,
        channels,
        data: out_bytes,
    };
    // The SR marker rides with the remote output: the phone writes these
    // bytes verbatim, so remote results are recognized on re-input the same
    // way local ones are.
    let sr_xmp = sh
        .cfg
        .backend
        .sr_xmp(&effective_model, scale, std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0));
    let encoded = sh.cfg.backend.encode_image(
        &out_image,
        output_format,
        transport_quality,
        Some(&sr_xmp),
        &transport_opts,
    );
    let encoded = match encoded {
        Ok(b) => b,
        Err(e) => {
            return Err(CoordinatorError {
                code: 5,
                message: format!("output encode failed: {e}"),
            })
        }
    };
    if encoded.len() as u64 > u32::MAX as u64 {
        return Err(CoordinatorError {
            code: 6,
            message: format!(
                "Encoded output too large for transport ({} bytes)",
                encoded.len()
            ),
        });
    }
    Ok(encoded)
}

/// Lazily create + initialize the local engine (GPU → CPU fallback) and load
/// `model_name` if it isn't already loaded. Idempotent. Mirrors daemon.cpp.
fn ensure_engine_ready(sh: &Shared, model_name: &str) -> Result<(), String> {
    let mut slot = sh.engine.lock().unwrap();
    if !slot.initialized {
        let engine = sh.cfg.backend.create_engine()?;
        slot.engine = Some(engine);
        slot.initialized = true;
        let dev = slot
            .engine
            .as_ref()
            .unwrap()
            .current_device();
        log::info!(
            "Local engine ready on {}",
            match dev {
                upix_core::engine::DeviceType::GpuVulkan => "GPU",
                _ => "CPU",
            }
        );
    }
    // switch_model: load only when a different model is requested.
    if slot.loaded_model != model_name {
        let Some(model) = sh.cfg.backend.get_model(model_name) else {
            return Err(format!("model not found: {model_name}"));
        };
        let engine = slot.engine.as_mut().ok_or("engine missing")?;
        engine
            .load_model(&model.param_path, &model.bin_path)
            .map_err(|e| format!("load_model({model_name}) failed: {}", e.message))?;
        slot.loaded_model = model_name.to_string();
    }
    Ok(())
}

// ── model library ops ───────────────────────────────────────────────────────

/// Effective models dir for shared-library ops (override > configured base).
fn effective_models_dir(sh: &Shared) -> PathBuf {
    match sh.models_dir_override.lock().unwrap().as_ref() {
        Some(d) => d.clone(),
        None => sh.cfg.models_dir.clone(),
    }
}

/// Defense-in-depth: model names become filenames on the PC, so reject
/// anything that could escape the models directory or hide as a dotfile.
fn is_safe_model_name(name: &str) -> bool {
    if name.is_empty() || name.len() >= 64 {
        return false;
    }
    if name.starts_with('.') {
        return false;
    }
    name.chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == ' ')
}

fn handle_model_list(sh: &Shared, conn: &Conn) {
    let models = {
        let _models_guard = sh.models_mutex.lock().unwrap();
        if let Err(e) = sh.cfg.backend.scan_models(&effective_models_dir(sh)) {
            log::error!("Model list scan failed: {e}");
        }
        sh.cfg.backend.available_models()
    };
    let mut payload = Vec::with_capacity(4 + models.len() * wireio::MODEL_LIBRARY_ENTRY_SIZE);
    payload.extend_from_slice(&wireio::enc_model_list_response(models.len() as u32));
    for m in &models {
        let mut name = [0u8; 64];
        let n = m.name.as_bytes().len().min(63);
        name[..n].copy_from_slice(&m.name.as_bytes()[..n]);
        let entry = wireio::enc_library_entry(&upix_net::protocol::ModelLibraryEntry {
            name,
            scale_factor: m.scale_factor as u32,
            total_size: m.total_size,
        });
        payload.extend_from_slice(&entry);
    }
    if !send_sealed(conn, MessageType::ModelListResponse, &payload) {
        log::error!("Failed to send model list response");
        return;
    }
    log::info!("Sent model library list ({} models) to phone", models.len());
}

fn handle_model_upload(sh: &Shared, conn: &Conn, payload: &[u8]) {
    let fail = |message: &str| {
        let mut resp = upix_net::protocol::ModelOpResponse {
            success: 0,
            error: [0u8; 128],
        };
        let msg = message.as_bytes();
        let n = msg.len().min(127);
        resp.error[..n].copy_from_slice(&msg[..n]);
        log::error!("Model upload rejected: {message}");
        let _ = send_sealed(conn, MessageType::ModelUploadResponse, &wireio::enc_op_response(&resp));
    };

    let Some(req) = wireio::dec_upload_request(payload) else {
        fail("upload payload truncated");
        return;
    };
    let name = wireio::rd_bounded_str(&req.name, 0, 64);
    if !is_safe_model_name(&name) {
        fail(&format!("invalid model name: {name}"));
        return;
    }
    if req.param_size == 0
        || req.bin_size == 0
        || req.param_size > MAX_NETWORK_PAYLOAD as u64
        || req.bin_size > MAX_NETWORK_PAYLOAD as u64
        || req.param_size + req.bin_size > MAX_NETWORK_PAYLOAD as u64
        || wireio::MODEL_UPLOAD_REQUEST_SIZE + req.param_size as usize + req.bin_size as usize
            != payload.len()
    {
        fail("invalid upload sizes");
        return;
    }
    let param_begin = wireio::MODEL_UPLOAD_REQUEST_SIZE;
    let bin_begin = param_begin + req.param_size as usize;

    // Write flat into the models dir: <name>.param / <name>.bin. Stage both
    // files, then rename into place so a short write or crash can never
    // leave a half-written pair. The whole install runs under models_mutex_
    // (it serializes two uploads of the same name and keeps readers out of a
    // file being replaced).
    let models_dir = sh.cfg.backend.models_dir();
    let param_path = models_dir.join(format!("{name}.param"));
    let bin_path = models_dir.join(format!("{name}.bin"));
    let param_tmp = models_dir.join(format!("{name}.param.upix-upload"));
    let bin_tmp = models_dir.join(format!("{name}.bin.upix-upload"));
    // Backups: the two install renames are not atomic as a pair, so a
    // failure after the first would otherwise leave a new .param next to an
    // old .bin. Staging the previous files aside lets every failure path put
    // them back. (Leftover .upix-* files are ignored by the scanner, which
    // matches the .param/.bin extension exactly.)
    let param_bak = models_dir.join(format!("{name}.param.upix-backup"));
    let bin_bak = models_dir.join(format!("{name}.bin.upix-backup"));

    // Refuse a destination that is not a regular file (a directory, fifo,
    // ...); a symlink to a regular file is fine.
    let inspect = |path: &Path| -> Result<bool, String> {
        match std::fs::symlink_metadata(path) {
            Err(_) => Ok(false),
            Ok(md) if md.is_file() => Ok(true),
            Ok(md) if md.file_type().is_symlink() => match std::fs::metadata(path) {
                Ok(t) if t.is_file() => Ok(true),
                _ => Err(format!(
                    "existing destination for {name} is not a regular file"
                )),
            },
            Ok(_) => Err(format!(
                "existing destination for {name} is not a regular file"
            )),
        }
    };
    let had_param = match inspect(&param_path) {
        Ok(v) => v,
        Err(m) => {
            fail(&m);
            return;
        }
    };
    let had_bin = match inspect(&bin_path) {
        Ok(v) => v,
        Err(m) => {
            fail(&m);
            return;
        }
    };

    let write_result: Result<(), String> = {
        let _models_guard = sh.models_mutex.lock().unwrap();
        (|| -> Result<(), String> {
            write_file(&param_tmp, &payload[param_begin..bin_begin])?;
            write_file(&bin_tmp, &payload[bin_begin..])?;
            let mut saved_param = false;
            let mut saved_bin = false;
            let installed_param = std::cell::Cell::new(false);
            let installed_bin = std::cell::Cell::new(false);
            let rollback = |installed_param: bool, installed_bin: bool, saved_param: bool, saved_bin: bool| {
                // Roll back: drop only what THIS attempt installed, then
                // restore only what it actually moved aside — a stale backup
                // left by an earlier crash must never overwrite a still-good
                // installed file.
                if installed_param {
                    let _ = std::fs::remove_file(&param_path);
                }
                if installed_bin {
                    let _ = std::fs::remove_file(&bin_path);
                }
                let _ = std::fs::remove_file(&param_tmp);
                let _ = std::fs::remove_file(&bin_tmp);
                if saved_param {
                    let _ = std::fs::rename(&param_bak, &param_path);
                }
                if saved_bin {
                    let _ = std::fs::rename(&bin_bak, &bin_path);
                }
                let _ = sh.cfg.backend.scan_models(&effective_models_dir(sh));
            };
            if had_param {
                if std::fs::rename(&param_path, &param_bak).is_err() {
                    rollback(false, false, false, false);
                    return Err("cannot back up existing .param".into());
                }
                saved_param = true;
            }
            if had_bin {
                if std::fs::rename(&bin_path, &bin_bak).is_err() {
                    rollback(false, false, saved_param, false);
                    return Err("cannot back up existing .bin".into());
                }
                saved_bin = true;
            }
            if std::fs::rename(&param_tmp, &param_path).is_err() {
                rollback(false, false, saved_param, saved_bin);
                return Err("cannot install .param".into());
            }
            installed_param.set(true);
            if std::fs::rename(&bin_tmp, &bin_path).is_err() {
                rollback(installed_param.get(), false, saved_param, saved_bin);
                return Err("cannot install .bin".into());
            }
            installed_bin.set(true);
            // Rescan so the new model is immediately usable and visible to
            // lists.
            let _ = sh.cfg.backend.scan_models(&effective_models_dir(sh));
            Ok(())
        })()
    };
    if let Err(e) = write_result {
        fail(&format!("failed to write model files: {e}"));
        return;
    }
    let resp = upix_net::protocol::ModelOpResponse {
        success: 1,
        error: [0u8; 128],
    };
    if !send_sealed(conn, MessageType::ModelUploadResponse, &wireio::enc_op_response(&resp)) {
        log::error!("Failed to send upload ack for {name}");
        return;
    }
    log::info!("Model uploaded from phone: {name} ({} bytes)", req.param_size + req.bin_size);
}

fn write_file(path: &Path, data: &[u8]) -> Result<(), String> {
    let mut f = std::fs::File::create(path).map_err(|e| format!("{}: {e}", path.display()))?;
    f.write_all(data).map_err(|e| format!("{}: {e}", path.display()))?;
    f.sync_all().ok();
    Ok(())
}

fn handle_model_download(sh: &Shared, conn: &Conn, payload: &[u8]) {
    let fail = |message: &str| {
        let mut resp = upix_net::protocol::ModelDownloadResponse {
            success: 0,
            scale_factor: 0,
            total_size: 0,
            param_size: 0,
            bin_size: 0,
            error: [0u8; 128],
        };
        let msg = message.as_bytes();
        let n = msg.len().min(127);
        resp.error[..n].copy_from_slice(&msg[..n]);
        log::error!("Model download failed: {message}");
        let _ = send_sealed(
            conn,
            MessageType::ModelDownloadResponse,
            &wireio::enc_download_response(&resp),
        );
    };

    let Some(req) = wireio::dec_download_request(payload) else {
        fail("download request truncated");
        return;
    };
    let name = wireio::rd_bounded_str(&req.name, 0, 64);
    let model = {
        let _models_guard = sh.models_mutex.lock().unwrap();
        if let Err(e) = sh.cfg.backend.scan_models(&effective_models_dir(sh)) {
            fail(&format!("model scan failed: {e}"));
            return;
        }
        sh.cfg.backend.get_model(&name)
    };
    let Some(model) = model else {
        fail(&format!("model not found: {name}"));
        return;
    };
    let param_size = match std::fs::metadata(&model.param_path) {
        Ok(m) => m.len(),
        Err(_) => {
            fail("cannot stat param file");
            return;
        }
    };
    let bin_size = match std::fs::metadata(&model.bin_path) {
        Ok(m) => m.len(),
        Err(_) => {
            fail("cannot stat bin file");
            return;
        }
    };
    // The reply is ONE encrypted frame: the receiver rejects any frame whose
    // payload_length exceeds MAX_NETWORK_PAYLOAD. Cap by what actually fits
    // on the wire so an oversized model fails with a clear message here
    // instead of an opaque disconnect on the phone.
    let max_transfer_bytes =
        MAX_NETWORK_PAYLOAD as u64 - wireio::MODEL_DOWNLOAD_RESPONSE_SIZE as u64 - SEAL_OVERHEAD as u64;
    if param_size + bin_size > max_transfer_bytes {
        fail(&format!(
            "model too large for the single-frame transfer limit ({param_size} + {bin_size} > {max_transfer_bytes})"
        ));
        return;
    }
    let (param_bytes, bin_bytes) = match (std::fs::read(&model.param_path), std::fs::read(&model.bin_path))
    {
        (Ok(p), Ok(b)) => (p, b),
        _ => {
            fail("failed to read model files");
            return;
        }
    };
    let resp = upix_net::protocol::ModelDownloadResponse {
        success: 1,
        scale_factor: model.scale_factor as u32,
        total_size: param_size + bin_size,
        param_size,
        bin_size,
        error: [0u8; 128],
    };
    let mut out = Vec::with_capacity(
        wireio::MODEL_DOWNLOAD_RESPONSE_SIZE + param_bytes.len() + bin_bytes.len(),
    );
    out.extend_from_slice(&wireio::enc_download_response(&resp));
    out.extend_from_slice(&param_bytes);
    out.extend_from_slice(&bin_bytes);
    if !send_sealed(conn, MessageType::ModelDownloadResponse, &out) {
        log::error!("Failed to send model download response for {name}");
        return;
    }
    log::info!("Model sent to phone: {name} ({})", param_size + bin_size);
}
