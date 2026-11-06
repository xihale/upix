//! AF_UNIX transport — Rust port of `share/utils/unix_socket.{hpp,cpp}`
//! (local serve daemon). Mirrors the TCPSocket/TCPServer surface so callers
//! can swap between them. Framing I/O plus the I/O lock live in
//! `crate::stream::StreamCore`.

use std::os::unix::fs::FileTypeExt;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::io::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::{Arc, Mutex};

use crate::error::NetError;
use crate::stream::StreamCore;

/// `sockaddr_un.sun_path` limit (108 bytes on Linux): reject early with a
/// clear error rather than failing silently in connect(2)/bind(2).
const SUN_PATH_MAX: usize = 108;

/// AF_UNIX stream socket (mirrors `UnixSocket`).
pub struct UnixSocket {
    core: Arc<StreamCore<UnixStream>>,
    path: String,
}

impl Clone for UnixSocket {
    fn clone(&self) -> Self {
        Self {
            core: Arc::clone(&self.core),
            path: self.path.clone(),
        }
    }
}

impl UnixSocket {
    /// Connect to a server listening at `path` (mirrors `UnixSocket::connect`).
    pub fn connect(path: &str) -> Result<Self, NetError> {
        if path.is_empty() {
            return Err(NetError::new(libc::EINVAL, "Empty socket path"));
        }
        if path.len() >= SUN_PATH_MAX {
            return Err(NetError::new(
                libc::ENAMETOOLONG,
                format!("Socket path too long (max {})", SUN_PATH_MAX - 1),
            ));
        }
        let stream = UnixStream::connect(path)
            .map_err(|e| NetError::from_io("Connection failed", &e))?;
        // ECONNREFUSED on a present-but-unlistened socket means a stale file
        // from a crashed daemon; the errno above surfaces it distinctly so
        // the client can treat it as "no daemon" and fall back to the inline
        // path (same observable contract as the C++).
        Ok(Self {
            core: Arc::new(StreamCore::new(stream)),
            path: path.to_string(),
        })
    }

    /// Bound blocking reads AND writes (SO_RCVTIMEO + SO_SNDTIMEO), matching
    /// the TCP socket API.
    pub fn set_timeout(&self, timeout_ms: u64) -> Result<(), NetError> {
        self.core.set_timeout(Some(std::time::Duration::from_millis(timeout_ms)))
    }

    pub fn recv(&self, buf: &mut [u8]) -> Result<usize, NetError> {
        self.core.recv(buf)
    }
    pub fn recv_exact(&self, buf: &mut [u8]) -> Result<(), NetError> {
        self.core.recv_exact(buf)
    }
    pub fn recv_timeout(&self, buf: &mut [u8], timeout_ms: u64) -> Result<usize, NetError> {
        self.core
            .recv_timeout(buf, std::time::Duration::from_millis(timeout_ms))
    }
    pub fn send(&self, data: &[u8]) -> Result<usize, NetError> {
        self.core.send(data)
    }
    pub fn send_all(&self, data: &[u8]) -> Result<(), NetError> {
        self.core.send_all(data)
    }

    pub fn path(&self) -> &str {
        &self.path
    }

    /// Release the connection (mirrors `UnixSocket::close`): wakes concurrent
    /// owners via shutdown; the descriptor closes when the last clone drops.
    pub fn close(&self) {
        self.core.shutdown();
    }
}

/// AF_UNIX stream server (mirrors `UnixServer`).
pub struct UnixServer {
    listener: Mutex<Option<UnixListener>>,
    path: Mutex<String>,
}

impl Default for UnixServer {
    fn default() -> Self {
        Self::new()
    }
}

impl UnixServer {
    pub fn new() -> Self {
        Self {
            listener: Mutex::new(None),
            path: Mutex::new(String::new()),
        }
    }

