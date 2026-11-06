//! Installed-model scanning and the downloadable catalog, until the core
//! integration exports `ModelManager` / `model_metadata` (the `integration`
//! feature swaps the scene table for `upix_core::model_metadata`).
//!
//! The scene table below is a verbatim mirror of `share/core/model_metadata.cpp`
//! (patterns in test order, zh labels, 推荐 flag); the key normalization is
//! the same: lowercase, `'_'` and `' '` → `'-'`, first matching rule wins.

use std::path::{Path, PathBuf};

/// One installed model row.
#[derive(Clone, Debug, PartialEq)]
pub struct ModelRow {
    /// File stem, as the CLI prints it (e.g. `NomosUni-SPAN-x2`).
    pub name: String,
    pub scale: i32,
    /// zh scene label, empty when no rule matches (imported models carry no
    /// chip — ModelsPage.qml grammar).
    pub scene: String,
    /// The table's recommended flag (the "推荐" chip).
    pub recommended: bool,
    pub param_path: PathBuf,
    pub has_bin: bool,
}

/// One catalog (remote) row.
#[derive(Clone, Debug, PartialEq)]
pub struct CatalogRow {
    pub name: &'static str,
    pub scene: &'static str,
    pub scale: i32,
    /// Whether the desktop's models/README.md calls it a first pick.
    pub recommended: bool,
}

/// The release catalog (mirrors desktop ModelsPage's remote list); downloads
/// run through `upix_host::modelctl` once the integration lands.
pub const CATALOG: &[CatalogRow] = &[
    CatalogRow { name: "NomosUni-SPAN-x2", scene: "照片", scale: 2, recommended: false },
    CatalogRow { name: "NomosUni-SPAN-x4", scene: "照片", scale: 4, recommended: false },
    CatalogRow { name: "Real-ESRGANv3-anime-x2", scene: "动漫", scale: 2, recommended: true },
    CatalogRow { name: "Real-ESRGANv3-anime-x4", scene: "动漫", scale: 4, recommended: false },
    CatalogRow { name: "Real-ESRGANv3-general-x2", scene: "照片", scale: 2, recommended: true },
    CatalogRow { name: "Real-ESRGAN-x4plus", scene: "照片", scale: 4, recommended: true },
    CatalogRow { name: "Real-CUGAN-4x", scene: "动漫", scale: 4, recommended: false },
    CatalogRow { name: "DeJPG-x1", scene: "照片·修复", scale: 1, recommended: false },
    CatalogRow { name: "Archivist-Medium-x1", scene: "动漫·修复", scale: 1, recommended: true },
    CatalogRow { name: "UltraSharp-x4", scene: "综合", scale: 4, recommended: true },
    CatalogRow { name: "Remacri-x4", scene: "综合", scale: 4, recommended: true },
    CatalogRow { name: "Animesharp-x4", scene: "动漫", scale: 4, recommended: true },
];

/// `UPIX_MODELS_DIR`, else `$HOME/.local/share/upix/models` (flat cache).
pub fn models_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("UPIX_MODELS_DIR") {
        return PathBuf::from(dir);
    }
    let mut dir = crate::host_bridge::home_dir()
        .unwrap_or_else(|| PathBuf::from("."));
    dir.push(".local/share/upix/models");
    dir
}

/// Scans `dir` for `<name>.param` (+ `.bin`) pairs, sorted by name. Missing
/// dir answers an empty list (first run).
pub fn scan_installed(dir: &Path) -> Vec<ModelRow> {
    let mut rows = Vec::new();
    let Ok(entries) = std::fs::read_dir(dir) else {
        return rows;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("param") {
            continue;
        }
        let Some(name) = path.file_stem().map(|s| s.to_string_lossy().into_owned()) else {
            continue;
        };
        let scale = scale_from_filename(&name).unwrap_or(2);
        let (scene, recommended) = classify_scene(&name)
            .map(|(s, r)| (s.to_owned(), r))
            .unwrap_or((String::new(), false));
        rows.push(ModelRow {
            has_bin: dir.join(format!("{name}.bin")).exists(),
            name,
            scale,
            scene,
            recommended,
            param_path: path,
        });
    }
    rows.sort_by(|a, b| a.name.cmp(&b.name));
    rows
}

