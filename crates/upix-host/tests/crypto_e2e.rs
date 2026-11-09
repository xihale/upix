//! Real-crypto E2E: the production crypto stack (`upix_host::net_adapters`
//! — the same bundle `upix-cli host start` and the GUI use) driven through
//! the full coordinator flow over in-memory transport.
//!
//! `coordinator_flow.rs` proves the host state machines with mock crypto;
//! these tests prove the crypto itself end-to-end: X25519 key agreement,
//! Ed25519 transcript signatures, crypto_secretbox sealed frames
//! (libsodium wire format — golden-locked in `upix-net`), TrustStore
//! persistence, and MAC-failure rejection.

mod common;

use std::sync::atomic::Ordering;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use common::{decode_png, fixed_str, legacy_submit_payload, make_test_png, MockBackend, MockModel, TestClient, TestPipe};
use upix_host::coordinator::{Coordinator, CoordinatorConfig, PendingApproval};
use upix_host::net_adapters::{NetCrypto, NetFramer, NetSigner, NetTrust};
use upix_host::seams::CryptoProvider as _;
use upix_host::Framer;
use upix_host::wireio;
use upix_net::protocol::{ClientConnectRequest, HEADER_SIZE};
use upix_net::{crypto, protocol::MessageType, EncryptedSession, Identity, TrustStore};

fn tmp_dir(tag: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!(
        "upix-crypto-e2e-{tag}-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&d).unwrap();
    d
}

// ── in-memory listener (same harness as coordinator_flow) ──────────────────

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
        let l = Arc::new(PipeListener {
            incoming: std::sync::Mutex::new(std::collections::VecDeque::new()),
            open: std::sync::atomic::AtomicBool::new(true),
            cv: std::sync::Condvar::new(),
        });
        LISTENER.with(|c| *c.lock().unwrap() = Some(Arc::clone(&l)));
        Ok(Box::new(PipeListenerHandle { l }))
    }
}

fn connect_registered() -> Arc<TestClient> {
    let l = LISTENER
        .with(|c| c.lock().unwrap().clone())
        .expect("listener registered");
    let (server_side, client_side) = TestPipe::pair();
    let (client, _pipe) = TestClient::new(client_side);
    l.incoming.lock().unwrap().push_back(Box::new(server_side));
    l.cv.notify_all();
    client
}

// ── server config with the real adapters ───────────────────────────────────

const SERVER_SEED: [u8; 32] = [0x5E; 32];

fn server_config(
    port: u16,
    auto_approve: bool,
    dir: &std::path::Path,
    sink: Option<mpsc::Sender<PendingApproval>>,
) -> Coordinator {
    let backend = {
        MockBackend::install_model_files("Mock-x2", dir);
        Arc::new(MockBackend::new(
            dir.to_path_buf(),
            vec![MockModel {
                name: "Mock-x2".into(),
                scale: 2,
                input_channels: 3,
            }],
            2,
        ))
    };
    let identity = Identity::from_seed(&SERVER_SEED);
    let trust = NetTrust::new(TrustStore::load(&dir.join("trusted_clients")));
    let coord = Coordinator::new(CoordinatorConfig {
        port,
        bind_addr: "127.0.0.1".into(),
        models_dir: dir.to_path_buf(),
        auto_approve,
        backend,
        framer: Arc::new(NetFramer),
        crypto: Arc::new(NetCrypto),
        signer: Arc::new(NetSigner { identity }),
        trust: Some(Arc::new(trust) as Arc<dyn upix_host::seams::TrustApi>),
        listener_factory: Box::new(FactoryRegistry),
    });
    if let Some(sink) = sink {
        coord.set_approval_sink(sink);
    }
    coord
}

// ── real client-side handshake (what the phone does) ───────────────────────

const CLIENT_SEED: [u8; 32] = [0xA7; 32];

struct RealClient {
    client: Arc<TestClient>,
    session: EncryptedSession,
}

