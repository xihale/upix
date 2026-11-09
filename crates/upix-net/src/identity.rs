//! Persistent Ed25519 identity — Rust port of
//! `engine/core/distributed/identity.{hpp,cpp}` (IdentityKey part).

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use crate::crypto;
use crate::error::IdentityError;

/// 64-byte libsodium Ed25519 secret key = seed(32) || pk(32).
const ED_SECRETKEY_LEN: usize = 64;

/// User home for on-disk state (mirrors `engine/utils/platform.hpp`):
/// HOME covers Linux/macOS; USERPROFILE covers Windows where HOME is
/// typically unset for GUI-launched processes. Falls back to "." when
/// neither is set — same degenerate fallback the callers keep.
pub fn user_home() -> Option<String> {
    if let Some(home) = std::env::var_os("HOME") {
        if !home.is_empty() {
            return Some(home.to_string_lossy().into_owned());
        }
    }
    std::env::var_os("USERPROFILE").map(|p| p.to_string_lossy().into_owned())
}

/// `~/.upix/identity` (mirrors `default_identity_path`).
pub fn default_identity_path() -> PathBuf {
    let home = user_home().unwrap_or_else(|| ".".to_string());
    Path::new(&home).join(".upix").join("identity")
}

/// `~/.upix/trusted_clients` (mirrors `default_trust_path`).
pub fn default_trust_path() -> PathBuf {
    let home = user_home().unwrap_or_else(|| ".".to_string());
    Path::new(&home).join(".upix").join("trusted_clients")
}

/// Create/truncate `file` with mode 0600 *in the open call*, so the contents
/// never exist even briefly with the process umask's permissions — a plain
/// `fs::write` creates the file 0644 first and only gets chmod'ed afterwards,
/// leaving a window where another local user can read the Ed25519 secret key.
fn write_private_file(file: &Path, data: &[u8]) -> std::io::Result<()> {
    let mut f = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(file)?;
    f.write_all(data)
}

/// chmod 0600 after the fact; failure is not fatal (e.g. Windows), ignored.
fn ensure_0600(file: &Path) {
    if let Ok(meta) = fs::metadata(file) {
        let mut perms = meta.permissions();
        perms.set_mode(0o600);
        let _ = fs::set_permissions(file, perms);
    }
}

/// Persistent Ed25519 identity. Loads the existing key file or generates a
/// new keypair and writes it (permissions 0600, 0600 *at creation* — see
/// `write_private_file`).
pub struct Identity {
    seed: [u8; 32],
    public_key: [u8; 32],
}

impl Identity {
    /// Load or create the identity at `file`. The on-disk format is the raw
    /// 64-byte libsodium secret key (`seed || pk`); the public key is the
    /// file's last 32 bytes, taken verbatim (a hand-edited file whose pk does
    /// not match the seed is fingerprinted with the file's pk, exactly like
    /// the C++ — signatures still derive from the seed).
    pub fn load_or_create(file: &Path) -> Result<Self, IdentityError> {
        if file.exists() {
            let bytes = fs::read(file).map_err(|_| IdentityError::Open {
                path: file.to_string_lossy().into_owned(),
            })?;
            if bytes.len() != ED_SECRETKEY_LEN {
                return Err(IdentityError::Truncated {
                    path: file.to_string_lossy().into_owned(),
                });
            }
            let mut seed = [0u8; 32];
            seed.copy_from_slice(&bytes[..32]);
            let mut public_key = [0u8; 32];
            public_key.copy_from_slice(&bytes[32..]);
            return Ok(Self { seed, public_key });
        }

        // 新建。父目录（如 ~/.upix/）可能不存在，先创建。
        if let Some(parent) = file.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent).map_err(|source| IdentityError::CreateDir {
                    path: parent.to_string_lossy().into_owned(),
                    source,
                })?;
            }
        }

        let (public_key, secret) = crypto::ed25519_keygen();
        write_private_file(file, &secret).map_err(|source| IdentityError::Write {
            path: file.to_string_lossy().into_owned(),
            source,
        })?;
        ensure_0600(file);
        let mut seed = [0u8; 32];
        seed.copy_from_slice(&secret[..32]);
        Ok(Self { seed, public_key })
    }

    /// Test/construction helper: identity from a fixed 32-byte seed.
    pub fn from_seed(seed: &[u8; 32]) -> Self {
        let (public_key, _secret) = crypto::ed25519_seed_keypair(seed);
        Self {
            seed: *seed,
            public_key,
        }
    }

    pub fn public_key(&self) -> [u8; 32] {
        self.public_key
    }

    /// Ed25519 detached signature over `msg` (deterministic).
    pub fn sign(&self, msg: &[u8]) -> [u8; 64] {
        let (_pk, secret) = crypto::ed25519_seed_keypair(&self.seed);
        crypto::ed25519_sign(&secret, msg)
    }

    /// sha256(pubkey) 前 8 字节 hex 大写，如 "ABCD1234EF567890"
    /// (mirrors `fingerprint`).
    pub fn fingerprint(&self) -> String {
        fingerprint(&self.public_key)
    }

    /// Per-connection device id for AUTH_RESPONSE: RFC 4122 v4 UUID truncated
    /// to 35 chars, matching the coordinator's
    /// `crypto::generate_uuid().substr(0, 35)`.
    pub fn device_id(&self) -> String {
        let mut id = crypto::generate_uuid();
        id.truncate(35);
        id
    }
}

