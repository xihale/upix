//! Serve daemon integration test (CONTRACT §6 acceptance): UDS request →
//! progress frames → response, with MockEngine; plus pid-file status/stop
//! semantics and the output-path derivation rule.

mod common;

use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;

use common::*;
use upix_host::coordinator::Framer;
use upix_host::seams::UdsFactory;
use upix_host::serve::{run_daemon, DaemonStatus, ServeConfig, ServeDevice};
use upix_net::protocol::{MessageType, ServeProgress};
use upix_host::wireio;

/// Pipe-based UDS listener for in-process daemon tests (same accept
/// semantics as the std adapter: poll tick, shutdown wake).
#[derive(Default)]
struct PipeUdsListener {
    pending: std::sync::Mutex<std::collections::VecDeque<Box<dyn upix_host::seams::Transport>>>,
    open: std::sync::atomic::AtomicBool,
    cv: std::sync::Condvar,
}

impl upix_host::seams::Listener for PipeUdsListener {
    fn accept(&self) -> Result<Option<Box<dyn upix_host::seams::Transport>>, String> {
        let mut q = self.pending.lock().unwrap();
        loop {
            if let Some(t) = q.pop_front() {
                return Ok(Some(t));
            }
            if !self.open.load(Ordering::SeqCst) {
                return Ok(None);
            }
            let (g, _) = self
                .cv
                .wait_timeout(q, Duration::from_millis(50))
                .unwrap();
            q = g;
        }
    }

    fn shutdown(&self) {
        self.open.store(false, Ordering::SeqCst);
        self.cv.notify_all();
    }
}

struct PipeUdsFactory;

impl UdsFactory for PipeUdsFactory {
    fn listen(&self, _socket_path: &Path) -> Result<Box<dyn upix_host::seams::Listener>, String> {
        let l = Arc::new(PipeUdsListener::default());
        l.open.store(true, Ordering::SeqCst);
        *PENDING_UDS.lock().unwrap() = Some(Arc::clone(&l));
        Ok(Box::new(PipeUdsListenerHandle { l }))
    }

    fn connect(&self, _socket_path: &Path) -> Result<Box<dyn upix_host::seams::Transport>, String> {
        Err("use connect_registered in this test".into())
    }
}

static PENDING_UDS: std::sync::Mutex<Option<Arc<PipeUdsListener>>> = std::sync::Mutex::new(None);

/// The four daemon tests share the process-global listener registry (the
/// in-process daemon registers its listener at start), so they serialize on
/// this lock to avoid cross-talk when cargo runs them in parallel threads.
static SERIAL: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn with_serial<T>(f: impl FnOnce() -> T) -> T {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    f()
}

struct PipeUdsListenerHandle {
    l: Arc<PipeUdsListener>,
}

impl upix_host::seams::Listener for PipeUdsListenerHandle {
    fn accept(&self) -> Result<Option<Box<dyn upix_host::seams::Transport>>, String> {
        self.l.accept()
    }

    fn shutdown(&self) {
        self.l.shutdown()
    }
}

fn uds_connect_registered() -> (Arc<TestClient>, Arc<TestPipe>) {
    let l = PENDING_UDS
        .lock()
        .unwrap()
        .clone()
        .expect("daemon listener registered");
    let (server_side, client_side) = TestPipe::pair();
    let (client, pipe) = TestClient::new(client_side);
    l.pending.lock().unwrap().push_back(Box::new(server_side));
    l.cv.notify_all();
    (client, pipe)
}

