//! upix-net — Rust port of `share/core/distributed/{protocol,crypto_sodium,
//! crypto_session,identity}` + `share/utils/{network,stream_socket,
//! unix_socket}`: binary frame protocol, libsodium-equivalent crypto
//! (RustCrypto), identity/truststore, TCP/UDS transport.
//!
//! WIRE COMPAT IS CONTRACTUAL: old C++/Android clients must interop.
//! Byte-exactness is proven against golden vectors (tests/golden/) dumped
//! from the C++ implementation.
//!
//! Porting contract: docs/rewrite/CONTRACT.md §net.

pub mod protocol;

pub use upix_img::EncoderOptions;
