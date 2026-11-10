//! Installed-model scanning, family grouping (`upix_core::model_families`), and
//! downloadable catalog (`upix_host::modelctl` when `integration` is enabled).

use std::collections::HashSet;
use std::path::{Path, PathBuf};

/// One model variant item inside a family card.
#[derive(Clone, Debug, PartialEq)]
pub struct ModelVariant {
    /// Model stem (e.g. `NomosUni-SPAN-x2`).
    pub name: String,
    /// Short badge label (e.g. `x2`, `x4plus`, `medium x1`).
    pub short_name: String,
    pub scale: i32,
    pub scene: String,
    pub recommended: bool,
    pub installed: bool,
    pub param_path: PathBuf,
    pub bin_path: PathBuf,
    pub size_bytes: u64,
}

/// A grouped model family (`engine/core/model_families.hpp` / `upix_core::model_families`).
#[derive(Clone, Debug, PartialEq)]
pub struct ModelGroup {
    pub title: String,
    pub summary: String,
    pub scene: String,
    pub recommended: bool,
    pub items: Vec<ModelVariant>,
}

/// The published catalog, shared with the CLI (`upix_core::model_catalog`).
/// This module used to carry its own list, which drifted out of date; the
/// single source also means an offline GUI shows the same models as
/// `upix model list-remote`.
pub use upix_core::model_catalog::CATALOG as FALLBACK_CATALOG;

/// `UPIX_MODELS_DIR`, else `$HOME/.local/share/upix/models` (flat cache).
pub fn models_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("UPIX_MODELS_DIR") {
        return PathBuf::from(dir);
    }
    let mut dir = crate::host_bridge::home_dir().unwrap_or_else(|| PathBuf::from("."));
    dir.push(".local/share/upix/models");
    dir
}

/// Scans `dir` for `<name>.param` (+ `.bin`) pairs and groups them by model family.
pub fn scan_installed_groups(dir: &Path) -> (Vec<ModelGroup>, HashSet<String>) {
    let mut stems = Vec::new();
    let mut installed_set = HashSet::new();

    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("param") {
                continue;
            }
            let Some(stem) = path.file_stem().map(|s| s.to_string_lossy().into_owned()) else {
                continue;
            };
            if dir.join(format!("{stem}.bin")).exists() {
                installed_set.insert(stem.clone());
                stems.push(stem);
            }
        }
    }
    stems.sort();

    let groups = build_groups_from_stems(&stems, dir, &installed_set);
    (groups, installed_set)
}

/// Builds remote catalog groups from `remote_stems`, marking which variants are already in `installed_set`.
pub fn build_remote_groups(
    remote_stems: &[String],
    dir: &Path,
    installed_set: &HashSet<String>,
) -> Vec<ModelGroup> {
    let mut sorted = remote_stems.to_vec();
    sorted.sort();
    sorted.dedup();
    build_groups_from_stems(&sorted, dir, installed_set)
}

#[cfg(feature = "integration")]
fn build_groups_from_stems(
    stems: &[String],
    dir: &Path,
    installed_set: &HashSet<String>,
) -> Vec<ModelGroup> {
    let raw_groups = upix_core::model_families::groups(stems);
    let mut out = Vec::new();
    let mut orphans = Vec::new();

    for g in raw_groups {
        let variants: Vec<ModelVariant> = g
            .items
            .into_iter()
            .map(|it| {
                let param_path = dir.join(format!("{}.param", it.name));
                let bin_path = dir.join(format!("{}.bin", it.name));
                let size_bytes = std::fs::metadata(&param_path)
                    .map(|m| m.len())
                    .unwrap_or(0)
                    + std::fs::metadata(&bin_path)
                        .map(|m| m.len())
                        .unwrap_or(0);
                let scale = scale_from_filename(&it.name).unwrap_or(2);
                let meta = upix_core::model_metadata::classify_model_scene(&it.name);
                let installed = installed_set.contains(&it.name);
                ModelVariant {
                    name: it.name,
                    short_name: it.short_name,
                    scale,
                    scene: meta.scene,
                    recommended: meta.recommended,
                    installed,
                    param_path,
                    bin_path,
                    size_bytes,
                }
            })
            .collect();

        if variants.is_empty() {
            continue;
        }

        if g.label.is_empty() {
            orphans.extend(variants);
        } else {
            let first_scene = variants
                .first()
                .map(|v| v.scene.clone())
                .unwrap_or_default();
            let any_recommended = variants.iter().any(|v| v.recommended);
            let summary = format!("{} 款", variants.len());
            out.push(ModelGroup {
                title: g.label,
                summary,
                scene: first_scene,
                recommended: any_recommended,
                items: variants,
            });
        }
    }

    if !orphans.is_empty() {
        let first_scene = orphans
            .first()
            .map(|v| v.scene.clone())
            .unwrap_or_default();
        let any_recommended = orphans.iter().any(|v| v.recommended);
        let summary = format!("{} 款", orphans.len());
        out.push(ModelGroup {
            title: "独立模型".to_string(),
            summary,
            scene: first_scene,
            recommended: any_recommended,
            items: orphans,
        });
    }

    out
}

