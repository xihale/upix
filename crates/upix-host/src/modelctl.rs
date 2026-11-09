//! Rust port of `cli/src/model_downloader.cpp` (+ the `modelctl` API pin in
//! CONTRACT §6): GitHub "models" release client — manifest fetch, sha256-
//! verified zip download, staged install with backups + rollback, and local
//! .param/.bin import.
//!
//! Release layout (built by scripts/publish_models.py):
//!   manifest.json                     — catalog (name/scale/size/sha256/url)
//!   <model-name>.zip                  — one zip per model: <name>.param + <name>.bin
//!
//! Transport policy (kept from the C++): only https:// may be fetched. The
//! manifest is remote input (a user-supplied repo controls it); libcurl's
//! file:// hole is closed here by checking the response URI after redirects.
//! A manifest entry without sha256 is refused ("refusing to install an
//! unverified download").

use std::fs::File;
use std::io::{Read as _, Write as _};
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::Deserialize;
use ureq::ResponseExt as _;
use sha2::{Digest, Sha256};

/// Default release endpoint: the "models" tag of the upstream repo.
pub const DEFAULT_MODEL_REPO: &str = "xihale/upix";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RemoteModel {
    pub name: String,
    pub scale: i32,
    /// zip size in bytes
    pub size_bytes: u64,
    /// zip sha256, hex
    pub sha256: String,
    /// zip download URL
    pub url: String,
}

pub struct DownloadProgress {
    pub downloaded: u64,
    /// 0 = unknown
    pub total: u64,
}

pub struct ModelDownloadResult {
    pub ok: bool,
    pub message: String,
    pub param_path: PathBuf,
    pub bin_path: PathBuf,
}

/// Manifest URL for a repo ("owner/name" or "" for the default).
pub fn manifest_url(repo: &str) -> String {
    let r = if repo.is_empty() { DEFAULT_MODEL_REPO } else { repo };
    format!("https://github.com/{r}/releases/download/models/manifest.json")
}

#[derive(Deserialize)]
struct ManifestJson {
    version: i32,
    #[serde(default)]
    models: Vec<ManifestEntry>,
}

#[derive(Deserialize)]
struct ManifestEntry {
    name: String,
    #[serde(default = "default_scale")]
    scale: i32,
    #[serde(default)]
    size_bytes: u64,
    #[serde(default)]
    sha256: String,
    #[serde(default)]
    url: String,
}

fn default_scale() -> i32 {
    2
}

fn http_get(url: &str, timeout: Duration) -> Result<Vec<u8>, String> {
    let config = ureq::Agent::config_builder()
        .timeout_global(Some(timeout))
        .build();
    let agent: ureq::Agent = config.into();
    let resp = agent
        .get(url)
        .call()
        .map_err(|e| format!("GET {url} failed: {e}"))?;
    // Only https may be fetched — see the module doc. The final URI covers
    // redirects dropping to another scheme.
    if resp.get_uri().scheme_str() != Some("https") {
        return Err(format!(
            "refusing non-https transfer ({})",
            resp.get_uri()
        ));
    }
    let mut body = Vec::new();
    resp.into_body()
        .into_reader()
        .take(64 * 1024 * 1024)
        .read_to_end(&mut body)
        .map_err(|e| format!("read {url}: {e}"))?;
    Ok(body)
}

/// Fetch and parse the release manifest (version 1; anything else fails with
/// the same wording as the C++).
pub fn list_remote(repo: &str) -> Result<Vec<RemoteModel>, String> {
    let body = http_get(&manifest_url(repo), Duration::from_secs(30))?;
    let parsed: ManifestJson = serde_json::from_slice(&body)
        .map_err(|e| format!("failed to parse manifest: {e}"))?;
    if parsed.version != 1 {
        return Err(format!(
            "unsupported manifest version {} (this build understands version 1)",
            parsed.version
        ));
    }
    Ok(parsed
        .models
        .into_iter()
        .map(|m| RemoteModel {
            name: m.name,
            scale: m.scale,
            size_bytes: m.size_bytes,
            sha256: m.sha256,
            url: m.url,
        })
        .collect())
}