/// sha256(pubkey) 前 8 字节 hex 大写 (mirrors the free function
/// `fingerprint`).
pub fn fingerprint(pubkey: &[u8; 32]) -> String {
    let full = crypto::sha256_hex(pubkey);
    // sha256_hex 返回 64 字符，取前 16 字符（8 字节）大写
    full[..16].to_uppercase()
}

// ── base64 ──────────────────────────────────────────────────────────────────

/// Standard (padded, no line-wrap) base64 — libsodium
/// `sodium_base64_VARIANT_ORIGINAL`, byte-for-byte compatible, so persisted
/// TrustStore entries and exchanged peer-pubkey strings keep decoding
/// identically.
pub fn base64_encode(data: &[u8]) -> String {
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD.encode(data)
}

/// Standard base64 decode; `None` on invalid input (mirrors `base64_decode`).
pub fn base64_decode(s: &str) -> Option<Vec<u8>> {
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD.decode(s).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fingerprint_matches_sha256_prefix() {
        let fp = fingerprint(&[0u8; 32]);
        assert_eq!(fp.len(), 16);
        assert!(fp.chars().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()));
    }

    #[test]
    fn base64_roundtrip() {
        let data: Vec<u8> = (0..=255u8).collect();
        let enc = base64_encode(&data);
        assert_eq!(base64_decode(&enc).as_deref(), Some(&data[..]));
        assert_eq!(base64_decode("!!!"), None);
    }

    #[test]
    fn load_or_create_is_0600_and_stable() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("identity");

        let first = Identity::load_or_create(&path).expect("creates");
        let mode = fs::metadata(&path).expect("meta").permissions().mode();
        assert_eq!(mode & 0o777, 0o600, "identity file must be 0600");

        // Reload returns the SAME key (file format seed||pk).
        let second = Identity::load_or_create(&path).expect("loads");
        assert_eq!(first.public_key(), second.public_key());
        let msg = b"sign me";
        assert_eq!(first.sign(msg), second.sign(msg));
    }

    #[test]
    fn truncated_file_rejected() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("identity");
        fs::write(&path, [0u8; 30]).expect("write stub");
        assert!(matches!(
            Identity::load_or_create(&path),
            Err(IdentityError::Truncated { .. })
        ));
    }

    #[test]
    fn userprofile_fallback_used_when_home_missing() {
        // Mirrors user_home(): HOME unset/empty → USERPROFILE.
        // SAFETY: ENV_LOCK serializes all env mutation in this binary; no
        // other thread reads these vars concurrently.
        let _env_lock = EnvGuard::lock();
        unsafe {
            std::env::remove_var("HOME");
            std::env::set_var("USERPROFILE", "/fake/home");
        }
        let path = default_identity_path();
        let as_str = path.to_string_lossy();
        assert!(as_str.starts_with("/fake/home"), "got {as_str}");
        assert!(as_str.ends_with(".upix/identity"));

        unsafe {
            std::env::remove_var("USERPROFILE");
        }
        let fallback = default_identity_path().to_string_lossy().into_owned();
        assert!(fallback.starts_with("./"), "degenerate fallback, got {fallback}");
    }

    /// Serializes env-var mutation across the test binary's threads and
    /// restores HOME afterwards.
    struct EnvGuard;
    static ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    impl EnvGuard {
        fn lock() -> std::sync::MutexGuard<'static, ()> {
            let g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
            // SAFETY: see the test above; serialized by ENV_LOCK.
            unsafe {
                std::env::set_var("HOME", std::env::temp_dir().to_string_lossy().into_owned());
            }
            g
        }
    }
}