/// KEY_HELLO → server KEY_HELLO (verify server Ed25519 sig over the full
/// transcript) → derive session key via X25519 → AUTH_PROOF (sealed) →
/// AUTH_RESPONSE (sealed, expect authenticated=1). Returns the sealed-session
/// client ready for CONNECT / task traffic.
fn real_handshake(client: &Arc<TestClient>, id: &Identity) -> Result<RealClient, String> {
    let id_pub = id.public_key();
    let (c_eph, c_priv) = crypto::x25519_keygen();  // (public, private)

    // Client KEY_HELLO: sig covers its own half (server half zeroed).
    let mut partial = [0u8; 128];
    partial[0..32].copy_from_slice(&c_eph);
    partial[32..64].copy_from_slice(&id_pub);
    let hello = wireio::enc_key_hello(&upix_net::protocol::KeyHelloMessage {
        eph_pub: c_eph,
        id_pub,
        sig: id.sign(&partial),
    });
    client.send(MessageType::KeyHello, &hello);

    // Server KEY_HELLO (plaintext) — verify the server's signature.
    let (hdr, payload) = client.recv_frame(3000).ok_or("no server KEY_HELLO")?;
    assert_eq!(hdr.msg_type, MessageType::KeyHello);
    let s_hello = wireio::dec_key_hello(&payload).ok_or("bad server KEY_HELLO")?;
    let mut transcript = [0u8; 128];
    transcript[0..32].copy_from_slice(&c_eph);
    transcript[32..64].copy_from_slice(&id_pub);
    transcript[64..96].copy_from_slice(&s_hello.eph_pub);
    transcript[96..128].copy_from_slice(&s_hello.id_pub);
    assert!(
        crypto::ed25519_verify(&s_hello.id_pub, &s_hello.sig, &transcript),
        "server KEY_HELLO signature invalid"
    );

    // Session key: X25519(our eph priv, server eph pub).
    let session_key = crypto::x25519_shared(&c_priv, &s_hello.eph_pub).expect("x25519");
    let mut session = EncryptedSession::default();
    session.activate(&session_key);

    // AUTH_PROOF (sealed): Ed25519 sig over the full transcript.
    let proof = wireio::enc_auth_proof(&id.sign(&transcript));
    send_sealed(client, &session, MessageType::AuthProof, &proof);

    // AUTH_RESPONSE (sealed).
    let (hdr, payload) = open_sealed(client, &session, 3000).ok_or("no AUTH_RESPONSE")?;
    assert_eq!(hdr.msg_type, MessageType::AuthResponse);
    let resp = wireio::dec_auth_response(&payload).ok_or("bad AUTH_RESPONSE")?;
    if resp.authenticated != 1 {
        return Err("client not authenticated".into());
    }
    Ok(RealClient { client: Arc::clone(client), session })
}

fn send_sealed(client: &TestClient, session: &EncryptedSession, ty: MessageType, payload: &[u8]) {
    let wire = session
        .seal(
            upix_net::protocol::MessageHeader {
                magic: upix_net::protocol::MAGIC,
                msg_type: ty,
                payload_length: payload.len() as u32,
                checksum: 0,
            },
            payload,
        )
        .unwrap();
    client.pipe.write(&wire);
}

/// Receive one framed message and open it with the session. Returns
/// `(plaintext header, plaintext payload)`; `None` on timeout/MAC failure.
fn open_sealed(
    client: &TestClient,
    session: &EncryptedSession,
    timeout_ms: i64,
) -> Option<(upix_net::protocol::MessageHeader, Vec<u8>)> {
    // Sealed frames keep their true length in the header's payload_length
    // (nonce + ciphertext + MAC), so recv_frame reads the full body.
    let (hdr, body) = client.recv_frame(timeout_ms)?;
    let mut wire = Vec::with_capacity(HEADER_SIZE + body.len());
    wire.extend_from_slice(&hdr.magic.to_le_bytes());
    wire.extend_from_slice(&(hdr.msg_type as u16).to_le_bytes());
    wire.extend_from_slice(&hdr.payload_length.to_le_bytes());
    wire.extend_from_slice(&hdr.checksum.to_le_bytes());
    wire.extend_from_slice(&body);
    session.open(&wire)
}

// ── tests ──────────────────────────────────────────────────────────────────