/// The embedded catalog as manifest-shaped entries: names and scale only.
/// `size_bytes`, `sha256` and `url` stay empty because they are per-release
/// facts that only the live manifest knows — a fallback listing can say *what*
/// exists, downloading still needs the network.
pub fn catalog_models() -> Vec<RemoteModel> {
    upix_core::model_catalog::names()
        .iter()
        .map(|name| RemoteModel {
            name: (*name).to_string(),
            scale: upix_core::model_graph::scale_from_filename(name).unwrap_or(2),
            size_bytes: 0,
            sha256: String::new(),
            url: String::new(),
        })
        .collect()
}

/// `list_remote` with the embedded catalog as the offline fallback.
///
/// Returns `(models, fallback_reason)`: `None` means the release answered and
/// the caller may download; `Some(e)` means the list came from
/// [`catalog_models`] and carries the network error that caused it. Every
/// caller gets the same answer for the same situation, which is the point —
/// the CLI used to hard-fail where the GUI silently fell back to a stale list.
pub fn list_remote_or_catalog(repo: &str) -> (Vec<RemoteModel>, Option<String>) {
    match list_remote(repo) {
        Ok(v) => (v, None),
        Err(e) => (catalog_models(), Some(e)),
    }
}

/// Download a model by (case/dash-insensitive) manifest name into
/// `models_dir`. Returns the installed (param, bin) paths.
pub fn download(
    name: &str,
    models_dir: &std::path::Path,
    repo: &str,
    on_progress: Option<&dyn Fn(&DownloadProgress)>,
) -> Result<(std::path::PathBuf, std::path::PathBuf), String> {
    let models = list_remote(repo)?;
    let model = find_remote(&models, name)
        .ok_or_else(|| format!("model not found in manifest: {name}"))?;
    let r = download_model(model, models_dir, on_progress);
    if r.ok {
        Ok((r.param_path, r.bin_path))
    } else {
        Err(r.message)
    }
}

