//! Coordinator integration tests (CONTRACT §6 acceptance): real handshake /
//! pairing / submit flows driven through the coordinator state machine over
//! in-memory transport with the mock crypto/backend.
//!
//! Note: the sibling crates' crypto (upix-net) is still landing; the mock
//! "session" is a plaintext pass-through with the same frame shape. The
//! state machines under test (pairing gate, dual submit formats, chunked
//! reassembly + interrupt isolation, progress pump, cancel, per-task error
//! isolation) are the host crate's own logic and are exercised for real here.

mod common;

use std::sync::atomic::Ordering;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use common::*;
use upix_host::coordinator::{Coordinator, CoordinatorConfig, PendingApproval};
use upix_host::seams::SessionCodec as _;
use upix_net::protocol::{MessageType, TaskProgress};
use upix_host::wireio;

fn test_config(
    port: u16,
    auto_approve: bool,
    backend: Arc<MockBackend>,
    trust: Option<Arc<MockTrust>>,
    sink: Option<mpsc::Sender<PendingApproval>>,
) -> Coordinator {
    let cfg = CoordinatorConfig {
        port,
        bind_addr: "127.0.0.1".into(),
        models_dir: backend.dir.clone(),
        auto_approve,
        backend,
        framer: Arc::new(PlainFramer),
        crypto: Arc::new(MockCrypto),
        signer: Arc::new(MockSigner { key: [0x11; 32] }),
        trust: trust.map(|t| t as Arc<dyn upix_host::seams::TrustApi>),
        listener_factory: Box::new(FactoryRegistry),
    };
    let coord = Coordinator::new(cfg);
    if let Some(sink) = sink {
        coord.set_approval_sink(sink);
    }
    coord
}

/// In-process pipe listener. The factory registers each fresh listener in a
/// thread-local so the test body can push accepted connections into the
/// coordinator's accept loop (each #[test] runs on its own thread).
#[derive(Default)]
struct PipeListener {
    incoming: std::sync::Mutex<std::collections::VecDeque<Box<dyn upix_host::seams::Transport>>>,
    open: std::sync::atomic::AtomicBool,
    cv: std::sync::Condvar,
}

struct PipeListenerHandle {
    l: Arc<PipeListener>,
}

impl upix_host::seams::Listener for PipeListenerHandle {
    fn accept(&self) -> Result<Option<Box<dyn upix_host::seams::Transport>>, String> {
        let mut q = self.l.incoming.lock().unwrap();
        loop {
            if let Some(t) = q.pop_front() {
                return Ok(Some(t));
            }
            if !self.l.open.load(Ordering::SeqCst) {
                return Ok(None);
            }
            let (g, _) = self
                .l
                .cv
                .wait_timeout(q, Duration::from_millis(50))
                .unwrap();
            q = g;
        }
    }

    fn shutdown(&self) {
        self.l.open.store(false, Ordering::SeqCst);
        self.l.cv.notify_all();
    }
}

thread_local! {
    static LISTENER: std::sync::Mutex<Option<Arc<PipeListener>>> = const { std::sync::Mutex::new(None) };
}

struct FactoryRegistry;
impl upix_host::seams::ListenerFactory for FactoryRegistry {
    fn listen(&self, _port: u16) -> Result<Box<dyn upix_host::seams::Listener>, String> {
        let l = Arc::new(PipeListener::default());
        l.open.store(true, Ordering::SeqCst);
        LISTENER.with(|c| *c.lock().unwrap() = Some(Arc::clone(&l)));
        Ok(Box::new(PipeListenerHandle { l }))
    }
}

fn connect_registered() -> (Arc<TestClient>, Arc<TestPipe>) {
    let l = LISTENER
        .with(|c| c.lock().unwrap().clone())
        .expect("listener registered");
    let (server_side, client_side) = TestPipe::pair();
    let (client, pipe) = TestClient::new(client_side);
    l.incoming.lock().unwrap().push_back(Box::new(server_side));
    l.cv.notify_all();
    (client, pipe)
}

