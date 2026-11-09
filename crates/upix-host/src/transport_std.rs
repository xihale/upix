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
#[cfg(unix)]
use std::os::fd::AsRawFd;
#[cfg(unix)]
use std::os::unix::net::UnixListener as StdUnixListener;
#[cfg(unix)]
use std::os::unix::net::UnixStream;
#[cfg(unix)]
use std::path::PathBuf;
use std::path::Path;
use std::sync::Mutex;
use std::time::Duration;

#[cfg(unix)]
use crate::seams::UdsFactory;
use crate::seams::{Listener, ListenerFactory, RecvErr, Transport};

/// Accept poll tick: bounds how promptly the accept loop notices a stop.
const ACCEPT_POLL_MS: u64 = 500;

/// Wake any thread parked in read/write on `stream` WITHOUT closing the
/// descriptor (a close from another thread would race fd recycling; a
/// shutdown only interrupts). Unix: shutdown(2) on the raw fd. Windows:
/// Winsock shutdown(SD_BOTH) via socket2 (std has no shutdown).
fn shutdown_stream(stream: &TcpStream) {
    #[cfg(unix)]
    {
        let fd = stream.as_raw_fd();
        // SAFETY: shutdown(2) on a live descriptor; see the doc above.
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
    }
    #[cfg(windows)]
    {
        use socket2::SockRef;
        let _ = SockRef::from(stream).shutdown(std::net::Shutdown::Both);
    }
}

/// Listener variant: shutdown(2) on a listening socket wakes pending
/// accept(2)s — the coordinator's prompt-stop path.
fn shutdown_listener(listener: &StdListener) {
    #[cfg(unix)]
    {
        let fd = listener.as_raw_fd();
        // SAFETY: shutdown(2) on a live listening descriptor only wakes
        // pending accepts; see shutdown_stream.
        unsafe {
            libc::shutdown(fd, libc::SHUT_RDWR);
        }
    }
    #[cfg(windows)]
    {
        use socket2::SockRef;
        let _ = SockRef::from(listener).shutdown(std::net::Shutdown::Both);
    }
}

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
        // shutdown() wakes a thread parked in recv(2) — close() from
        // another thread would deadlock on the rx lock.
        shutdown_stream(&self.halves.rx.lock().unwrap());
        shutdown_stream(&self.halves.tx.lock().unwrap());
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
        shutdown_listener(&self.inner);
    }
}


pub struct TcpFactory;

impl ListenerFactory for TcpFactory {
    fn listen(&self, port: u16) -> Result<Box<dyn Listener>, String> {
        let l = StdListener::bind(("0.0.0.0", port)).map_err(|e| format!("bind :{port}: {e}"))?;
        Ok(Box::new(StdTcpListener { inner: l }))
    }
}

// ── UDS (unix-only: Windows std has no UnixListener; the serve daemon is
//    a unix feature — named pipes are future work) ──────────────────

#[cfg(unix)]
mod uds_transport {
    use super::*;

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
}

#[cfg(unix)]
pub use uds_transport::{StdUdsFactory, StdUdsListener, StdUdsSocket};

// ── Windows serve-daemon transport ─────────────────────────────────────────
// The serve daemon's endpoint on Windows is loopback TCP. The protocol,
// framing, lifecycle and pid-file conventions are identical to the unix
// AF_UNIX daemon — only the transport differs. Loopback-only binding never
// triggers the Windows Firewall prompt, and (like the unix socket, whose file
// mode defaults to umask rather than 0600) it is reachable by local processes
// only. One daemon per session: a second `daemon start` fails at bind, the
// same exclusivity the unix socket file provides.
#[cfg(windows)]
pub mod loopback {
    use super::*;
    use std::net::{IpAddr, SocketAddr};

    /// Fixed service port. IANA-unassigned dynamic range. Override with
    /// `UPIX_SERVE_PORT` for parallel daemons (e.g. different model sets).
    pub const DEFAULT_SERVE_PORT: u16 = 47619;

    pub fn serve_port() -> u16 {
        match std::env::var("UPIX_SERVE_PORT") {
            Ok(v) => v.trim().parse().ok().filter(|&p| p != 0).unwrap_or(DEFAULT_SERVE_PORT),
            Err(_) => DEFAULT_SERVE_PORT,
        }
    }

    fn endpoint() -> SocketAddr {
        SocketAddr::new(IpAddr::from([127, 0, 0, 1]), serve_port())
    }

    /// Cheap reachability probe (`daemon status`, delegation preflight).
    /// Short timeout: a refused connect fails instantly; a silently dropping
    /// firewall stack must not stall the caller.
    pub fn probe() -> bool {
        StdTcpSocket::connect(endpoint(), Duration::from_millis(250)).is_ok()
    }

    /// Drop-in [`UdsFactory`] replacement: `socket_path` is kept as the
    /// pid-file base (display/status), the endpoint itself is the port.
    pub struct LoopbackFactory;

    impl crate::seams::UdsFactory for LoopbackFactory {
        fn listen(&self, _socket_path: &Path) -> Result<Box<dyn crate::seams::Listener>, String> {
            let port = serve_port();
            let addr = SocketAddr::new(IpAddr::from([127, 0, 0, 1]), port);
            let l = StdListener::bind(addr)
                .map_err(|e| {
                    if e.kind() == std::io::ErrorKind::AddrInUse {
                        format!(
                            "bind {addr}: address already in use — another daemon (or an unrelated service) holds port {port}; set UPIX_SERVE_PORT to pick a different one"
                        )
                    } else {
                        format!("bind {addr}: {e}")
                    }
                })?;
            Ok(Box::new(StdTcpListener { inner: l }))
        }

        fn connect(&self, _socket_path: &Path) -> Result<Box<dyn crate::seams::Transport>, String> {
            // Same bound as the unix UnixStream::connect: no explicit timeout,
            // refusal is immediate; a firewalled loopback connect cannot
            // happen, so the OS default is fine.
            StdTcpSocket::connect(endpoint(), Duration::from_millis(1000))
                .map(|s| Box::new(s) as Box<dyn crate::seams::Transport>)
        }
    }
}

#[cfg(windows)]
pub use loopback::LoopbackFactory;