/// Case/dash-insensitive name match against the manifest (same normalization
/// idea as ModelManager::get_model).
pub fn find_remote<'a>(models: &'a [RemoteModel], name: &str) -> Option<&'a RemoteModel> {
    let norm = |s: &str| s.to_ascii_lowercase().replace(['-', '_'], "");
    let target = norm(name);
    models
        .iter()
        .find(|m| norm(&m.name) == target)
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut f = File::open(path).map_err(|e| format!("cannot open {}: {e}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("read {}: {e}", path.display()))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hex(&hasher.finalize()))
}

fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

/// Download `model` (from the manifest) and install into `models_dir`:
/// stage into a unique same-filesystem directory, verify sha256, extract the
/// exact model pair, then replace the installed pair with backups and
/// rollback on handled failures (two-file replacement is not crash-atomic —
/// same as the C++).
pub fn download_model(
    model: &RemoteModel,
    models_dir: &Path,
    on_progress: Option<&dyn Fn(&DownloadProgress)>,
) -> ModelDownloadResult {
    let mut res = ModelDownloadResult {
        ok: false,
        message: String::new(),
        param_path: PathBuf::new(),
        bin_path: PathBuf::new(),
    };
    if model.name.is_empty() || model.name.contains('/') || model.name.contains('\\') {
        res.message = "model name must be a nonempty filename without path separators".into();
        return res;
    }
    if let Err(e) = std::fs::create_dir_all(models_dir) {
        res.message = format!(
            "cannot create models directory {}: {e}",
            models_dir.display()
        );
        return res;
    }
    let stage = match create_stage(models_dir) {
        Ok(s) => s,
        Err(e) => {
            res.message = e;
            return res;
        }
    };
    let discard_stage = || {
        let _ = std::fs::remove_dir_all(&stage);
    };

    let zip_path = stage.join("download.zip");
    if let Err(e) = http_download_to_file(
        &model.url,
        &zip_path,
        model.size_bytes,
        on_progress,
    ) {
        discard_stage();
        res.message = e;
        return res;
    }

    let mut expected = model.sha256.to_lowercase();
    if expected.is_empty() {
        discard_stage();
        res.message = "manifest entry has no sha256 — refusing to install an unverified download"
            .into();
        return res;
    }
    let hash = match sha256_file(&zip_path) {
        Ok(h) => h,
        Err(e) => {
            discard_stage();
            res.message = e;
            return res;
        }
    };
    expected = expected.to_lowercase();
    if hash != expected {
        discard_stage();
        res.message = format!("sha256 mismatch: expected {expected}, got {hash}");
        return res;
    }

    let extracted = stage.join("extracted");
    if let Err(e) = std::fs::create_dir(&extracted) {
        discard_stage();
        res.message = format!("cannot create extraction directory: {e}");
        return res;
    }
    if let Err(e) = extract_zip(&zip_path, &extracted, &model.name) {
        discard_stage();
        res.message = e;
        return res;
    }

    let param = models_dir.join(format!("{}.param", model.name));
    let bin = models_dir.join(format!("{}.bin", model.name));
    let staged_param = extracted.join(param.file_name().unwrap_or_default());
    let staged_bin = extracted.join(bin.file_name().unwrap_or_default());
    let backup_param = stage.join("previous.param");
    let backup_bin = stage.join("previous.bin");

    // Refuse a destination that is not a regular file; symlink-to-regular is
    // fine (that is how a model name points at another copy).
    let inspect = |path: &Path| -> Result<bool, String> {
        match std::fs::symlink_metadata(path) {
            Err(_) => Ok(false),
            Ok(md) if md.is_file() => Ok(true),
            Ok(md) if md.file_type().is_symlink() => match std::fs::metadata(path) {
                Ok(t) if t.is_file() => Ok(true),
                _ => Err(format!(
                    "existing model destination {} is not a regular file",
                    path.display()
                )),
            },
            Ok(_) => Err(format!(
                "existing model destination {} is not a regular file",
                path.display()
            )),
        }
    };
    let (had_param, had_bin) = match (inspect(&param), inspect(&bin)) {
        (Ok(a), Ok(b)) => (a, b),
        (Err(m), _) | (_, Err(m)) => {
            discard_stage();
            res.message = m;
            return res;
        }
    };

    let mut saved_param = false;
    let mut saved_bin = false;
    let mut installed_param = false;
    let mut installed_bin = false;
    let mut committed = true;
    if had_param {
        committed = std::fs::rename(&param, &backup_param).is_ok();
        saved_param = committed;
    }
    if committed && had_bin {
        committed = std::fs::rename(&bin, &backup_bin).is_ok();
        saved_bin = committed;
    }
    if committed {
        committed = std::fs::rename(&staged_param, &param).is_ok();
        installed_param = committed;
    }
    if committed {
        committed = std::fs::rename(&staged_bin, &bin).is_ok();
        installed_bin = committed;
    }
    if !committed {
        // Rollback: remove what THIS attempt installed, restore only what it
        // actually moved aside.
        let mut failures = String::new();
        if installed_param {
            if std::fs::remove_file(&param).is_err() {
                failures.push_str("; cannot remove installed .param");
            }
        }
        if installed_bin {
            if std::fs::remove_file(&bin).is_err() {
                failures.push_str("; cannot remove installed .bin");
            }
        }
        if saved_param && std::fs::rename(&backup_param, &param).is_err() {
            failures.push_str("; cannot restore previous .param");
        }
        if saved_bin && std::fs::rename(&backup_bin, &bin).is_err() {
            failures.push_str("; cannot restore previous .bin");
        }
        if failures.is_empty() {
            discard_stage();
            res.message =
                "model install failed (previous files restored)".into();
        } else {
            res.message = format!(
                "model install failed; rollback also failed{}. Recovery backups and staged files are in {}",
                failures,
                stage.display()
            );
        }
        return res;
    }

    res.param_path = param;
    res.bin_path = bin;
    res.ok = true;
    discard_stage();
    res
}

fn http_download_to_file(
    url: &str,
    dest: &Path,
    expected_size: u64,
    on_progress: Option<&dyn Fn(&DownloadProgress)>,
) -> Result<(), String> {
    if !url.starts_with("https://") {
        return Err(format!("refusing non-https url ({url})"));
    }
    let config = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(3600)))
        .build();
    let agent: ureq::Agent = config.into();
    let resp = agent
        .get(url)
        .call()
        .map_err(|e| format!("GET {url} failed: {e}"))?;
    if resp.get_uri().scheme_str() != Some("https") {
        return Err(format!("refusing non-https transfer ({})", resp.get_uri()));
    }
    let mut reader = resp.into_body().into_reader();
    let mut out = File::create(dest).map_err(|e| format!("create {}: {e}", dest.display()))?;
    let mut downloaded: u64 = 0;
    let mut buf = [0u8; 128 * 1024];
    loop {
        let n = reader
            .read(&mut buf)
            .map_err(|e| format!("download {url}: {e}"))?;
        if n == 0 {
            break;
        }
        out.write_all(&buf[..n])
            .map_err(|e| format!("write {}: {e}", dest.display()))?;
        downloaded += n as u64;
        if let Some(cb) = on_progress {
            cb(&DownloadProgress {
                downloaded,
                total: expected_size,
            });
        }
    }
    out.sync_all().ok();
    if expected_size > 0 && downloaded != expected_size {
        return Err(format!(
            "downloaded {} bytes, expected {expected_size}",
            downloaded
        ));
    }
    Ok(())
}