/// Full happy path with real crypto: handshake, pairing, legacy submit of a
/// small PNG → progress + success response, everything sealed; also proves
/// sealed wire bytes differ from the plaintext frame.
#[test]
fn full_task_over_real_crypto() {
    let dir = tmp_dir("happy");
    let mut coord = server_config(8911, true, &dir, None);
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let id = Identity::from_seed(&CLIENT_SEED);
    let client_arc = connect_registered();
    let rc = real_handshake(&client_arc, &id).expect("handshake");
    let RealClient { client, session } = &rc;

    // CONNECT_REQUEST (sealed).
    let creq = ClientConnectRequest {
        device_name: fixed_str::<64>("crypto-e2e-phone"),
        fingerprint: fixed_str::<17>("cafef00dcafef00d"),
    };
    send_sealed(client, session, MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq));
    let (hdr, payload) = open_sealed(client, session, 3000).expect("no CONNECT_RESPONSE");
    assert_eq!(hdr.msg_type, MessageType::ClientConnectResponse);
    let resp = wireio::dec_connect_response(&payload).expect("bad CONNECT_RESPONSE");
    assert_eq!(resp.approved, 1);

    // Sealed wire differs from the plaintext frame for the same payload.
    let plain = NetFramer.serialize_frame(MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq));
    let sealed = session
        .seal(
            upix_net::protocol::MessageHeader {
                magic: upix_net::protocol::MAGIC,
                msg_type: MessageType::ClientConnectRequest,
                payload_length: wireio::enc_connect_request(&creq).len() as u32,
                checksum: 0,
            },
            &wireio::enc_connect_request(&creq),
        )
        .unwrap();
    assert_ne!(&plain[HEADER_SIZE..], &sealed[HEADER_SIZE..], "ciphertext must differ from plaintext");
    // magic + msg_type stay stable; sealed rewrites payload_length (nonce+ct+MAC)
    // and zeroes the checksum (Poly1305 replaces it) — see EncryptedSession::seal.
    assert_eq!(plain[..6], sealed[..6], "framing magic/type must stay stable");

    // Legacy single-frame submit of a small PNG → task runs → success.
    let png = make_test_png(8, 4);
    let payload = legacy_submit_payload("Mock-x2", "png", "png", &png);
    send_sealed(client, session, MessageType::ClientSubmit, &payload);

    let mut response = None;
    for _ in 0..20 {
        match open_sealed(client, session, 3000) {
            Some((h, p)) => {
                if h.msg_type == MessageType::ClientResponse {
                    response = Some(p);
                    break;
                }
            }
            None => break,
        }
    }
    let p = response.expect("no CLIENT_RESPONSE");
    let resp = wireio::dec_task_response(&p).expect("bad response");
    assert_eq!(resp.success, 1, "task failed: {:?}", String::from_utf8_lossy(&resp.error_message));
    let result_bytes = &p[wireio::CLIENT_TASK_RESPONSE_SIZE..];
    let (w, h, c, _) = decode_png(result_bytes).expect("result not a PNG");
    assert_eq!((w, h, c), (16, 8, 3));

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// A tampered AUTH_PROOF (bit flip in transit) must fail MAC verification:
/// server answers authenticated=0 and aborts the connection.
#[test]
fn tampered_proof_is_rejected() {
    let dir = tmp_dir("tamper");
    let mut coord = server_config(8912, true, &dir, None);
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let id = Identity::from_seed(&CLIENT_SEED);
    let client_arc = connect_registered();
    let id_pub = id.public_key();
    let (c_eph, c_priv) = crypto::x25519_keygen();  // (public, private)

    let mut partial = [0u8; 128];
    partial[0..32].copy_from_slice(&c_eph);
    partial[32..64].copy_from_slice(&id_pub);
    let hello = wireio::enc_key_hello(&upix_net::protocol::KeyHelloMessage {
        eph_pub: c_eph,
        id_pub,
        sig: id.sign(&partial),
    });
    client_arc.send(MessageType::KeyHello, &hello);

    let (_, payload) = client_arc.recv_frame(3000).expect("no server KEY_HELLO");
    let s_hello = wireio::dec_key_hello(&payload).expect("bad server KEY_HELLO");
    let session_key = crypto::x25519_shared(&c_priv, &s_hello.eph_pub).expect("x25519");
    let mut session = EncryptedSession::default();
    session.activate(&session_key);

    // Build a valid AUTH_PROOF, then flip one ciphertext byte in transit.
    let mut transcript = [0u8; 128];
    transcript[0..32].copy_from_slice(&c_eph);
    transcript[32..64].copy_from_slice(&id_pub);
    transcript[64..96].copy_from_slice(&s_hello.eph_pub);
    transcript[96..128].copy_from_slice(&s_hello.id_pub);
    let proof = wireio::enc_auth_proof(&id.sign(&transcript));
    let mut wire = session
        .seal(
            upix_net::protocol::MessageHeader {
                magic: upix_net::protocol::MAGIC,
                msg_type: MessageType::AuthProof,
                payload_length: proof.len() as u32,
                checksum: 0,
            },
            &proof,
        )
        .unwrap();
    let last = wire.len() - 1;
    wire[last] ^= 0x01;
    client_arc.pipe.write(&wire);

    // Server must reject: AUTH_RESPONSE with authenticated=0 (MAC failure
    // path answers None-with-drop instead; both count as rejection).
    let rejected = match client_arc.recv_frame(3000) {
        None => true, // connection dropped without a reply
        Some((h, body)) => {
            assert_eq!(h.msg_type, MessageType::AuthResponse);
            let mut wire2 = Vec::with_capacity(HEADER_SIZE + body.len());
            wire2.extend_from_slice(&h.magic.to_le_bytes());
            wire2.extend_from_slice(&(h.msg_type as u16).to_le_bytes());
            wire2.extend_from_slice(&h.payload_length.to_le_bytes());
            wire2.extend_from_slice(&h.checksum.to_le_bytes());
            wire2.extend_from_slice(&body);
            match session.open(&wire2) {
                Some((_, p)) => {
                    let resp = wireio::dec_auth_response(&p).unwrap();
                    resp.authenticated == 0
                }
                // The corrupted proof changed nothing on the reply path…
                None => false,
            }
        }
    };
    assert!(rejected, "tampered AUTH_PROOF was accepted");

    // And the connection is dead: no reply to a fresh CONNECT_REQUEST.
    let creq = ClientConnectRequest {
        device_name: fixed_str::<64>("should-not-pair"),
        fingerprint: fixed_str::<17>("0000000000000000"),
    };
    send_sealed(&client_arc, &session, MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq));
    assert!(
        client_arc.recv_frame(1000).is_none(),
        "aborted session must not answer further frames"
    );

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// Untrusted client with real TrustStore: approval request → deny → approved=0
/// and nothing persisted; second connection → approve via TrustApi →
/// approved=1/trusted=1; third connection auto-approves, and the trust file
/// on disk contains the key.
#[test]
fn trust_store_persists_real_approvals() {
    let dir = tmp_dir("trust");
    let trust_path = dir.join("trusted_clients");
    let (tx, rx) = mpsc::channel();
    let mut coord = server_config(8913, false, &dir, Some(tx));
    coord.start().expect("start");
    std::thread::sleep(Duration::from_millis(50));

    let id = Identity::from_seed(&CLIENT_SEED);
    let id_pub = id.public_key();

    // Connection 1: pending approval, deny it.
    let client_arc = connect_registered();
    let rc = real_handshake(&client_arc, &id).expect("handshake 1");
    let creq = ClientConnectRequest {
        device_name: fixed_str::<64>("trust-e2e-phone"),
        fingerprint: fixed_str::<17>("0011223344556677"),
    };
    send_sealed(&rc.client, &rc.session, MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq));
    let req = rx.recv_timeout(Duration::from_secs(3)).expect("no approval event");
    assert_eq!(req.device_name, "trust-e2e-phone");
    coord.resolve_approval(req.id, false);
    let (_, p) = open_sealed(&rc.client, &rc.session, 3000).expect("no CONNECT_RESPONSE");
    let resp = wireio::dec_connect_response(&p).unwrap();
    assert_eq!(resp.approved, 0);
    assert!(!TrustStore::load(&trust_path).contains(&id_pub), "denied peer must not persist");

    // Connection 2: fresh handshake + session, approve it.
    let client2 = connect_registered();
    let rc2 = real_handshake(&client2, &id).expect("handshake 2");
    send_sealed(&rc2.client, &rc2.session, MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq));
    let req = rx.recv_timeout(Duration::from_secs(3)).expect("no approval event 2");
    coord.resolve_approval(req.id, true);
    let (_, p) = open_sealed(&rc2.client, &rc2.session, 3000).expect("no CONNECT_RESPONSE 2");
    let resp = wireio::dec_connect_response(&p).unwrap();
    assert_eq!(resp.approved, 1);
    assert!(TrustStore::load(&trust_path).contains(&id_pub), "approval must persist to disk");

    // Connection 3: trusted peer auto-approves (no new approval event).
    let client3 = connect_registered();
    let rc3 = real_handshake(&client3, &id).expect("handshake 3");
    send_sealed(&rc3.client, &rc3.session, MessageType::ClientConnectRequest, &wireio::enc_connect_request(&creq));
    let (_, p) = open_sealed(&rc3.client, &rc3.session, 3000).expect("no CONNECT_RESPONSE 3");
    let resp = wireio::dec_connect_response(&p).unwrap();
    assert_eq!(resp.approved, 1);
    assert_eq!(resp.trusted, 1);
    assert!(rx.try_recv().is_err(), "trusted peer must not enqueue a new approval");

    coord.stop();
    let _ = std::fs::remove_dir_all(dir);
}

/// Keep the import honest: NetCodec is what NetCrypto::new_session returns
/// (exercised by every sealed frame above); assert the type round-trips and
/// that the body on the wire is not the plaintext.
#[test]
fn net_codec_roundtrip() {
    let key = [0x42u8; 32];
    let codec = NetCrypto.new_session(&key);
    let payload = b"roundtrip";
    let sealed = codec.seal(MessageType::AuthProof, payload).expect("seal");
    assert_eq!(sealed.len(), HEADER_SIZE + 24 + payload.len() + 16);
    assert_ne!(&sealed[HEADER_SIZE..][..payload.len()], &payload[..]);
    let (hdr, opened) = codec.open(&sealed).expect("open");
    assert_eq!(hdr.msg_type, MessageType::AuthProof);
    assert_eq!(opened, payload);
}
