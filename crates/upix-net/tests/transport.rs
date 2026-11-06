//! Transport-level unit tests: recv_poll first-byte timeout semantics (a
//! poll tick must never abandon a partially received frame), TCP + UDS
//! loopback roundtrips, and a full encrypted-frame exchange over a real
//! socket mirroring the coordinator's recv path.

#![cfg(test)]

use std::os::unix::fs::PermissionsExt;
use std::thread;
use std::time::Duration;

use upix_net::frame;
use upix_net::protocol::{MessageHeader, MessageType, HEADER_SIZE, MAGIC};
use upix_net::session::EncryptedSession;
use upix_net::tcp::{TcpServer, TcpSocket};
use upix_net::uds::{UnixServer, UnixSocket};

fn hdr(t: MessageType, len: u32) -> MessageHeader {
    MessageHeader {
        magic: MAGIC,
        msg_type: t,
        payload_length: len,
        checksum: 0,
    }
}

fn listen_free_port(server: &TcpServer) -> u16 {
    for port in 28_000..29_000 {
        if server.listen(port, 10).is_ok() {
            return port;
        }
    }
    panic!("no free port in range");
}

#[test]
fn recv_poll_first_byte_timeout_does_not_lose_half_frames() {
    let server = TcpServer::new();
    let port = listen_free_port(&server);

    let conn = thread::spawn(move || loop {
        match server.accept() {
            Ok(sock) => return sock,
            Err(e) if e.code == libc::EAGAIN => continue,
            Err(e) => panic!("accept failed: {e}"),
        }
    });

    let client = TcpSocket::connect("127.0.0.1", port).expect("connect");
    let peer = conn.join().expect("accept thread");

    // 1) Nothing sent → timeout, zero bytes consumed.
    let mut buf = [0u8; HEADER_SIZE];
    peer.set_recv_timeout(10_000).expect("set full timeout");
    let out = peer.recv_poll(&mut buf, 100).expect("poll ok");
    assert_eq!(out, upix_net::stream::PollFrame::Timeout);

    // 2) Header split across TWO writes with the first chunk arriving only
    //    after the poll window has been entered: the poll must return the
    //    FULL frame once bytes start flowing — never abandon the half frame.
    let payload = b"half-frame-body";
    let wire = frame::serialize_frame(hdr(MessageType::ClientData, payload.len() as u32), payload);
    let sender_client = client.clone();
    let sender = thread::spawn(move || {
        thread::sleep(Duration::from_millis(60)); // enter the poll window first
        sender_client.send_all(&wire[..6]).expect("send part 1");
        thread::sleep(Duration::from_millis(40));
        sender_client.send_all(&wire[6..]).expect("send part 2");
    });

    // Poll window (100ms) shorter than the first-byte delay + full frame read.
    let out = peer.recv_poll(&mut buf, 100).expect("poll completes");
    assert_eq!(out, upix_net::stream::PollFrame::Frame);
    let header = frame::parse_header(&buf).expect("header parses");
    assert_eq!(header.payload_length as usize, payload.len());

    let mut body = vec![0u8; payload.len()];
    peer.recv_exact(&mut body).expect("payload read");
    assert_eq!(body, payload);

    sender.join().expect("sender");
    client.close();
}

#[test]
fn tcp_loopback_send_recv_with_direction_locks() {
    let server = TcpServer::new();
    let port = listen_free_port(&server);

    let accept_thread = thread::spawn(move || loop {
        match server.accept() {
            Ok(sock) => return sock,
            Err(e) if e.code == libc::EAGAIN => continue,
            Err(e) => panic!("accept failed: {e}"),
        }
    });

    let client = TcpSocket::connect("127.0.0.1", port).expect("connect");
    assert_eq!(client.peer_addr_string(), "127.0.0.1");
    assert_eq!(client.peer_port(), port);
    let peer = accept_thread.join().expect("accepted");

    // Bidirectional traffic concurrently: separate rx/tx locks let both run.
    let reader = {
        let peer = peer.clone();
        thread::spawn(move || {
            let mut echo_buf = [0u8; 16];
            peer.recv_exact(&mut echo_buf).expect("recv");
            echo_buf.to_vec()
        })
    };
    client.send_all(b"ping-ping-ping-1").expect("send_all");
    let echoed = reader.join().expect("reader");
    assert_eq!(&echoed, b"ping-ping-ping-1");

    // shutdown: parked reader wakes with ECONNRESET, matching the C++
    // contract (a thread parked in recv() sees EOF).
    peer.shutdown();
    let mut sink = [0u8; 8];
    let result = peer.recv(&mut sink);
    assert!(result.is_err(), "recv after shutdown must fail");

    client.close();
}