/// Extract only model entries into a private stage. Ancillary archive
/// entries are ignored, while every .param/.bin entry must use the published
/// names.
fn extract_zip(zip_path: &Path, out_dir: &Path, model_name: &str) -> Result<(), String> {
    let f = File::open(zip_path).map_err(|e| format!("cannot open zip: {e}"))?;
    let mut zip =
        zip::ZipArchive::new(f).map_err(|e| format!("cannot open zip: {e}"))?;
    let expected_param = format!("{model_name}.param");
    let expected_bin = format!("{model_name}.bin");
    let mut saw_param = false;
    let mut saw_bin = false;
    for i in 0..zip.len() {
        let mut entry = zip
            .by_index(i)
            .map_err(|e| format!("cannot read zip entry {i}: {e}"))?;
        let raw_name = entry.name().to_string();
        let slash = raw_name.rfind('/');
        let base = match slash {
            Some(p) => raw_name[p + 1..].to_string(),
            None => raw_name.clone(),
        };
        let lower = base.to_lowercase();
        if !lower.ends_with(".param") && !lower.ends_with(".bin") {
            continue;
        }
        let is_param = raw_name == expected_param;
        let is_bin = raw_name == expected_bin;
        if !is_param && !is_bin {
            return Err(format!(
                "unexpected model entry '{raw_name}' (expected '{expected_param}' and '{expected_bin}')"
            ));
        }
        let dst = out_dir.join(&base);
        let mut out = File::create(&dst).map_err(|e| format!("create {}: {e}", dst.display()))?;
        std::io::copy(&mut entry, &mut out)
            .map_err(|e| format!("extract {raw_name}: {e}"))?;
        if is_param {
            saw_param = true;
        }
        if is_bin {
            saw_bin = true;
        }
    }
    if !saw_param || !saw_bin {
        return Err(format!(
            "zip is missing model files (param found: {saw_param}, bin found: {saw_bin})"
        ));
    }
    Ok(())
}