fn full_pairing(client: &TestClient) {
    client_handshake(client, [0xAA; 32]).expect("handshake");
    // CONNECT_REQUEST.
    let creq = upix_net::protocol::ClientConnectRequest {
        device_name: fixed_str::<64>("test-phone"),
        fingerprint: fixed_str::<17>("0011223344556677"),
    };
    let sealed = PlainSession
        .seal(MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq))
        .unwrap();
    client.pipe.write(&sealed);

    // CONNECT_RESPONSE.
    let (hdr, payload) = client.recv_frame(3000).expect("no CONNECT_RESPONSE");
    assert_eq!(hdr.msg_type, MessageType::ClientConnectResponse);
    let resp = wireio::dec_connect_response(&payload).expect("bad CONNECT_RESPONSE");
    assert_eq!(resp.approved, 1);
    assert_eq!(resp.pipeline_depth, 2);
}

fn tmp_dir(tag: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!(
        "upix-host-test-{tag}-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn make_backend(dir: std::path::PathBuf) -> Arc<MockBackend> {
    make_backend_with_delay(dir, 0)
}

fn make_backend_with_delay(dir: std::path::PathBuf, tile_delay_ms: u64) -> Arc<MockBackend> {
    MockBackend::install_model_files("Mock-x2", &dir);
    let mut b = MockBackend::new(
        dir,
        vec![MockModel {
            name: "Mock-x2".into(),
            scale: 2,
            input_channels: 3,
        }],
        2,
    );
    b.tile_delay_ms = tile_delay_ms;
    Arc::new(b)
}

/// Full happy path: coordinator(auto_approve) + real handshake/pairing/
/// legacy submit of a small PNG → success response with upscaled result.
#[test]
fn success_flow_legacy_submit_small_png() {
    let dir = tmp_dir("success");
    let backend = make_backend(dir.clone());
    let mut coord = test_config(8901, true, Arc::clone(&backend), None, None);
    // Swap in the registering factory.
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let (client, _pipe) = connect_registered();
    full_pairing(&client);

    // Legacy single-frame submit: struct + inline input (a PNG).
    let png = make_test_png(8, 4);
    let payload = legacy_submit_payload("Mock-x2", "png", "png", &png);
    client.send(MessageType::ClientSubmit, &payload);

    // Expect TASK_PROGRESS (stage 1 encode label) then CLIENT_RESPONSE.
    let mut got_progress_stage1 = false;
    let mut response = None;
    for _ in 0..20 {
        match client.recv_frame(3000) {
            Some((h, p)) => {
                if h.msg_type == MessageType::TaskProgress {
                    let tp = wireio::dec_task_progress(&p).unwrap();
                    if tp.stage == 1 {
                        got_progress_stage1 = true;
                    }
                } else if h.msg_type == MessageType::ClientResponse {
                    response = Some(p);
                    break;
                }
            }
            None => break,
        }
    }
    let payload = response.expect("no CLIENT_RESPONSE");
    assert!(got_progress_stage1, "missing stage-1 encode progress frame");
    let resp = wireio::dec_task_response(&payload).expect("bad response");
    assert_eq!(resp.success, 1, "task failed: {:?}", String::from_utf8_lossy(&resp.error_message));
    // Result data rides in the same frame (small result).
    assert_eq!(
        resp.result_data_length as usize,
        payload.len() - wireio::CLIENT_TASK_RESPONSE_SIZE
    );
    // Result decodes as a 16x8 PNG (x2 upscale of 8x4).
    let result_bytes = &payload[wireio::CLIENT_TASK_RESPONSE_SIZE..];
    let (w, h, c, _) = decode_png(result_bytes).expect("result not a PNG");
    assert_eq!((w, h, c), (16, 8, 3));
    assert_eq!(backend.process_calls.load(Ordering::SeqCst), 1);

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// Chunked submit: struct-only frame + DATA continuations → success; and a
/// chunked submit interrupted with a non-DATA frame → per-task error, and
/// the session stays usable (next submit succeeds).
#[test]
fn chunked_submit_complete_and_interrupted() {
    let dir = tmp_dir("chunked");
    let backend = make_backend(dir.clone());
    let mut coord = test_config(8902, true, Arc::clone(&backend), None, None);
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let (client, _pipe) = connect_registered();
    full_pairing(&client);

    // ── Interrupted chunked submit ──
    let png = make_test_png(8, 4);
    let head = chunked_submit_header("Mock-x2", "png", "png", png.len() as u32);
    client.send(MessageType::ClientSubmit, &head);
    // Instead of DATA, send a stray frame type (TASK_CANCEL would be
    // tolerated; use MODEL_LIST_REQUEST — non-DATA) to break reassembly.
    client.send(MessageType::ModelListRequest, &[]);

    // Per-task error arrives, connection stays open.
    let (h, p) = client.recv_frame(3000).expect("no reply to interrupted chunk");
    assert_eq!(h.msg_type, MessageType::ClientResponse);
    let resp = wireio::dec_task_response(&p).unwrap();
    assert_eq!(resp.success, 0);
    let msg = wireio::rd_bounded_str(&p, 1, 128);
    assert!(
        msg.contains("chunked submit interrupted"),
        "unexpected error: {msg}"
    );

    // ── Session reusable: complete chunked submit now succeeds ──
    let head = chunked_submit_header("Mock-x2", "png", "png", png.len() as u32);
    client.send(MessageType::ClientSubmit, &head);
    // DATA continuations (two halves).
    let mid = png.len() / 2;
    client.send(MessageType::ClientData, &png[..mid]);
    client.send(MessageType::ClientData, &png[mid..]);

    let mut response = None;
    for _ in 0..20 {
        match client.recv_frame(3000) {
            Some((h, p)) => {
                if h.msg_type == MessageType::ClientResponse {
                    response = Some(p);
                    break;
                }
            }
            None => break,
        }
    }
    let p = response.expect("no CLIENT_RESPONSE for chunked submit");
    let resp = wireio::dec_task_response(&p).unwrap();
    assert_eq!(resp.success, 1, "chunked task failed");
    assert_eq!(
        resp.result_data_length as usize,
        p.len() - wireio::CLIENT_TASK_RESPONSE_SIZE
    );
    assert_eq!(backend.process_calls.load(Ordering::SeqCst), 1); // interrupted one never ran

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// Submit then immediate cancel → success=0 with "canceled"; a second submit
/// on the same session still succeeds.
#[test]
fn cancel_after_submit_yields_canceled() {
    let dir = tmp_dir("cancel");
    let backend = make_backend_with_delay(dir.clone(), 40);
    let mut coord = test_config(8903, true, backend, None, None);
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let (client, _pipe) = connect_registered();
    full_pairing(&client);

    let png = make_test_png(8, 4);
    let payload = legacy_submit_payload("Mock-x2", "png", "png", &png);
    client.send(MessageType::ClientSubmit, &payload);
    // Cancel right away (races the worker start — tolerated).
    client.send(MessageType::TaskCancel, &[]);

    let mut response = None;
    for _ in 0..50 {
        match client.recv_frame(2000) {
            Some((h, p)) => {
                if h.msg_type == MessageType::ClientResponse {
                    response = Some(p);
                    break;
                }
            }
            None => break,
        }
    }
    let p = response.expect("no CLIENT_RESPONSE after cancel");
    let resp = wireio::dec_task_response(&p).unwrap();
    assert_eq!(resp.success, 0);
    let msg = wireio::rd_bounded_str(&p, 1, 128);
    assert_eq!(msg, "canceled", "expected the phone's cancel key string");

    // Session still usable.
    let payload = legacy_submit_payload("Mock-x2", "png", "png", &png);
    client.send(MessageType::ClientSubmit, &payload);
    let mut response = None;
    for _ in 0..30 {
        match client.recv_frame(3000) {
            Some((h, p)) => {
                if h.msg_type == MessageType::ClientResponse {
                    response = Some(p);
                    break;
                }
            }
            None => break,
        }
    }
    let p = response.expect("no CLIENT_RESPONSE after cancel recovery");
    let resp = wireio::dec_task_response(&p).unwrap();
    assert_eq!(resp.success, 1, "post-cancel submit failed");

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// First-time peers block on approval: pop_pending_approval surfaces the
/// request, denying yields approved=0, the trust store is NOT updated, and a
/// reconnecting denied peer must be approved again. Approving then makes the
/// next connection auto-approved (trusted).
#[test]
fn pairing_gate_approve_deny_reconnect() {
    let dir = tmp_dir("pairing");
    let backend = make_backend(dir.clone());
    let trust = Arc::new(MockTrust::default());
    let (tx, rx) = mpsc::channel();
    let mut coord = test_config(8904, false, Arc::clone(&backend), Some(Arc::clone(&trust)), Some(tx));
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    // ── Connection 1: request arrives, deny it ──
    let (client, _pipe) = connect_registered();
    client_handshake(&client, [0xAA; 32]).expect("handshake");
    let creq = upix_net::protocol::ClientConnectRequest {
        device_name: fixed_str::<64>("phone-a"),
        fingerprint: fixed_str::<17>("deadbeefdeadbeef"),
    };
    let sealed = PlainSession
        .seal(MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq))
        .unwrap();
    client.pipe.write(&sealed);

    // The channel sink forwards the request.
    let req = rx.recv_timeout(Duration::from_secs(3)).expect("no approval event");
    assert_eq!(req.device_name, "phone-a");
    assert_eq!(req.fingerprint.len(), 16);
    coord.resolve_approval(req.id, false);

    // Client gets approved=0.
    let (h, p) = client.recv_frame(3000).expect("no CONNECT_RESPONSE");
    assert_eq!(h.msg_type, MessageType::ClientConnectResponse);
    let resp = wireio::dec_connect_response(&p).unwrap();
    assert_eq!(resp.approved, 0);
    assert!(!trust.contains(&[0xAA; 32]), "denied peer must not be trusted");

    // ── Connection 2 (fresh): approve it ──
    let (client2, _pipe2) = connect_registered();
    client_handshake(&client2, [0xAA; 32]).expect("handshake 2");
    let sealed = PlainSession
        .seal(MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq))
        .unwrap();
    client2.pipe.write(&sealed);
    let req = rx.recv_timeout(Duration::from_secs(3)).expect("no approval 2");
    coord.resolve_approval(req.id, true);

    let (h, p) = client2.recv_frame(3000).expect("no CONNECT_RESPONSE 2");
    assert_eq!(h.msg_type, MessageType::ClientConnectResponse);
    let resp = wireio::dec_connect_response(&p).unwrap();
    assert_eq!(resp.approved, 1);
    assert!(trust.contains(&[0xAA; 32]), "approved peer must be trusted");

    // ── Connection 3: trusted peer auto-approves, no new request ──
    let (client3, _pipe3) = connect_registered();
    client_handshake(&client3, [0xAA; 32]).expect("handshake 3");
    let sealed = PlainSession
        .seal(MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq))
        .unwrap();
    client3.pipe.write(&sealed);
    let (h, p) = client3
        .recv_frame(3000)
        .expect("trusted peer got no CONNECT_RESPONSE");
    assert_eq!(h.msg_type, MessageType::ClientConnectResponse);
    let resp = wireio::dec_connect_response(&p).unwrap();
    assert_eq!(resp.approved, 1);
    assert_eq!(resp.trusted, 1);
    assert!(
        rx.try_recv().is_err(),
        "trusted peer must not enqueue a new approval"
    );

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// Model library ops over the session: list returns the scanned model with
/// name/scale/size; download returns param+bin bytes; upload installs a new
/// model into the models dir (staged + renamed).
#[test]
fn model_list_upload_download() {
    let dir = tmp_dir("models");
    let backend = make_backend(dir.clone());
    MockBackend::install_model_files("Mock-x2", &dir);
    let mut coord = test_config(8905, true, Arc::clone(&backend), None, None);
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    // Model ops are one-shot: handshake → CONNECT_REQUEST → op frame →
    // reply, then the session ends (same as the C++).
    let (client, _pipe) = connect_registered();
    full_pairing(&client);

    // ── model list ──
    client.send(MessageType::ModelListRequest, &[]);
    let (h, p) = client.recv_frame(3000).expect("no MODEL_LIST_RESPONSE");
    assert_eq!(h.msg_type, MessageType::ModelListResponse);
    let count = wireio::rd_u32(&p, 0).unwrap();
    assert_eq!(count, 1);
    let entry = wireio::dec_library_entry(&p[4..]).unwrap();
    assert_eq!(wireio::rd_bounded_str(&p, 4, 64), "Mock-x2");
    assert_eq!(entry.scale_factor, 2);
    assert_eq!(entry.total_size, 7);

    // ── model download (fresh connection: ops end the session) ──
    let (client, _pipe) = connect_registered();
    full_pairing(&client);
    let req = upix_net::protocol::ModelDownloadRequest {
        name: fixed_str::<64>("Mock-x2"),
    };
    client.send(
        MessageType::ModelDownloadRequest,
        &wireio::enc_download_request(&req),
    );
    let (h, p) = client.recv_frame(3000).expect("no DOWNLOAD_RESPONSE");
    assert_eq!(h.msg_type, MessageType::ModelDownloadResponse);
    let resp = wireio::dec_download_response(&p).unwrap();
    assert_eq!(resp.success, 1);
    assert_eq!(resp.param_size, 5); // b"param"
    assert_eq!(resp.bin_size, 3); // b"bin"
    let param = &p[wireio::MODEL_DOWNLOAD_RESPONSE_SIZE
        ..wireio::MODEL_DOWNLOAD_RESPONSE_SIZE + resp.param_size as usize];
    assert_eq!(param, b"param");

    // ── model upload (fresh connection) ──
    let (client, _pipe) = connect_registered();
    full_pairing(&client);

    // Upload installs <name>.param/.bin into the models dir.
    let mut payload = Vec::new();
    payload.extend_from_slice(&wireio::enc_upload_request(
        &upix_net::protocol::ModelUploadRequest {
            name: fixed_str::<64>("Uploaded-x3"),
            scale_factor: 3,
            param_size: 5,
            bin_size: 3,
        },
    ));
    payload.extend_from_slice(b"param");
    payload.extend_from_slice(b"bin");
    client.send(MessageType::ModelUpload, &payload);
    let (h, p) = client.recv_frame(3000).expect("no UPLOAD_RESPONSE");
    assert_eq!(h.msg_type, MessageType::ModelUploadResponse);
    let resp = wireio::dec_op_response(&p).unwrap();
    assert_eq!(resp.success, 1, "upload rejected");
    assert_eq!(std::fs::read(dir.join("Uploaded-x3.param")).unwrap(), b"param");
    assert_eq!(std::fs::read(dir.join("Uploaded-x3.bin")).unwrap(), b"bin");

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// Progress slot: while a (slow) task runs, TASK_PROGRESS stage-0 frames
/// arrive with increasing percents from the atomic slot (worker never writes
/// the socket directly — the pump emits at most one frame per poll tick).
#[test]
fn progress_frames_flow_from_atomic_slot() {
    let dir = tmp_dir("progress");
    let backend = make_backend_with_delay(dir.clone(), 60);
    let mut coord = test_config(8906, true, backend, None, None);
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let (client, _pipe) = connect_registered();
    full_pairing(&client);

    // 8x64 → 8 simulated tiles; with 60ms per tile the pump observes the
    // atomic slot mid-task (a completed task drops its trailing slot value,
    // exactly like the C++: terminal response wins).
    let png = make_test_png(8, 64);
    let payload = legacy_submit_payload("Mock-x2", "png", "png", &png);
    client.send(MessageType::ClientSubmit, &payload);

    let mut stage0 = 0;
    let mut last_pct: Option<u32> = None;
    loop {
        match client.recv_frame(5000) {
            Some((h, p)) if h.msg_type == MessageType::TaskProgress => {
                let tp: TaskProgress = wireio::dec_task_progress(&p).unwrap();
                if tp.stage == 0 {
                    stage0 += 1;
                    if let Some(last) = last_pct {
                        assert!(tp.percent >= last, "percent went backwards");
                    }
                    last_pct = Some(tp.percent);
                }
            }
            Some((h, p)) if h.msg_type == MessageType::ClientResponse => {
                let resp = wireio::dec_task_response(&p).unwrap();
                assert_eq!(resp.success, 1);
                break;
            }
            _ => panic!("stream ended without response"),
        }
    }
    assert!(stage0 >= 1, "expected at least one stage-0 progress frame");

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}
