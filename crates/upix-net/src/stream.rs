//! Shared stream-socket core — Rust port of
//! `legacy/share/utils/stream_socket.{hpp,cpp}`.
//!
//! Plain-fd streams (TCP and AF_UNIX) share this core: it owns the stream
//! plus SEPARATE receive/transmit locks. Full-duplex streams allow
//! simultaneous read+write, and the wire framing is per direction, so a
//! reader polling with SO_RCVTIMEO must never keep a sender waiting for the
//! whole poll window: a shared rx/tx lock turned every interleave into a
//! lock convoy (~200 ms per acquire; a 546 KB result took 11-23 s to
//! transfer). `send_all` / `recv_exact` hold their own direction's lock
//! across the whole call, so concurrent writers/reader races remain
//! impossible.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::unix::io::AsRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::Duration;

use crate::error::NetError;

/// Timeout-capable stream abstraction shared by TCP and UDS sockets.
/// `stream_read`/`stream_write` take `&self` by reborrowing through the
/// `impl Read/Write for &TcpStream` adapters — concurrent send/recv on one
/// socket is exactly what the direction locks are NOT protecting against
/// (they serialize per direction); the OS handles simultaneity.
pub(crate) trait StreamLike: AsRawFd {
    fn stream_read(&self, buf: &mut [u8]) -> std::io::Result<usize>;
    fn stream_write(&self, buf: &[u8]) -> std::io::Result<usize>;
    fn set_rx_timeout_opt(&self, d: Option<Duration>) -> std::io::Result<()>;
    fn set_tx_timeout_opt(&self, d: Option<Duration>) -> std::io::Result<()>;
}

impl StreamLike for TcpStream {
    fn stream_read(&self, buf: &mut [u8]) -> std::io::Result<usize> {
        let mut r = self;
        Read::read(&mut r, buf)
    }
    fn stream_write(&self, buf: &[u8]) -> std::io::Result<usize> {
        let mut w = self;
        Write::write(&mut w, buf)
    }
    fn set_rx_timeout_opt(&self, d: Option<Duration>) -> std::io::Result<()> {
        self.set_read_timeout(d)
    }
    fn set_tx_timeout_opt(&self, d: Option<Duration>) -> std::io::Result<()> {
        self.set_write_timeout(d)
    }
}

impl StreamLike for UnixStream {
    fn stream_read(&self, buf: &mut [u8]) -> std::io::Result<usize> {
        let mut r = self;
        Read::read(&mut r, buf)
    }
    fn stream_write(&self, buf: &[u8]) -> std::io::Result<usize> {
        let mut w = self;
        Write::write(&mut w, buf)
    }
    fn set_rx_timeout_opt(&self, d: Option<Duration>) -> std::io::Result<()> {
        self.set_read_timeout(d)
    }
    fn set_tx_timeout_opt(&self, d: Option<Duration>) -> std::io::Result<()> {
        self.set_write_timeout(d)
    }
}

/// Outcome of `recv_poll`: no byte arrived within the first-byte window, or
/// the full buffer was filled once the frame started arriving.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PollFrame {
    Frame,
    Timeout,
}

pub(crate) struct StreamCore<S: StreamLike> {
    sock: S,
    tx: Mutex<()>,
    rx: Mutex<()>,
    /// Full-session receive timeout configured via `set_recv_timeout`.
    /// `recv_poll` restores this after the first byte lands.
    rx_timeout: Mutex<Option<Duration>>,
    closed: AtomicBool,
}

impl<S: StreamLike> StreamCore<S> {
    pub(crate) fn new(sock: S) -> Self {
        Self {
            sock,
            tx: Mutex::new(()),
            rx: Mutex::new(()),
            rx_timeout: Mutex::new(None),
            closed: AtomicBool::new(false),
        }
    }

    pub(crate) fn sock(&self) -> &S {
        &self.sock
    }

    /// Single send syscall under the tx lock (mirrors `StreamSocket::send`).
    /// EINTR is retried so a delivered signal never looks like a dead
    /// connection.
    pub(crate) fn send(&self, data: &[u8]) -> Result<usize, NetError> {
        let _tx = self.tx.lock().unwrap_or_else(|e| e.into_inner());
        self.send_locked(data)
    }

