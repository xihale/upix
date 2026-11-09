//! std-based TCP/UDS transport adapters implementing [`crate::seams`]
//! traits. These mirror `engine/utils/{network,stream_socket,unix_socket}`
//! semantics: send/receive under SEPARATE locks (a handler's polled reads and
//! a worker's sends share one connection without deadlocking), poll-tick
//! accept, and `shutdown()` wake for teardown.
//!
//! When upix-net's `tcp`/`uds` modules land, the pinned implementations can
//! replace these; until then this is the real transport (not a mock) — same
//! syscalls, same locking discipline.

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener as StdListener, TcpStream, ToSocketAddrs};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixListener as StdUnixListener;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

use crate::seams::{Listener, ListenerFactory, RecvErr, Transport, UdsFactory};

/// Accept poll tick: bounds how promptly the accept loop notices a stop.
const ACCEPT_POLL_MS: u64 = 500;

fn io_msg(e: &std::io::Error) -> String {
    match e.kind() {
        std::io::ErrorKind::WouldBlock => "EAGAIN".to_string(),
        _ => format!("{e}"),
    }
}

fn is_would_block(e: &std::io::Error) -> bool {
    matches!(
        e.kind(),
        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut | std::io::ErrorKind::Interrupted
    )
}

// ── TCP ─────────────────────────────────────────────────────────────────────

/// Half-split socket: rx and tx own separate `try_clone` fds, each behind its
/// own mutex — the exact StreamSocket discipline (half-send/half-receive must
/// not deadlock; workers send while the handler reads).
struct TcpHalves {
    rx: Mutex<TcpStream>,
    tx: Mutex<TcpStream>,
}

pub struct StdTcpSocket {
    halves: TcpHalves,
    peer: SocketAddr,
    rx_timeout_ms: Mutex<i64>,
}

impl StdTcpSocket {
    pub fn new(stream: TcpStream, peer: SocketAddr) -> Result<Self, String> {
        let rx = stream.try_clone().map_err(|e| format!("tcp fd clone: {e}"))?;
        let tx = stream;
        Ok(Self {
            halves: TcpHalves {
                rx: Mutex::new(rx),
                tx: Mutex::new(tx),
            },
            peer,
            rx_timeout_ms: Mutex::new(0),
        })
    }

    pub fn peer_addr(&self) -> SocketAddr {
        self.peer
    }

    pub fn connect(addr: SocketAddr, timeout: Duration) -> Result<Self, String> {
        let stream = TcpStream::connect_timeout(&addr, timeout)
            .map_err(|e| format!("connect {addr}: {}", io_msg(&e)))?;
        Self::new(stream, addr)
    }

    pub fn connect_resolving(host: &str, port: u16, timeout: Duration) -> Result<Self, String> {
        let addrs: Vec<SocketAddr> = (host, port)
            .to_socket_addrs()
            .map_err(|e| e.to_string())?
            .collect();
        let mut last_err: Option<std::io::Error> = None;
        for a in addrs {
            match TcpStream::connect_timeout(&a, timeout) {
                Ok(s) => return Self::new(s, a),
                Err(e) => last_err = Some(e),
            }
        }
        Err(match last_err {
            Some(e) => format!("connect {host}:{port}: {e}"),
            None => format!("connect {host}:{port}: no addresses"),
        })
    }
}

impl Transport for StdTcpSocket {
    fn send_all(&self, buf: &[u8]) -> Result<(), String> {
        let mut tx = self.halves.tx.lock().map_err(|_| "tx mutex poisoned".to_string())?;
        let mut sent = 0;
        while sent < buf.len() {
            match tx.write(&buf[sent..]) {
                Ok(0) => return Err("peer closed (send)".into()),
                Ok(n) => sent += n,
                Err(e) if is_would_block(&e) => continue,
                Err(e) => return Err(format!("send: {}", io_msg(&e))),
            }
        }
        Ok(())
    }

    fn set_recv_timeout_ms(&self, ms: i64) {
        *self.rx_timeout_ms.lock().unwrap() = ms;
        let rx = self.halves.rx.lock().unwrap();
        let d = if ms <= 0 {
            None
        } else {
            Some(Duration::from_millis(ms as u64))
        };
        let _ = rx.set_read_timeout(d);
    }

    fn receive(&self, out: &mut [u8]) -> Result<usize, RecvErr> {
        let mut rx = self.halves.rx.lock().map_err(|_| RecvErr::Dead("rx mutex poisoned".into()))?;
        match rx.read(out) {
            Ok(n) => Ok(n),
            Err(e) if is_would_block(&e) => Err(RecvErr::WouldBlock),
            Err(e) => Err(RecvErr::Dead(format!("recv: {}", io_msg(&e)))),
        }
    }

    fn shutdown(&self) {
        // libc::shutdown wakes a thread parked in recv(2) — close() from
        // another thread would deadlock on the rx lock.
        let fd = self.halves.rx.lock().unwrap().as_raw_fd();
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
        let fd = self.halves.tx.lock().unwrap().as_raw_fd();
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
    }
}

pub struct StdTcpListener {
    inner: StdListener,
}

