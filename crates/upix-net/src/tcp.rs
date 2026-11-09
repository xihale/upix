//! TCP transport — Rust port of `engine/utils/network.{hpp,cpp}`.
//!
//! Framing I/O (send/recv and their `_all` variants) plus the separate
//! rx/tx locks live in `crate::stream::StreamCore` (stream_socket.cpp port).

use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, ToSocketAddrs};
#[cfg(unix)]
use std::os::unix::io::AsRawFd;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::error::NetError;
use crate::stream::{PollFrame, StreamCore};

/// Maximum payload size for network messages (100 MB).
pub const MAX_NETWORK_PAYLOAD: u32 = 100 * 1024 * 1024;

/// Aggressive TCP keepalive so long-lived request/response connections
/// survive NAT / stateful firewall idle eviction during the long,
/// traffic-free processing phase. Without this, a NAT that drops an idle
/// connection (~30-60s on many home/Wi-Fi routers) silently kills the link
/// while the PC is still upscaling.
///   idle=10s before first probe (under typical NAT timeouts), intvl=3s, count=5
/// Plus TCP_NODELAY and the default 1MB socket buffers, exactly mirroring
/// `apply_tcp_keepalive` + the connect-path setsockopt calls in network.cpp.
#[cfg(unix)]
fn apply_stream_sockopts(stream: &TcpStream) {
    let fd = stream.as_raw_fd();
    let flag: libc::c_int = 1;
    unsafe {
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_NODELAY,
            &flag as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        let buf_size: libc::c_int = 1024 * 1024;
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_SNDBUF,
            &buf_size as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_RCVBUF,
            &buf_size as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_KEEPALIVE,
            &flag as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        // First-probe idle time: macOS names the option TCP_KEEPALIVE (no
        // TCP_KEEPIDLE); Linux/BSD use TCP_KEEPIDLE.
        let idle: libc::c_int = 10;
        #[cfg(target_os = "macos")]
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_KEEPALIVE,
            &idle as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        #[cfg(not(target_os = "macos"))]
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_KEEPIDLE,
            &idle as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        let interval: libc::c_int = 3;
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_KEEPINTVL,
            &interval as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
        let count: libc::c_int = 5;
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_KEEPCNT,
            &count as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
    }
}

/// Windows variant of the unix sockopts above: same NAT-survival goal.
/// Windows has no TCP_KEEPIDLE family; SIO_KEEPALIVE_VALS (via socket2's
/// TcpKeepalive) carries the idle/interval pair, probe count is OS-fixed.
#[cfg(windows)]
fn apply_stream_sockopts(stream: &TcpStream) {
    use std::time::Duration;

    use socket2::{SockRef, TcpKeepalive};

    let sock = SockRef::from(stream);
    let _ = sock.set_tcp_nodelay(true);
    let _ = sock.set_send_buffer_size(1024 * 1024);
    let _ = sock.set_recv_buffer_size(1024 * 1024);
    let mut ka = TcpKeepalive::new();
    ka = ka.with_time(Duration::from_secs(10));
    ka = ka.with_interval(Duration::from_secs(3));
    let _ = sock.set_tcp_keepalive(&ka);
}

/// Basic TCP socket wrapper (mirrors `TCPSocket`).
pub struct TcpSocket {
    core: Arc<StreamCore<TcpStream>>,
    peer_address: Mutex<String>,
    peer_port: Mutex<u16>,
}

impl Clone for TcpSocket {
    fn clone(&self) -> Self {
        Self {
            core: Arc::clone(&self.core),
            peer_address: Mutex::new(self.peer_addr_string()),
            peer_port: Mutex::new(self.peer_port()),
        }
    }
}

impl TcpSocket {
    /// Blocking IPv4 connect (mirrors `TCPSocket::connect`: inet_pton
    /// AF_INET only — use `connect_with_timeout` for IPv6/DNS names).
    pub fn connect(host: &str, port: u16) -> Result<Self, NetError> {
        let Ok(addr) = host.parse::<Ipv4Addr>() else {
            return Err(NetError::new(libc::EINVAL, "Invalid address"));
        };
        let stream = TcpStream::connect(SocketAddr::new(IpAddr::V4(addr), port))
            .map_err(|e| NetError::from_io("Connection failed", &e))?;
        apply_stream_sockopts(&stream);
        Ok(Self {
            core: Arc::new(StreamCore::new(stream)),
            peer_address: Mutex::new(host.to_string()),
            peer_port: Mutex::new(port),
        })
    }