    /// Create, bind, and listen at `path` (mirrors `UnixServer::listen`,
    /// backlog default 16). Removes any stale socket file first — but only
    /// after proving nothing live is behind it — then chmod 0600 so only
    /// the owning user can connect.
    pub fn listen(&self, path: &str, backlog: i32) -> Result<(), NetError> {
        self.stop();

        if path.is_empty() {
            return Err(NetError::new(libc::EINVAL, "Empty socket path"));
        }
        if path.len() >= SUN_PATH_MAX {
            return Err(NetError::new(
                libc::ENAMETOOLONG,
                format!("Socket path too long (max {})", SUN_PATH_MAX - 1),
            ));
        }

        // SAFETY: plain socket/bind/listen sequence with explicit errno
        // mapping, mirroring the C++ so error codes callers branch on
        // (EADDRINUSE, ECONNREFUSED, EEXIST) stay identical.
        let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
        if fd < 0 {
            return Err(NetError::from_io(
                "Failed to create socket",
                &std::io::Error::last_os_error(),
            ));
        }

        let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
        addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
        // sun_path is c_char (i8) and includes a trailing NUL byte.
        let path_bytes: Vec<libc::c_char> =
            path.bytes().map(|b| b as libc::c_char).collect();
        addr.sun_path[..path.len()].copy_from_slice(&path_bytes);

        // Bind first, WITHOUT unlinking an existing socket file: that file
        // may belong to a live daemon, and blind unlink+bind would silently
        // steal its accept queue, leaving two "running" daemons where only
        // one is reachable (and both serve files with independent engines).
        // Only remove the file after a probe proves nothing live is behind
        // it.
        let bind_len = path.len() + std::mem::offset_of!(libc::sockaddr_un, sun_path);
        if unsafe {
            libc::bind(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                bind_len as libc::socklen_t,
            )
        } < 0
        {
            let bind_err = std::io::Error::last_os_error();
            if bind_err.raw_os_error() != Some(libc::EADDRINUSE) {
                self.stop();
                return Err(NetError::from_io("Bind failed", &bind_err));
            }

            // Probe: is anything live behind the socket file?
            let probe = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
            if probe < 0 {
                let probe_err = std::io::Error::last_os_error();
                self.stop();
                return Err(NetError::from_io("Probe socket failed", &probe_err));
            }
            let probe_rc = unsafe {
                libc::connect(
                    probe,
                    &addr as *const _ as *const libc::sockaddr,
                    bind_len as libc::socklen_t,
                )
            };
            if probe_rc == 0 {
                unsafe { libc::close(probe) };
                self.stop();
                return Err(NetError::new(
                    libc::EADDRINUSE,
                    "Socket already served by a live process",
                ));
            }
            let probe_err = std::io::Error::last_os_error();
            unsafe { libc::close(probe) };
            if probe_err.raw_os_error() != Some(libc::ECONNREFUSED) {
                // Not connectable and not refused (EACCES, ENOENT race, ...)
                // — refuse to touch a file we cannot understand.
                self.stop();
                return Err(NetError::from_io(
                    "Socket path in use by unconnectable file",
                    &probe_err,
                ));
            }
            // ECONNREFUSED alone is not proof of a dead daemon: Linux
            // reports it for connect(2) to any existing non-socket file too.
            // Only ever remove the path when it really is (was) a socket —
            // never a user file that happens to sit on our path.
            match std::fs::metadata(path) {
                Ok(meta) if !meta.file_type().is_socket() => {
                    self.stop();
                    return Err(NetError::new(
                        libc::EEXIST,
                        "Socket path exists but is not a socket file",
                    ));
                }
                Ok(_) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => {
                    self.stop();
                    return Err(NetError::from_io("stat failed", &e));
                }
            }
            // Stale file from a crashed instance: clean it up and retry once.
            if let Err(e) = std::fs::remove_file(path) {
                if e.kind() != std::io::ErrorKind::NotFound {
                    self.stop();
                    return Err(NetError::from_io("Failed to unlink stale socket", &e));
                }
            }
            if unsafe {
                libc::bind(
                    fd,
                    &addr as *const _ as *const libc::sockaddr,
                    bind_len as libc::socklen_t,
                )
            } < 0
            {
                let e = std::io::Error::last_os_error();
                self.stop();
                return Err(NetError::from_io("Bind failed", &e));
            }
        }

        // Restrict to owner-only: local auth via filesystem permissions.
        let perms = PermissionsExt::from_mode(0o600);
        if let Err(e) = std::fs::set_permissions(path, perms) {
            self.stop();
            return Err(NetError::from_io("chmod failed", &e));
        }

        if unsafe { libc::listen(fd, backlog) } < 0 {
            let e = std::io::Error::last_os_error();
            self.stop();
            return Err(NetError::from_io("Listen failed", &e));
        }

        let listener = UnixListener::from(unsafe { OwnedFd::from_raw_fd(fd) });
        *self.listener.lock().unwrap_or_else(|e| e.into_inner()) = Some(listener);
        *self.path.lock().unwrap_or_else(|e| e.into_inner()) = path.to_string();
        Ok(())
    }

    /// Blocks until a client connects (polls 500ms so the accept loop can
    /// observe a shutdown; timeout → EAGAIN for the caller's run-flag check).
    pub fn accept(&self) -> Result<UnixSocket, NetError> {
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

        let (stream, _addr) = listener
            .accept()
            .map_err(|e| NetError::from_io("Accept failed", &e))?;
        let path = self.path.lock().unwrap_or_else(|e| e.into_inner()).clone();
        Ok(UnixSocket {
            core: Arc::new(StreamCore::new(stream)),
            path,
        })
    }

    /// Close the listener and remove the socket file so the path is clean
    /// for the next instance and clients get an immediate ECONNREFUSED
    /// (treated as "no daemon").
    pub fn stop(&self) {
        *self.listener.lock().unwrap_or_else(|e| e.into_inner()) = None;
        let mut path = self.path.lock().unwrap_or_else(|e| e.into_inner());
        if !path.is_empty() {
            let _ = std::fs::remove_file(&*path);
            path.clear();
        }
    }

    pub fn path(&self) -> String {
        self.path.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }
}