fn tmp_dir(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!(
        "upix-host-serve-{tag}-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn serve_config(dir: PathBuf) -> ServeConfig {
    let models_dir = dir.join("models");
    MockBackend::install_model_files("Mock-x2", &models_dir);
    let backend = Arc::new(MockBackend::new(
        models_dir.clone(),
        vec![MockModel {
            name: "Mock-x2".into(),
            scale: 2,
            input_channels: 3,
        }],
        2,
    ));
    ServeConfig {
        socket_path: dir.join("serve.sock").to_string_lossy().into_owned(),
        models_dir,
        model_name: "Mock-x2".into(),
        device: ServeDevice::Cpu,
        idle_timeout_s: 0, // disabled in tests
        backend,
        framer: Arc::new(PlainFramer),
        uds: Box::new(PipeUdsFactory),
    }
}

/// daemon: UDS request (paths only) → ServeProgress stage 0 (tile ticks) +
/// stage 1 (encode) → ServeResponse naming the written file; the file
/// contains the upscaled PNG.
#[test]
fn daemon_uds_request_progress_response() {
    with_serial(|| {
    let dir = tmp_dir("flow");
    // Input image on "disk" (8x4 PNG); output path omitted → daemon derives
    // `<stem>_<model>.png`.
    let input_path = dir.join("photo.png");
    std::fs::write(&input_path, make_test_png(8, 4)).unwrap();

    let cfg = serve_config(dir.clone());
    let handle = run_daemon(cfg).expect("daemon start");
    std::thread::sleep(Duration::from_millis(200));

    let (client, _pipe) = uds_connect_registered();
    // Drive the request through the client-side helper by injecting our pipe
    // client: build + send the request ourselves (the helper would connect
    // through the UdsFactory, which the pipe factory refuses).
    let mut req = upix_net::protocol::ServeRequest {
        model_name: fixed_str::<64>("Mock-x2"),
        output_format: fixed_str::<16>("png"),
        tile_size: 0,
        tile_overlap: 8,
        output_quality: 90,
        input_path_length: input_path.to_string_lossy().len() as u32,
        output_path_length: 0,
        encoder: encoder_wire(),
    };
    let _ = &mut req;
    let mut payload = wireio::enc_serve_request(&req);
    payload.extend_from_slice(input_path.to_string_lossy().as_bytes());

    // Header checksum must verify (plaintext frames enforce it).
    let framer = PlainFramer;
    client.pipe.write(&framer.serialize_frame(MessageType::ServeRequest, &payload));

    let mut stages: Vec<u32> = Vec::new();
    let mut last_tile_pct: Option<i32> = None;
    let response = loop {
        match client.recv_frame(5000) {
            Some((h, p)) if h.msg_type == MessageType::ServeProgress => {
                let pr: ServeProgress = wireio::dec_serve_progress(&p).unwrap();
                if stages.last() != Some(&pr.stage) {
                    stages.push(pr.stage);
                }
                if pr.stage == 0 && pr.total_tiles > 0 {
                    let pct = (pr.completed_tiles as i64 * 100 / pr.total_tiles as i64) as i32;
                    if let Some(last) = last_tile_pct {
                        assert!(pct >= last, "tile percent went backwards");
                    }
                    last_tile_pct = Some(pct);
                }
            }
            Some((h, p)) if h.msg_type == MessageType::ServeResponse => break p,
            Some((h, _)) => panic!("unexpected daemon frame type {}", h.msg_type as u16),
            None => panic!("daemon stream ended without a response"),
        }
    };

    assert_eq!(stages.first(), Some(&0), "missing tile progress stage");
    assert_eq!(stages.last(), Some(&1), "missing encode stage");

    let resp = wireio::dec_serve_response(&response).unwrap();
    assert_eq!(resp.ok, 1);
    assert_eq!(resp.width, 16);
    assert_eq!(resp.height, 8);
    assert_eq!(resp.channels, 3);
    let out_path = String::from_utf8_lossy(
        &response[212..212 + resp.out_path_length as usize],
    )
    .into_owned();
    assert!(
        out_path.ends_with("photo_Mock-x2.png"),
        "derived output path wrong: {out_path}"
    );
    assert_eq!(wireio::rd_bounded_str(&response, 148, 64), "Mock-x2");

    // The daemon wrote the file itself (no image bytes crossed the socket).
    let out_bytes = std::fs::read(&out_path).expect("output file missing");
    let (w, h, c, _) = decode_png(&out_bytes).expect("output not a PNG");
    assert_eq!((w, h, c), (16, 8, 3));

    handle.stop();
    let _ = std::fs::remove_dir_all(dir);
    });
}

/// Lazy model switch: a request naming a different model reloads under the
/// engine lock and the response reflects the model actually used.
#[test]
fn daemon_lazy_model_switch() {
    with_serial(|| {
    let dir = tmp_dir("switch");
    let models_dir = dir.join("models");
    MockBackend::install_model_files("Mock-x2", &models_dir);
    MockBackend::install_model_files("Mock-x4", &models_dir);
    let backend = Arc::new(MockBackend::new(
        models_dir.clone(),
        vec![
            MockModel {
                name: "Mock-x2".into(),
                scale: 2,
                input_channels: 3,
            },
            MockModel {
                name: "Mock-x4".into(),
                scale: 4,
                input_channels: 3,
            },
        ],
        2,
    ));
    let cfg = ServeConfig {
        socket_path: dir.join("serve.sock").to_string_lossy().into_owned(),
        models_dir: models_dir.clone(),
        model_name: "Mock-x2".into(),
        device: ServeDevice::Cpu,
        idle_timeout_s: 0,
        backend,
        framer: Arc::new(PlainFramer),
        uds: Box::new(PipeUdsFactory),
    };
    let handle = run_daemon(cfg).expect("daemon start");
    std::thread::sleep(Duration::from_millis(100));

    let input_path = dir.join("in.png");
    std::fs::write(&input_path, make_test_png(8, 4)).unwrap();
    let out_path = dir.join("out.png");

    let (client, _pipe) = uds_connect_registered();
    let req = upix_net::protocol::ServeRequest {
        model_name: fixed_str::<64>("Mock-x4"),
        output_format: fixed_str::<16>("png"),
        tile_size: 0,
        tile_overlap: 8,
        output_quality: 90,
        input_path_length: input_path.to_string_lossy().len() as u32,
        output_path_length: out_path.to_string_lossy().len() as u32,
        encoder: encoder_wire(),
    };
    let mut payload = wireio::enc_serve_request(&req);
    payload.extend_from_slice(input_path.to_string_lossy().as_bytes());
    payload.extend_from_slice(out_path.to_string_lossy().as_bytes());
    client.pipe.write(&PlainFramer.serialize_frame(MessageType::ServeRequest, &payload));

    let response = loop {
        match client.recv_frame(5000) {
            Some((h, _p)) if h.msg_type == MessageType::ServeProgress => {}
            Some((h, p)) if h.msg_type == MessageType::ServeResponse => break p,
            Some((h, _)) => panic!("unexpected frame {}", h.msg_type as u16),
            None => panic!("stream ended"),
        }
    };
    let resp = wireio::dec_serve_response(&response).unwrap();
    assert_eq!(
        resp.ok,
        1,
        "lazy switch request failed: {}",
        wireio::rd_bounded_str(&response, 20, 128)
    );
    assert_eq!(resp.width, 32); // x4 model
    assert_eq!(resp.height, 16);
    assert_eq!(wireio::rd_bounded_str(&response, 148, 64), "Mock-x4");

    handle.stop();
    let _ = std::fs::remove_dir_all(dir);
    });
}

/// Daemon errors answer per-request ServeResponse(ok=0) without killing the
/// daemon: missing input file, then a good request still succeeds.
#[test]
fn daemon_error_isolation() {
    with_serial(|| {
    let dir = tmp_dir("err");
    let cfg = serve_config(dir.clone());
    let handle = run_daemon(cfg).expect("daemon start");
    std::thread::sleep(Duration::from_millis(200));

    let (client, _pipe) = uds_connect_registered();
    let missing = dir.join("nope.png");
    let req = upix_net::protocol::ServeRequest {
        model_name: fixed_str::<64>("Mock-x2"),
        output_format: fixed_str::<16>("png"),
        tile_size: 0,
        tile_overlap: 8,
        output_quality: 90,
        input_path_length: missing.to_string_lossy().len() as u32,
        output_path_length: 0,
        encoder: encoder_wire(),
    };
    let mut payload = wireio::enc_serve_request(&req);
    payload.extend_from_slice(missing.to_string_lossy().as_bytes());
    client.pipe.write(&PlainFramer.serialize_frame(MessageType::ServeRequest, &payload));

    let response = loop {
        match client.recv_frame(5000) {
            Some((h, _p)) if h.msg_type == MessageType::ServeProgress => {}
            Some((h, p)) if h.msg_type == MessageType::ServeResponse => break p,
            Some((h, _)) => panic!("unexpected frame {}", h.msg_type as u16),
            None => panic!("stream ended"),
        }
    };
    let resp = wireio::dec_serve_response(&response).unwrap();
    assert_eq!(resp.ok, 0);
    let err = wireio::rd_bounded_str(&response, 20, 128);
    assert!(err.contains("load failed"), "unexpected error: {err}");

    // Daemon still serves.
    let input_path = dir.join("good.png");
    std::fs::write(&input_path, make_test_png(8, 4)).unwrap();
    let (client2, _pipe2) = uds_connect_registered();
    let req = upix_net::protocol::ServeRequest {
        model_name: fixed_str::<64>("Mock-x2"),
        output_format: fixed_str::<16>("png"),
        tile_size: 0,
        tile_overlap: 8,
        output_quality: 90,
        input_path_length: input_path.to_string_lossy().len() as u32,
        output_path_length: 0,
        encoder: encoder_wire(),
    };
    let mut payload = wireio::enc_serve_request(&req);
    payload.extend_from_slice(input_path.to_string_lossy().as_bytes());
    client2.pipe.write(&PlainFramer.serialize_frame(MessageType::ServeRequest, &payload));
    let response = loop {
        match client2.recv_frame(5000) {
            Some((h, _p)) if h.msg_type == MessageType::ServeProgress => {}
            Some((h, p)) if h.msg_type == MessageType::ServeResponse => break p,
            Some((h, _)) => panic!("unexpected frame {}", h.msg_type as u16),
            None => panic!("stream ended"),
        }
    };
    let resp = wireio::dec_serve_response(&response).unwrap();
    assert_eq!(resp.ok, 1);

    handle.stop();
    let _ = std::fs::remove_dir_all(dir);
    });
}

/// status(): pid file + liveness. stop() validates the pid before signalling
/// and rejects a stale file.
#[test]
fn status_and_stop_pid_semantics() {
    let dir = tmp_dir("pid");
    let socket_path = dir.join("serve.sock").to_string_lossy().into_owned();

    // No pid file: not running.
    let st = upix_host::serve::status(&socket_path);
    assert_eq!(st, DaemonStatus {
        running: false,
        pid: None,
        socket_present: false,
        socket_path: socket_path.clone(),
    });

    // stop() with no pid file errors out.
    assert!(upix_host::serve::stop(&socket_path).is_err());

    // A pid file naming OUR process fails the upix-daemon validation
    // (cmdline lacks "daemon" naming our test binary context) → stale file
    // removed, error returned.
    std::fs::write(upix_host::serve::pid_file_path(&socket_path), std::process::id().to_string()).unwrap();
    let st = upix_host::serve::status(&socket_path);
    assert!(!st.running, "test binary must not validate as an upix daemon");
    assert!(upix_host::serve::stop(&socket_path).is_err());
    assert!(
        !std::path::Path::new(&upix_host::serve::pid_file_path(&socket_path)).exists(),
        "stale pid file must be removed by stop()"
    );

    // Corrupt pid file → error, no signal.
    std::fs::write(upix_host::serve::pid_file_path(&socket_path), "garbage").unwrap();
    assert!(upix_host::serve::stop(&socket_path).is_err());

    let _ = std::fs::remove_dir_all(dir);
}