/// Filename scale: model_graph.cpp's `scale_from_filename` verbatim — every
/// 'x'/'X' followed by digits, accumulate while digits and n ≤ 8 (the cap
/// stops "x99999999999" from wrapping into the accepted 1..8 range), the
/// last accepted value wins. Returns None for 0.
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

struct SceneRule {
    pattern: &'static str,
    scene: &'static str,
    recommended: bool,
}

/// model_metadata.cpp `kSceneRules`, same order (order matters: the anime
/// variant of a family must be tested before the photo base).
const SCENE_RULES: &[SceneRule] = &[
    SceneRule { pattern: "real-esrganv3-anime", scene: "动漫", recommended: true },
    SceneRule { pattern: "real-esrganv3-general", scene: "照片", recommended: true },
    SceneRule { pattern: "real-esrgan-x4plus-anime", scene: "动漫", recommended: false },
    SceneRule { pattern: "real-esrgan-x4plus", scene: "照片", recommended: true },
    SceneRule { pattern: "real-esrgan-x2plus", scene: "照片", recommended: false },
    SceneRule { pattern: "real-cugan", scene: "动漫", recommended: false },
    SceneRule { pattern: "nomosuni-compact", scene: "照片", recommended: false },
    SceneRule { pattern: "parimg-compact", scene: "照片", recommended: false },
    SceneRule { pattern: "nomosuni-span", scene: "照片", recommended: false },
    SceneRule { pattern: "lsdir-compact", scene: "照片", recommended: false },
    SceneRule { pattern: "nomos2-otf", scene: "照片", recommended: false },
    SceneRule { pattern: "nomos8ksc", scene: "照片", recommended: false },
    SceneRule { pattern: "nomoswebphoto", scene: "照片", recommended: false },
    SceneRule { pattern: "dejpg", scene: "照片·修复", recommended: false },
    SceneRule { pattern: "denoise", scene: "照片·修复", recommended: false },
    SceneRule { pattern: "deh264", scene: "视频·修复", recommended: false },
    SceneRule { pattern: "archivist-medium", scene: "动漫·修复", recommended: true },
    SceneRule { pattern: "archivist-rough", scene: "动漫·修复", recommended: false },
    SceneRule { pattern: "nes-composite", scene: "游戏·修复", recommended: false },
    SceneRule { pattern: "genesis-cleanup", scene: "游戏·修复", recommended: false },
    SceneRule { pattern: "hfa2k", scene: "动漫", recommended: false },
    SceneRule { pattern: "modernspanimation", scene: "动漫", recommended: false },
    SceneRule { pattern: "ultrasharp", scene: "综合", recommended: true },
    SceneRule { pattern: "remacri", scene: "综合", recommended: true },
    SceneRule { pattern: "animesharp", scene: "动漫", recommended: true },
    SceneRule { pattern: "animejanai", scene: "动漫", recommended: false },
    SceneRule { pattern: "nmkd-siax", scene: "综合", recommended: false },
    SceneRule { pattern: "bsrgan", scene: "照片", recommended: false },
];

/// `classify_model_scene` verbatim: normalize the key, first matching rule.
/// `None` = no chip (imported models).
pub fn classify_scene(name: &str) -> Option<(&'static str, bool)> {
    let mut key = String::with_capacity(name.len());
    for c in name.chars() {
        match c {
            '_' | ' ' => key.push('-'),
            _ => key.extend(c.to_lowercase()),
        }
    }
    SCENE_RULES
        .iter()
        .find(|r| key.contains(r.pattern))
        .map(|r| (r.scene, r.recommended))
}

/// Copies picked `.param`/`.bin` files into the models dir. Returns per-file
/// result lines for the log strip.
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