    /// Connect with a bounded timeout (mirrors `connect_with_timeout`):
    /// resolution is AF_UNSPEC-ish via `to_socket_addrs` (getaddrinfo), so
    /// IPv6 literals and hostnames work, and every candidate address is
    /// tried until ONE deadline shared by all of them (a host with many
    /// A/AAAA records cannot multiply the timeout). Mirrors the C++ bool
    /// return: failure carries no per-candidate errno detail — callers log
    /// host:port.
    pub fn connect_with_timeout(host: &str, port: u16, timeout_ms: u64) -> Result<Self, NetError> {
        let addrs: Vec<SocketAddr> = (host, port)
            .to_socket_addrs()
            .map_err(|_| NetError::new(resolution_failed_code(), "Resolution failed"))?
            .collect();
        if addrs.is_empty() {
            return Err(NetError::new(resolution_failed_code(), "Resolution failed"));
        }

        let deadline = std::time::Instant::now() + Duration::from_millis(timeout_ms);
        let mut last: Option<NetError> = None;
        for addr in addrs {
            let remaining = deadline.checked_duration_since(std::time::Instant::now());
            let Some(remaining) = remaining else { break };
            match TcpStream::connect_timeout(&addr, remaining) {
                Ok(stream) => {
                    apply_stream_sockopts(&stream);
                    return Ok(Self {
                        core: Arc::new(StreamCore::new(stream)),
                        peer_address: Mutex::new(host.to_string()),
                        peer_port: Mutex::new(port),
                    });
                }
                Err(e) => last = Some(NetError::from_io("connect", &e)),
            }
        }
        Err(last.unwrap_or_else(|| NetError::eagain("Connect timed out")))
    }

    /// Set BOTH timeouts (SO_RCVTIMEO + SO_SNDTIMEO), mirroring
    /// `set_timeout`.
    pub fn set_timeout(&self, timeout_ms: u64) -> Result<(), NetError> {
        self.core.set_timeout(Some(Duration::from_millis(timeout_ms)))
    }

    /// Set ONLY the receive timeout (mirrors `set_recv_timeout`): used by
    /// receive-poll loops that multiplex one connection between a reader and
    /// a sender, so shrinking the receive window never aborts concurrent
    /// big sends.
    pub fn set_recv_timeout(&self, timeout_ms: u64) -> Result<(), NetError> {
        self.core
            .set_recv_timeout(Some(Duration::from_millis(timeout_ms)))
    }

    pub fn timeout(&self) -> Option<Duration> {
        self.core.sock().read_timeout().ok().flatten()
    }

    /// One send syscall (mirrors `TCPSocket::send` through StreamSocket).
    pub fn send(&self, data: &[u8]) -> Result<usize, NetError> {
        self.core.send(data)
    }

    /// Send everything (tx lock held across the whole call).
    pub fn send_all(&self, data: &[u8]) -> Result<(), NetError> {
        self.core.send_all(data)
    }

    /// One recv syscall (rx lock held; EOF → ECONNRESET like the C++).
    pub fn recv(&self, buf: &mut [u8]) -> Result<usize, NetError> {
        self.core.recv(buf)
    }

    /// Receive exactly `buf.len()` bytes (rx lock held across the call).
    pub fn recv_exact(&self, buf: &mut [u8]) -> Result<(), NetError> {
        self.core.recv_exact(buf)
    }

    /// One recv bounded by a temporary receive-timeout override.
    pub fn recv_timeout(&self, buf: &mut [u8], timeout_ms: u64) -> Result<usize, NetError> {
        self.core.recv_timeout(buf, Duration::from_millis(timeout_ms))
    }

    /// Poll window for the FIRST byte only; once any byte of the frame is
    /// in, the rest of `buf` is read with the socket's configured receive
    /// timeout (see `crate::stream::StreamCore::recv_poll`). A poll tick
    /// never abandons a partially received frame while the peer is merely
    /// slow — that would corrupt the stream framing.
    pub fn recv_poll(&self, buf: &mut [u8], first_byte_timeout_ms: u64) -> Result<PollFrame, NetError> {
        self.core
            .recv_poll(buf, Duration::from_millis(first_byte_timeout_ms))
    }

    /// Announce shutdown in both directions without releasing the
    /// descriptor: parked readers see EOF, parked senders fail — this is
    /// what lets a shutdown path wake a session handler whose peer went
    /// silent. Lock-free w.r.t. the I/O locks (see StreamCore::shutdown for
    /// the lifetime contract).
    pub fn shutdown(&self) {
        self.core.shutdown();
    }

    /// Release the descriptor: dropping the last clone of this socket
    /// closes the fd (mirrors `TCPSocket::close` / the destructor).
    pub fn close(&self) {
        // The descriptor is owned by the Arc<StreamCore>; explicit close is
        // expressed by dropping all clones. shutdown() covers the wake-up
        // needs of concurrent owners.
        self.core.shutdown();
    }

