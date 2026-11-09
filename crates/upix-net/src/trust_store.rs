//! Trust whitelist — Rust port of the `TrustStore` half of
//! `legacy/share/core/distributed/identity.{hpp,cpp}`.
//!
//! File format: one base64-encoded 32-byte Ed25519 public key per line.
//! Loading drops invalid/short lines silently. `add` dedupes in memory;
//! `save` rewrites the file (0600 at creation — same rule as the identity
//! key: never expose the file with the umask's permissions first). Callers
//! query first and only add after an interactive approval; the handshake
//! never adds trust by itself.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use crate::identity::{base64_decode, base64_encode};

const ED_PUBKEY_LEN: usize = 32;

#[derive(Debug, Default)]
pub struct TrustStore {
    file: PathBuf,
    entries: Vec<[u8; 32]>,
}

impl TrustStore {
    /// Load from `file`; a missing file yields an empty store bound to that
    /// path (mirrors `TrustStore::load`).
    pub fn load(file: &Path) -> Self {
        let mut entries = Vec::new();
        if let Ok(content) = fs::read_to_string(file) {
            for line in content.lines() {
                // 去空白
                let line: String = line
                    .chars()
                    .filter(|c| !c.is_whitespace())
                    .collect();
                if line.is_empty() {
                    continue;
                }
                if let Some(bytes) = base64_decode(&line) {
                    if bytes.len() == ED_PUBKEY_LEN {
                        let mut pk = [0u8; ED_PUBKEY_LEN];
                        pk.copy_from_slice(&bytes);
                        entries.push(pk);
                    }
                }
            }
        }
        Self {
            file: file.to_path_buf(),
            entries,
        }
    }

    pub fn contains(&self, pubkey: &[u8; 32]) -> bool {
        self.entries.iter().any(|e| e == pubkey)
    }

    /// Add to the in-memory store (dedupe mirrors `TrustStore::add`).
    pub fn add(&mut self, pubkey: &[u8; 32]) {
        if !self.contains(pubkey) {
            self.entries.push(*pubkey);
        }
    }

    /// Persist to `self.file` (no-op when the path is empty). Best-effort:
    /// write failures surface via `Err` but callers historically only log
    /// them; the store state is unaffected.
    pub fn save(&self) -> std::io::Result<()> {
        if self.file.as_os_str().is_empty() {
            return Ok(());
        }
        if let Some(parent) = self.file.parent() {
            if !parent.as_os_str().is_empty() {
                // 目录已存在时不是真错误；失败 best-effort，与历史行为一致。
                let _ = fs::create_dir_all(parent);
            }
        }
        let mut body = String::with_capacity(self.entries.len() * 45);
        for pk in &self.entries {
            body.push_str(&base64_encode(pk));
            body.push('\n');
        }
        // Same 0600-at-creation rule as the identity key.
        let mut f = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&self.file)?;
        f.write_all(body.as_bytes())?;
        if let Ok(meta) = fs::metadata(&self.file) {
            let mut perms = meta.permissions();
            perms.set_mode(0o600);
            let _ = fs::set_permissions(&self.file, perms);
        }
        Ok(())
    }

    pub fn size(&self) -> usize {
        self.entries.len()
    }

    pub fn entries(&self) -> &[[u8; 32]] {
        &self.entries
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(base: u8) -> [u8; 32] {
        let mut k = [0u8; 32];
        for (i, b) in k.iter_mut().enumerate() {
            *b = base + i as u8;
        }
        k
    }

    #[test]
    fn add_query_save_reload_roundtrip() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("trusted_clients");

        let mut store = TrustStore::load(&path);
        assert_eq!(store.size(), 0, "missing file = empty store");
        let k1 = key(0x60);
        let k2 = key(0x80);
        assert!(!store.contains(&k1));

        store.add(&k1);
        store.add(&k2);
        store.add(&k1); // duplicate ignored
        assert_eq!(store.size(), 2);
        assert!(store.contains(&k1));
        assert!(store.contains(&k2));

        store.save().expect("save");
        let mode = fs::metadata(&path).expect("meta").permissions().mode();
        assert_eq!(mode & 0o777, 0o600, "trust store must be 0600");

        let reloaded = TrustStore::load(&path);
        assert_eq!(reloaded.size(), 2);
        assert!(reloaded.contains(&k1));
        assert!(reloaded.contains(&k2));
    }

    #[test]
    fn load_drops_invalid_lines_and_keeps_duplicates() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("trusted_clients");
        let content = format!(
            "{}\n\n   \t \n!!invalid!!\n{}\n  {}\t\n{}\n",
            base64_encode(&key(0x60)),
            base64_encode(&key(0x90)[..16]), // short → dropped
            base64_encode(&key(0x80)),       // padded with whitespace → kept
            base64_encode(&key(0x60)),       // duplicate → kept (load does NOT dedupe)
        );
        fs::write(&path, content).expect("write");

        let store = TrustStore::load(&path);
        assert_eq!(store.size(), 3, "valid, whitespace-wrapped, duplicate");
        assert!(store.contains(&key(0x60)));
        assert!(store.contains(&key(0x80)));
        assert!(!store.contains(&key(0x90)));
    }
}
