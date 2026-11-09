//! upix-net — Rust port of `engine/core/distributed/{protocol,crypto_sodium,
//! crypto_session,identity}` + `engine/utils/{network,stream_socket,
//! unix_socket}`: binary frame protocol, libsodium-equivalent crypto
//! (RustCrypto), identity/truststore, TCP/UDS transport.
//!
//! WIRE COMPAT IS CONTRACTUAL: old C++/Android clients must interop.
//! Byte-exactness is proven against golden vectors (tests/golden/) dumped
//! from the C++ implementation.
//!
//! Porting contract: docs/rewrite/CONTRACT.md §net.

pub mod crypto;
pub mod error;
pub mod frame;
pub mod identity;
pub mod protocol;
pub mod session;
pub mod stream;
pub mod tcp;
pub mod trust_store;
pub mod uds;

pub use error::{CryptoError, IdentityError, NetError};
pub use session::{EncryptedSession, SessionKey};
pub use trust_store::TrustStore;

// Module-level re-exports mirroring the pinned API names.
pub use identity::{fingerprint, Identity};
pub use tcp::{local_ips, TcpServer, TcpSocket};
#[cfg(unix)]
pub use uds::{UnixServer, UnixSocket};