    pub fn peer_addr_string(&self) -> String {
        self.peer_address
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }

    pub fn peer_port(&self) -> u16 {
        *self.peer_port.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// Errno-style code for getaddrinfo failure. libc-windows has no
/// EAI_* constants; EINVAL carries the message through unchanged.
#[cfg(windows)]
fn resolution_failed_code() -> i32 {
    libc::EINVAL
}

#[cfg(unix)]
fn resolution_failed_code() -> i32 {
    libc::EAI_NONAME
}

/// TCP server (mirrors `TCPServer`).
pub struct TcpServer {
    listener: Mutex<Option<TcpListener>>,
    port: Mutex<u16>,
}

impl Default for TcpServer {
    fn default() -> Self {
        Self::new()
    }
}

impl TcpServer {
    pub fn new() -> Self {
        Self {
            listener: Mutex::new(None),
            port: Mutex::new(0),
        }
    }

    /// Start listening (mirrors `TCPServer::listen`: SO_REUSEADDR, INADDR_ANY,
    /// caller-chosen backlog).
    #[cfg(unix)]
    pub fn listen(&self, port: u16, backlog: i32) -> Result<(), NetError> {
        self.stop();

        // SAFETY: plain socket/bind/listen sequence, mirroring the C++ so the
        // backlog (default 10) and SO_REUSEADDR semantics stay identical.
        let fd = unsafe { libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0) };
        if fd < 0 {
            return Err(NetError::from_io(
                "Failed to create socket",
                &std::io::Error::last_os_error(),
            ));
        }
        let opt: libc::c_int = 1;
        unsafe {
            libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_REUSEADDR,
                &opt as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            );
        }

        let addr = libc::sockaddr_in {
            #[cfg(target_os = "macos")]
            sin_len: std::mem::size_of::<libc::sockaddr_in>() as u8,
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr {
                s_addr: libc::INADDR_ANY,
            },
            sin_zero: [0; 8],
        };
        let bind_rc = unsafe {
            libc::bind(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t,
            )
        };
        if bind_rc < 0 {
            let e = std::io::Error::last_os_error();
            unsafe { libc::close(fd) };
            return Err(NetError::from_io("Bind failed", &e));
        }
        if unsafe { libc::listen(fd, backlog) } < 0 {
            let e = std::io::Error::last_os_error();
            unsafe { libc::close(fd) };
            return Err(NetError::from_io("Listen failed", &e));
        }

        use std::os::unix::io::{FromRawFd, OwnedFd};
        // SAFETY: fd is a freshly created, successfully listening socket;
        // ownership moves into OwnedFd (no other close on this number).
        let listener = TcpListener::from(unsafe { OwnedFd::from_raw_fd(fd) });
        *self.listener.lock().unwrap_or_else(|e| e.into_inner()) = Some(listener);
        *self.port.lock().unwrap_or_else(|e| e.into_inner()) = port;
        Ok(())
    }

    /// Windows variant of `listen`: std bind on INADDR_ANY. Windows sets
    /// SO_REUSEADDR semantics per-binding (no stolen-port hijack concern for
    /// a user-level coordinator); `backlog` is accepted for API parity and
    /// clamped to the OS default by the stack.
    #[cfg(windows)]
    pub fn listen(&self, port: u16, _backlog: i32) -> Result<(), NetError> {
        self.stop();
        let listener = TcpListener::bind((Ipv4Addr::UNSPECIFIED, port))
            .map_err(|e| NetError::from_io("Bind failed", &e))?;
        *self.listener.lock().unwrap_or_else(|e| e.into_inner()) = Some(listener);
        *self.port.lock().unwrap_or_else(|e| e.into_inner()) = port;
        Ok(())
    }

