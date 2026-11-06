//! Error types for upix-net.

/// Crypto-layer failures. Only the paths the C++ can also fail
/// (`crypto_scalarmult` low-order point, secretbox open) are observable;
/// the others exist to keep the library panic-free.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum CryptoError {
    #[error("x25519 scalarmult failed (low-order point?)")]
    LowOrderPoint,
    #[error("blake2b key setup failed")]
    Blake2bKey,
    #[error("crypto_secretbox seal failed (unreachable for well-formed input)")]
    Secretbox,
    #[error("EncryptedSession used before activate()")]
    SessionNotActive,
}

pub type Result<T, E = CryptoError> = std::result::Result<T, E>;

/// C++ `NetworkError{code, message}` mirror: errno-style code plus context.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{message} (code {code})")]
pub struct NetError {
    pub code: i32,
    pub message: String,
}

impl NetError {
    pub fn new(code: i32, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    pub fn from_io(context: &str, err: &std::io::Error) -> Self {
        Self::new(
            err.raw_os_error().unwrap_or(libc::EIO),
            format!("{context}: {err}"),
        )
    }

    pub fn ebadf(message: impl Into<String>) -> Self {
        Self::new(libc::EBADF, message)
    }

    /// "Accept timed out" / recv poll timeout: errno EAGAIN, same observable
    /// signal the C++ returns so accept loops can re-check their run flag.
    pub fn eagain(message: impl Into<String>) -> Self {
        Self::new(libc::EAGAIN, message)
    }

    pub fn econnreset(message: impl Into<String>) -> Self {
        Self::new(libc::ECONNRESET, message)
    }

    pub fn epipe(message: impl Into<String>) -> Self {
        Self::new(libc::EPIPE, message)
    }
}

/// Identity / trust store failures (the C++ throws `std::runtime_error`).
#[derive(Debug, thiserror::Error)]
pub enum IdentityError {
    #[error("Failed to open identity file: {path}")]
    Open { path: String },
    #[error("Identity file truncated: {path} (expected 64 bytes = seed || pubkey)")]
    Truncated { path: String },
    #[error("Failed to create identity directory {path}: {source}")]
    CreateDir {
        path: String,
        #[source]
        source: std::io::Error,
    },
    #[error("Failed to create identity file: {path}")]
    Write {
        path: String,
        #[source]
        source: std::io::Error,
    },
}