#[test]
fn encrypted_session_over_real_tcp_socket() {
    // End-to-end: seal a CLIENT_CONNECT_REQUEST, send it, and on the other
    // side run the coordinator's recv sequence — recv_poll(header) →
    // parse_header → recv_exact(payload) → session.open.
    let server = TcpServer::new();
    let port = listen_free_port(&server);

    let accept_thread = thread::spawn(move || loop {
        match server.accept() {
            Ok(sock) => return sock,
            Err(e) if e.code == libc::EAGAIN => continue,
            Err(e) => panic!("accept failed: {e}"),
        }
    });
    let client = TcpSocket::connect("127.0.0.1", port).expect("connect");
    let peer = accept_thread.join().expect("accepted");

    let key = [0x5Au8; 32];
    let mut session = EncryptedSession::default();
    session.activate(&key);
    let payload = b"Pixel 9 Pro|0123456789ABCDEF";
    let wire = session
        .seal(hdr(MessageType::ClientConnectRequest, payload.len() as u32), payload)
        .expect("seal");
    client.send_all(&wire).expect("send");

    peer.set_recv_timeout(5_000).expect("full timeout");
    let mut buf = [0u8; HEADER_SIZE];
    assert_eq!(
        peer.recv_poll(&mut buf, 1_000).expect("poll"),
        upix_net::stream::PollFrame::Frame
    );
    let header = frame::parse_header(&buf).expect("parses");
    assert!(header.payload_length <= upix_net::tcp::MAX_NETWORK_PAYLOAD);
    let mut rest = vec![0u8; header.payload_length as usize];
    peer.recv_exact(&mut rest).expect("payload");

    let mut full = buf.to_vec();
    full.extend_from_slice(&rest);
    let (hdr2, opened) = session.open(&full).expect("open");
    assert_eq!(hdr2.msg_type, MessageType::ClientConnectRequest);
    assert_eq!(hdr2.payload_length as usize, payload.len());
    assert_eq!(opened, payload);

    client.close();
}

#[test]
fn uds_listen_connect_echo_and_stop_unlinks() {
    let dir = tempfile::tempdir().expect("tempdir");
    let path = dir.path().join("upix-daemon.sock");
    let path_str = path.to_string_lossy().into_owned();

    let server = UnixServer::new();
    server.listen(&path_str, 16).expect("listen");
    assert!(path.exists(), "socket file created");
    let mode = std::fs::metadata(&path).unwrap().permissions().mode();
    assert_eq!(mode & 0o777, 0o600, "UDS socket file must be 0600");

    let accept_thread = thread::spawn(move || loop {
        match server.accept() {
            Ok(sock) => {
                server.stop();
                return sock;
            }
            Err(e) if e.code == libc::EAGAIN => continue,
            Err(e) => panic!("accept failed: {e}"),
        }
    });

    let client = UnixSocket::connect(&path_str).expect("connect");
    let peer = accept_thread.join().expect("accepted");

    client.send_all(b"hello daemon").expect("send");
    let mut buf = [0u8; 12];
    peer.recv_exact(&mut buf).expect("recv");
    assert_eq!(&buf, b"hello daemon");

    client.set_timeout(1_000).expect("set timeout");
    assert!(client.send_all(b"second frame").is_ok());
    peer.close();
    client.close();

    // The accept thread already ran stop(): the socket file is gone, so the
    // next connect gets ECONNREFUSED ("no daemon").
    assert!(!path.exists(), "socket file removed by stop()");
    assert!(UnixSocket::connect(&path_str).is_err());
}

#[test]
fn uds_refuses_to_steal_live_socket_and_replaces_stale() {
    let dir = tempfile::tempdir().expect("tempdir");
    let path = dir.path().join("upix-live.sock");
    let path_str = path.to_string_lossy().into_owned();

    let live = UnixServer::new();
    live.listen(&path_str, 4).expect("live daemon listens");

    // A second bind must NOT steal the live daemon's path.
    let impostor = UnixServer::new();
    let err = impostor.listen(&path_str, 4).expect_err("must refuse");
    assert_eq!(err.code, libc::EADDRINUSE, "live process behind socket");

    // The live daemon still serves (accept queue not stolen); the accept
    // thread owns `live` and stops (unlinks) after the first accept.
    let accept_thread = thread::spawn(move || loop {
        match live.accept() {
            Ok(sock) => {
                live.stop();
                return sock;
            }
            Err(e) if e.code == libc::EAGAIN => continue,
            Err(e) => panic!("accept failed: {e}"),
        }
    });
    let client = UnixSocket::connect(&path_str).expect("live daemon still connectable");
    let _peer = accept_thread.join().expect("live daemon accepted");
    assert!(!path.exists(), "live daemon stopped and unlinked");

    // A non-socket file must never be removed…
    std::fs::write(&path, b"plain user file").expect("plant non-socket file");
    let err = UnixServer::new()
        .listen(&path_str, 4)
        .expect_err("non-socket file must not be removed");
    assert_eq!(err.code, libc::EEXIST, "never delete a user file");
    std::fs::remove_file(&path).expect("clean non-socket file");

    // …while a genuinely STALE socket file (bound-but-closed socket leaves
    // the file behind with nothing live behind it) IS replaced.
    let stale_fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
    assert!(stale_fd >= 0);
    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let path_bytes: Vec<libc::c_char> =
        path_str.bytes().map(|b| b as libc::c_char).collect();
    addr.sun_path[..path_str.len()].copy_from_slice(&path_bytes);
    let bind_len = path_str.len() + std::mem::offset_of!(libc::sockaddr_un, sun_path);
    assert_eq!(unsafe {
        libc::bind(
            stale_fd,
            &addr as *const _ as *const libc::sockaddr,
            bind_len as libc::socklen_t,
        )
    }, 0);
    unsafe { libc::close(stale_fd) }; // "crashed daemon": file stays, nothing listens

    // …but the stale socket file IS replaced by a fresh bind.
    let reborn = UnixServer::new();
    reborn.listen(&path_str, 4).expect("stale socket replaced");
    assert!(path.exists(), "rebound");

    reborn.stop();
    assert!(!path.exists());
    let _ = client;
}