impl Listener for StdTcpListener {
    fn accept(&self) -> Result<Option<Box<dyn Transport>>, String> {
        let _ = self.inner.set_nonblocking(false);
        // Poll tick via SO_RCVTIMEO on accept.
        let _ = self.inner.set_nonblocking(true);
        loop {
            match self.inner.accept() {
                Ok((s, a)) => {
                    let _ = s.set_nonblocking(false);
                    return StdTcpSocket::new(s, a)
                        .map(|s| Box::new(s) as Box<dyn Transport>)
                        .map(Some);
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(ACCEPT_POLL_MS));
                    return Ok(None);
                }
                Err(e) => return Err(io_msg(&e)),
            }
        }
    }

    fn shutdown(&self) {
        let fd = self.inner.as_raw_fd();
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
    }
}


pub struct TcpFactory;

impl ListenerFactory for TcpFactory {
    fn listen(&self, port: u16) -> Result<Box<dyn Listener>, String> {
        let l = StdListener::bind(("0.0.0.0", port)).map_err(|e| format!("bind :{port}: {e}"))?;
        Ok(Box::new(StdTcpListener { inner: l }))
    }
}

// ── UDS ─────────────────────────────────────────────────────────────────────

pub struct StdUdsSocket {
    halves: TcpLikeHalves,
    rx_timeout_ms: Mutex<i64>,
}

struct TcpLikeHalves {
    rx: Mutex<UnixStream>,
    tx: Mutex<UnixStream>,
}

impl StdUdsSocket {
    pub fn new(stream: UnixStream) -> Result<Self, String> {
        let rx = stream.try_clone().map_err(|e| format!("uds fd clone: {e}"))?;
        Ok(Self {
            halves: TcpLikeHalves {
                rx: Mutex::new(rx),
                tx: Mutex::new(stream),
            },
            rx_timeout_ms: Mutex::new(0),
        })
    }
}

impl Transport for StdUdsSocket {
    fn send_all(&self, buf: &[u8]) -> Result<(), String> {
        let mut tx = self.halves.tx.lock().map_err(|_| "tx mutex poisoned".to_string())?;
        let mut sent = 0;
        while sent < buf.len() {
            match tx.write(&buf[sent..]) {
                Ok(0) => return Err("peer closed (send)".into()),
                Ok(n) => sent += n,
                Err(e) if is_would_block(&e) => continue,
                Err(e) => return Err(format!("send: {}", io_msg(&e))),
            }
        }
        Ok(())
    }

    fn set_recv_timeout_ms(&self, ms: i64) {
        *self.rx_timeout_ms.lock().unwrap() = ms;
        let rx = self.halves.rx.lock().unwrap();
        let d = if ms <= 0 {
            None
        } else {
            Some(Duration::from_millis(ms as u64))
        };
        let _ = rx.set_read_timeout(d);
    }

    fn receive(&self, out: &mut [u8]) -> Result<usize, RecvErr> {
        let mut rx = self.halves.rx.lock().map_err(|_| RecvErr::Dead("rx mutex poisoned".into()))?;
        match rx.read(out) {
            Ok(n) => Ok(n),
            Err(e) if is_would_block(&e) => Err(RecvErr::WouldBlock),
            Err(e) => Err(RecvErr::Dead(format!("recv: {}", io_msg(&e)))),
        }
    }

    fn shutdown(&self) {
        let fd = self.halves.rx.lock().unwrap().as_raw_fd();
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
        let fd = self.halves.tx.lock().unwrap().as_raw_fd();
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
    }
}

pub struct StdUdsListener {
    inner: StdUnixListener,
    path: PathBuf,
}

impl Listener for StdUdsListener {
    fn accept(&self) -> Result<Option<Box<dyn Transport>>, String> {
        let _ = self.inner.set_nonblocking(true);
        match self.inner.accept() {
            Ok((s, _)) => {
                let _ = s.set_nonblocking(false);
                StdUdsSocket::new(s)
                    .map(|s| Box::new(s) as Box<dyn Transport>)
                    .map(Some)
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(ACCEPT_POLL_MS));
                Ok(None)
            }
            Err(e) => Err(io_msg(&e)),
        }
    }

    fn shutdown(&self) {
        // Closing the listener wakes accept; unlink the socket file (the C++
        // UnixServer destructor does the same).
        let fd = self.inner.as_raw_fd();
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
        let _ = std::fs::remove_file(&self.path);
    }
}

impl StdUdsListener {
    pub fn socket_path(&self) -> &Path {
        &self.path
    }
}

pub struct StdUdsFactory;

impl UdsFactory for StdUdsFactory {
    fn listen(&self, socket_path: &Path) -> Result<Box<dyn Listener>, String> {
        if let Some(parent) = socket_path.parent() {
            if !parent.as_os_str().is_empty() {
                let _ = std::fs::create_dir_all(parent);
            }
        }
        // A stale socket file from a crashed daemon would fail bind; the C++
        // UnixServer unlinks before binding.
        let _ = std::fs::remove_file(socket_path);
        let l = StdUnixListener::bind(socket_path).map_err(|e| format!("bind {socket_path:?}: {e}"))?;
        Ok(Box::new(StdUdsListener { inner: l, path: socket_path.to_path_buf() }))
    }

    fn connect(&self, socket_path: &Path) -> Result<Box<dyn Transport>, String> {
        let s = UnixStream::connect(socket_path).map_err(|e| format!("connect {socket_path:?}: {e}"))?;
        let sock = StdUdsSocket::new(s)?;
        Ok(Box::new(sock))
    }
}