    /// Block until a client connects. Polls with a 500ms timeout so the
    /// accept loop can wake up periodically and observe a shutdown (a bare
    /// accept(2) would block forever and a concurrent close does not
    /// reliably wake it on Linux, hanging the process on Ctrl+C). The
    /// fd-mutex is held across poll+accept: a concurrent stop() must not
    /// close the descriptor while it is in (or about to enter) a syscall —
    /// the fd number could be reused by an unrelated open() and accepted as
    /// if it were this listener.
    ///
    /// Poll timeout surfaces as errno EAGAIN so the caller can re-check its
    /// run flag and loop back if still alive.
    #[cfg(unix)]
    pub fn accept(&self) -> Result<TcpSocket, NetError> {
        let guard = self.listener.lock().unwrap_or_else(|e| e.into_inner());
        let Some(listener) = guard.as_ref() else {
            return Err(NetError::ebadf("Server not listening"));
        };
        let fd = listener.as_raw_fd();

        let mut pfd = libc::pollfd {
            fd,
            events: libc::POLLIN,
            revents: 0,
        };
        let ready = loop {
            let pr = unsafe { libc::poll(&mut pfd, 1, 500) };
            if pr < 0 {
                let err = std::io::Error::last_os_error();
                if err.raw_os_error() == Some(libc::EINTR) {
                    continue;
                }
                return Err(NetError::from_io("Poll failed", &err));
            }
            break pr;
        };
        if ready == 0 {
            return Err(NetError::eagain("Accept timed out"));
        }
        // The listener is only readable when a connection is pending;
        // anything else (POLLNVAL after an external close, POLLERR/POLLHUP)
        // means the descriptor is gone and accept(2) must not be called.
        if (pfd.revents & libc::POLLIN) == 0 {
            return Err(NetError::new(
                libc::ECONNABORTED,
                "Listen socket no longer usable",
            ));
        }

        let (stream, peer) = listener.accept().map_err(|e| NetError::from_io("Accept failed", &e))?;
        drop(guard);

        apply_stream_sockopts(&stream);
        let peer_ip = match peer.ip() {
            IpAddr::V4(v4) => v4.to_string(),
            IpAddr::V6(v6) => v6.to_string(),
        };
        Ok(TcpSocket {
            core: Arc::new(StreamCore::new(stream)),
            peer_address: Mutex::new(peer_ip),
            peer_port: Mutex::new(peer.port()),
        })
    }

    /// Windows variant of `accept`: nonblocking accept in a 500ms poll loop
    /// (std accept(2) cannot be interrupted by a concurrent stop() on
    /// Windows either). A quiet tick surfaces as EAGAIN, matching the unix
    /// contract so the caller's run-flag re-check works unchanged.
    #[cfg(windows)]
    pub fn accept(&self) -> Result<TcpSocket, NetError> {
        loop {
            let guard = self.listener.lock().unwrap_or_else(|e| e.into_inner());
            let Some(listener) = guard.as_ref() else {
                return Err(NetError::ebadf("Server not listening"));
            };
            listener
                .set_nonblocking(true)
                .map_err(|e| NetError::from_io("set_nonblocking failed", &e))?;
            match listener.accept() {
                Ok((stream, peer)) => {
                    let _ = stream.set_nonblocking(false);
                    drop(guard);
                    apply_stream_sockopts(&stream);
                    let peer_ip = peer.ip().to_string();
                    return Ok(TcpSocket {
                        core: Arc::new(StreamCore::new(stream)),
                        peer_address: Mutex::new(peer_ip),
                        peer_port: Mutex::new(peer.port()),
                    });
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    drop(guard);
                    std::thread::sleep(Duration::from_millis(500));
                    // Loop re-takes the lock, so a concurrent stop() is seen.
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(NetError::from_io("Accept failed", &e)),
            }
        }
    }


    /// Stop listening and release the descriptor.
    pub fn stop(&self) {
        *self.listener.lock().unwrap_or_else(|e| e.into_inner()) = None;
        *self.port.lock().unwrap_or_else(|e| e.into_inner()) = 0;
    }

    pub fn port(&self) -> u16 {
        *self.port.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// Local IPv4 addresses, excluding loopback and virtual container/bridge
/// interfaces, ordered with physical LAN/WLAN interfaces first.
pub fn local_ips() -> Vec<IpAddr> {
    let mut primary = Vec::new();
    let mut secondary = Vec::new();
    if let Ok(ifaces) = if_addrs::get_if_addrs() {
        for iface in ifaces {
            if let if_addrs::IfAddr::V4(v4) = iface.addr {
                let n = iface.name.as_str();
                if n == "lo"
                    || n.contains("docker")
                    || n.starts_with("br-")
                    || n.starts_with("veth")
                    || n.starts_with("virbr")
                    || n.starts_with("cni")
                    || n.starts_with("flannel")
                    || n.starts_with("podman")
                {
                    continue;
                }
                let is_physical_lan = (n.starts_with("wl")
                    || n.starts_with("en")
                    || n.starts_with("eth")
                    || v4.ip.octets()[0] == 192)
                    && !n.starts_with("tun")
                    && !n.starts_with("tap")
                    && !n.starts_with("tailscale")
                    && !n.starts_with("wg");
                if is_physical_lan {
                    primary.push(IpAddr::V4(v4.ip));
                } else {
                    secondary.push(IpAddr::V4(v4.ip));
                }
            }
        }
    }
    primary.extend(secondary);
    primary
}