#[cfg(not(feature = "integration"))]
fn build_groups_from_stems(
    stems: &[String],
    dir: &Path,
    installed_set: &HashSet<String>,
) -> Vec<ModelGroup> {
    stems
        .iter()
        .map(|name| {
            let scale = scale_from_filename(name).unwrap_or(2);
            let param_path = dir.join(format!("{name}.param"));
            let bin_path = dir.join(format!("{name}.bin"));
            let size_bytes = std::fs::metadata(&param_path)
                .map(|m| m.len())
                .unwrap_or(0)
                + std::fs::metadata(&bin_path).map(|m| m.len()).unwrap_or(0);
            ModelGroup {
                title: name.clone(),
                summary: String::new(),
                scene: "通用".into(),
                recommended: false,
                items: vec![ModelVariant {
                    name: name.clone(),
                    short_name: format!("x{scale}"),
                    scale,
                    scene: "通用".into(),
                    recommended: false,
                    installed: installed_set.contains(name),
                    param_path,
                    bin_path,
                    size_bytes,
                }],
            }
        })
        .collect()
}

/// Classifies a model filename into `(scene, recommended)` if matched by a known rule.
pub fn classify_scene(name: &str) -> Option<(String, bool)> {
    #[cfg(feature = "integration")]
    {
        let meta = upix_core::model_metadata::classify_model_scene(name);
        if meta.scene.is_empty() || meta.scene == "通用" {
            None
        } else {
            Some((meta.scene, meta.recommended))
        }
    }
    #[cfg(not(feature = "integration"))]
    {
        let _ = name;
        None
    }
}

/// Short model badge label (e.g. `x2`, `x4plus`).
pub fn short_name(name: &str) -> String {
    #[cfg(feature = "integration")]
    {
        upix_core::model_families::short_model_name(name)
    }
    #[cfg(not(feature = "integration"))]
    {
        format!("x{}", scale_from_filename(name).unwrap_or(2))
    }
}

/// Filename scale (`x2`, `x4`, etc.).
pub fn scale_from_filename(name: &str) -> Option<i32> {
    let bytes = name.as_bytes();
    let mut from_name = 0i32;
    for i in 0..bytes.len().saturating_sub(1) {
        if bytes[i] != b'x' && bytes[i] != b'X' {
            continue;
        }
        if !bytes[i + 1].is_ascii_digit() {
            continue;
        }
        let mut n = 0i32;
        let mut j = i + 1;
        while j < bytes.len() && n <= 8 && bytes[j].is_ascii_digit() {
            n = n * 10 + (bytes[j] - b'0') as i32;
            j += 1;
        }
        if (1..=8).contains(&n) {
            from_name = n;
        }
    }
    (from_name > 0).then_some(from_name)
}

/// Formats bytes into human-readable string (`4.2 MB`, `512 KB`).
pub fn human_bytes(bytes: u64) -> String {
    if bytes == 0 {
        return "0 B".into();
    }
    const KB: f64 = 1024.0;
    const MB: f64 = KB * 1024.0;
    let b = bytes as f64;
    if b >= MB {
        format!("{:.1} MB", b / MB)
    } else if b >= KB {
        format!("{:.0} KB", b / KB)
    } else {
        format!("{bytes} B")
    }
}

/// Deletes `<name>.param` and `<name>.bin` from `dir`.
pub fn delete_local_model(dir: &Path, name: &str) -> Result<(), String> {
    let p1 = dir.join(format!("{name}.param"));
    let p2 = dir.join(format!("{name}.bin"));
    if p1.exists() {
        std::fs::remove_file(&p1).map_err(|e| format!("删除 {}: {e}", p1.display()))?;
    }
    if p2.exists() {
        std::fs::remove_file(&p2).map_err(|e| format!("删除 {}: {e}", p2.display()))?;
    }
    Ok(())
}

/// Copies picked `.param`/`.bin` files into the models dir.
pub fn import_files(files: &[PathBuf], dir: &Path) -> Vec<String> {
    let mut lines = Vec::new();
    if let Err(e) = std::fs::create_dir_all(dir) {
        return vec![format!("创建模型目录失败: {e}")];
    }
    for file in files {
        let Some(name) = file.file_name().map(|n| n.to_string_lossy().into_owned()) else {
            continue;
        };
        let ext_ok = matches!(
            file.extension().and_then(|e| e.to_str()),
            Some("param") | Some("bin")
        );
        if !ext_ok {
            lines.push(format!("{name}: 跳过（仅支持 .param / .bin）"));
            continue;
        }
        let target = dir.join(&name);
        match std::fs::copy(file, &target) {
            Ok(_) => lines.push(format!("{name}: 已导入")),
            Err(e) => lines.push(format!("{name}: 失败 {e}")),
        }
    }
    lines
}