    fn send_locked(&self, data: &[u8]) -> Result<usize, NetError> {
        if self.closed.load(Ordering::SeqCst) {
            return Err(NetError::ebadf("Socket not connected"));
        }
        loop {
            match self.sock.stream_write(data) {
                Ok(n) => return Ok(n),
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    return Err(NetError::eagain("Send timed out"));
                }
                Err(e) => return Err(NetError::from_io("Send failed", &e)),
            }
        }
    }

    /// Send everything, holding the tx lock across the whole call (mirrors
    /// `StreamSocket::send_all`). Zero-byte progress → EPIPE, same as the
    /// C++ (a zero-byte send for a non-empty request would spin forever).
    pub(crate) fn send_all(&self, mut data: &[u8]) -> Result<(), NetError> {
        let _tx = self.tx.lock().unwrap_or_else(|e| e.into_inner());
        while !data.is_empty() {
            match self.send_locked(data) {
                Ok(0) => return Err(NetError::epipe("Send made no progress")),
                Ok(n) => data = &data[n..],
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }

    /// Single recv syscall under the rx lock (mirrors `StreamSocket::receive`).
    /// EOF (0 bytes) maps to ECONNRESET like the C++.
    pub(crate) fn recv(&self, buf: &mut [u8]) -> Result<usize, NetError> {
        let _rx = self.rx.lock().unwrap_or_else(|e| e.into_inner());
        self.recv_locked(buf)
    }

    fn recv_locked(&self, buf: &mut [u8]) -> Result<usize, NetError> {
        if self.closed.load(Ordering::SeqCst) {
            return Err(NetError::ebadf("Socket not connected"));
        }
        loop {
            match self.sock.stream_read(buf) {
                Ok(0) => return Err(NetError::econnreset("Connection closed")),
                Ok(n) => return Ok(n),
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    return Err(NetError::eagain("Receive timed out"));
                }
                Err(e) => return Err(NetError::from_io("Receive failed", &e)),
            }
        }
    }

    /// Receive exactly `buf.len()` bytes, holding the rx lock across the
    /// whole call (mirrors `StreamSocket::receive_all`).
    pub(crate) fn recv_exact(&self, mut buf: &mut [u8]) -> Result<(), NetError> {
        let _rx = self.rx.lock().unwrap_or_else(|e| e.into_inner());
        while !buf.is_empty() {
            match self.recv_locked(buf) {
                Ok(n) => buf = &mut buf[n..],
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }

    /// One recv with a temporary SO_RCVTIMEO override; the configured
    /// receive timeout is restored afterwards. Never touches the send
    /// timeout (the C++ receive-poll loops rely on concurrent large sends
    /// keeping the generous SO_SNDTIMEO).
    pub(crate) fn recv_timeout(&self, buf: &mut [u8], timeout: Duration) -> Result<usize, NetError> {
        let previous = *self.rx_timeout.lock().unwrap_or_else(|e| e.into_inner());
        self.sock
            .set_rx_timeout_opt(Some(timeout))
            .map_err(|e| NetError::from_io("setsockopt failed", &e))?;
        let result = self.recv(buf);
        self.sock
            .set_rx_timeout_opt(previous)
            .map_err(|e| NetError::from_io("setsockopt restore failed", &e))?;
        result
    }

    /// First-byte-bounded frame read (the socket-level core of
    /// `Coordinator::recv_encrypted_poll`): poll window for the first byte
    /// only; once any byte of the frame is in, the rest is read with the
    /// configured receive timeout (a poll tick must never abandon a
    /// partially received frame while the peer is merely slow — that would
    /// corrupt the stream framing).
    ///
    /// Returns `PollFrame::Timeout` only when NOTHING arrived within the
    /// window; any mid-frame failure is an error.
    pub(crate) fn recv_poll(
        &self,
        buf: &mut [u8],
        first_byte_timeout: Duration,
    ) -> Result<PollFrame, NetError> {
        let _rx = self.rx.lock().unwrap_or_else(|e| e.into_inner());
        let full = *self.rx_timeout.lock().unwrap_or_else(|e| e.into_inner());
        self.sock
            .set_rx_timeout_opt(Some(first_byte_timeout))
            .map_err(|e| NetError::from_io("setsockopt failed", &e))?;

        let mut got = 0usize;
        loop {
            match self.recv_locked(&mut buf[got..]) {
                Ok(n) => {
                    got += n;
                    if got >= buf.len() {
                        break;
                    }
                    // First byte landed: switch to the full session timeout.
                    if self
                        .sock
                        .set_rx_timeout_opt(full)
                        .is_err()
                    {
                        break;
                    }
                }
                Err(e) if e.code == libc::EAGAIN && got == 0 => {
                    let _ = self.sock.set_rx_timeout_opt(full);
                    return Ok(PollFrame::Timeout);
                }
                Err(e) => {
                    let _ = self.sock.set_rx_timeout_opt(full);
                    return Err(e);
                }
            }
        }
        let _ = self.sock.set_rx_timeout_opt(full);
        Ok(PollFrame::Frame)
    }

    /// Configure ONLY the receive timeout (mirrors `set_recv_timeout`):
    /// leaves the send timeout untouched so concurrent large sends keep
    /// their generous SO_SNDTIMEO.
    pub(crate) fn set_recv_timeout(&self, d: Option<Duration>) -> Result<(), NetError> {
        *self.rx_timeout.lock().unwrap_or_else(|e| e.into_inner()) = d;
        self.sock
            .set_rx_timeout_opt(d)
            .map_err(|e| NetError::from_io("setsockopt failed", &e))
    }

    pub(crate) fn set_timeout(&self, d: Option<Duration>) -> Result<(), NetError> {
        self.sock
            .set_rx_timeout_opt(d)
            .map_err(|e| NetError::from_io("setsockopt failed", &e))?;
        self.sock
            .set_tx_timeout_opt(d)
            .map_err(|e| NetError::from_io("setsockopt failed", &e))
    }

    /// Announce shutdown in both directions WITHOUT releasing the
    /// descriptor: a thread parked in recv/recv_exact sees EOF (→
    /// ECONNRESET) and a parked send fails, instead of waiting out its
    /// SO_RCVTIMEO/SO_SNDTIMEO.
    ///
    /// Lock-free w.r.t. the I/O locks (that is the whole point — the calls
    /// being interrupted are the ones holding them). Safety contract, same
    /// as the C++: only call while the owning socket object is alive (an
    /// `Arc` clone held elsewhere satisfies this); the descriptor number
    /// cannot be recycled while any clone exists because it is closed only
    /// when the last clone drops.
    pub(crate) fn shutdown(&self) {
        // SAFETY: plain fd shutdown(2); see the lifetime contract above.
        unsafe {
            libc::shutdown(self.sock.as_raw_fd(), libc::SHUT_RDWR);
        }
    }
}

impl<S: StreamLike> Drop for StreamCore<S> {
    fn drop(&mut self) {
        self.closed.store(true, Ordering::SeqCst);
        // Closing the stream happens in S's own Drop (TcpStream/UnixStream).
    }
}