/// Unique same-filesystem staging directory (the C++ create_stage).
fn create_stage(dir: &Path) -> Result<PathBuf, String> {
    use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};
    static SEQUENCE: AtomicU64 = AtomicU64::new(0);
    for _ in 0..100 {
        let tick = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let stage = dir.join(format!(
            ".upix-model-stage-{}-{}",
            tick,
            SEQUENCE.fetch_add(1, AtomicOrdering::SeqCst)
        ));
        match std::fs::create_dir(&stage) {
            Ok(()) => return Ok(stage),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => {
                return Err(format!(
                    "cannot create staging directory {}: {e}",
                    stage.display()
                ))
            }
        }
    }
    Err(format!(
        "cannot create a unique staging directory in {}",
        dir.display()
    ))
}

/// Import local .param/.bin files (user-provided models): each file is
/// copied into the models dir; pairs are matched by stem. Only .param/.bin.
pub fn import(paths: &[PathBuf], models_dir: &Path) -> Result<Vec<(PathBuf, PathBuf)>, String> {
    std::fs::create_dir_all(models_dir)
        .map_err(|e| format!("cannot create models directory {}: {e}", models_dir.display()))?;
    let mut installed = Vec::new();
    // Group by stem so a pair lands together.
    let mut by_stem: std::collections::BTreeMap<String, Vec<PathBuf>> =
        std::collections::BTreeMap::new();
    for p in paths {
        let ext = p
            .extension()
            .map(|e| e.to_string_lossy().to_lowercase())
            .unwrap_or_default();
        if ext != "param" && ext != "bin" {
            return Err(format!(
                "import expects .param/.bin files (got {})",
                p.display()
            ));
        }
        let stem = p
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        by_stem.entry(stem).or_default().push(p.clone());
    }
    for (stem, files) in by_stem {
        let has_param = files.iter().any(|f| {
            f.extension()
                .map(|e| e.to_string_lossy().to_lowercase() == "param")
                .unwrap_or(false)
        });
        let has_bin = files.iter().any(|f| {
            f.extension()
                .map(|e| e.to_string_lossy().to_lowercase() == "bin")
                .unwrap_or(false)
        });
        if !has_param || !has_bin {
            return Err(format!(
                "incomplete pair for '{stem}' (need both .param and .bin)"
            ));
        }
        for src in &files {
            let dst = models_dir.join(
                src.file_name()
                    .map(|n| n.to_os_string())
                    .unwrap_or_default(),
            );
            std::fs::copy(src, &dst)
                .map_err(|e| format!("copy {} -> {}: {e}", src.display(), dst.display()))?;
        }
        installed.push((
            models_dir.join(format!("{stem}.param")),
            models_dir.join(format!("{stem}.bin")),
        ));
    }
    Ok(installed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_url_matches_cpp() {
        assert_eq!(
            manifest_url(""),
            "https://github.com/xihale/upix/releases/download/models/manifest.json"
        );
        assert_eq!(
            manifest_url("other/repo"),
            "https://github.com/other/repo/releases/download/models/manifest.json"
        );
    }

    #[test]
    fn find_remote_normalizes_case_and_dashes() {
        let models = vec![RemoteModel {
            name: "NomosUni-SPAN-x2".into(),
            scale: 2,
            size_bytes: 1,
            sha256: String::new(),
            url: String::new(),
        }];
        assert_eq!(
            find_remote(&models, "nomosuni_span_X2").unwrap().name,
            "NomosUni-SPAN-x2"
        );
        assert!(find_remote(&models, "nope").is_none());
    }

    #[test]
    fn download_rejects_bad_names_without_network() {
        let m = RemoteModel {
            name: "../evil".into(),
            scale: 2,
            size_bytes: 0,
            sha256: String::new(),
            url: String::new(),
        };
        let r = download_model(&m, Path::new("/tmp/definitely-not-used"), None);
        assert!(!r.ok);
        assert!(r.message.contains("path separators"));
    }
}
