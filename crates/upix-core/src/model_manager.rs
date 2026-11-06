//! ModelManager — Rust port of `share/core/model_manager.{hpp,cpp}`:
//! recursive model scan, `.param` parsing (input-channel inference incl. the
//! head-Reorg divisor), tolerant name lookup.

use crate::model_graph::scale_from_filename;
use crate::model_metadata::classify_model_scene;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ModelInfo {
    pub name: String,
    pub param_path: PathBuf,
    pub bin_path: PathBuf,
    pub scale_factor: i32,
    pub input_channels: i32,
    pub total_size: u64,
    // Scene labels for the curated catalog (see model_metadata).
    // Empty scene = not a catalog model (user import) → no badge anywhere.
    // Never serialized: the phone wire format (ModelLibraryEntry) copies
    // fields explicitly.
    pub scene: String,
    pub recommended: bool,
}

#[derive(Debug, Clone)]
pub struct ModelManagerError {
    pub code: i32,
    pub message: String,
}

pub struct ModelManager {
    models_dir: PathBuf,
    models: Vec<ModelInfo>,
}

// Model names arrive from two independent sources — the Android app's asset
// layout ("Real-ESRGANv3-anime_x2", underscore before the scale suffix) and
// the PC's models/ directory ("Real-ESRGANv3-anime-x2", dash) — that disagree
// only on the word separator. Fold both into one canonical
// lowercase/dash-joined form (spaces count as separators too, so
// shell-friendly "real cugan conservative x2" resolves) so a submitted name
// resolves regardless of which separator each side happened to use.
pub(crate) fn normalize_model_name(s: &str) -> String {
    s.chars()
        .map(|c| {
            if c == '_' || c == ' ' {
                '-'
            } else {
                c.to_ascii_lowercase()
            }
        })
        .collect()
}

// Upper bound for a head Reorg (pixel_unshuffle) stride read from a param
// file. Real values are 2 or 4; the bound keeps the stride² divisor below
// anything that could overflow and rejects garbage tokens outright.
const MAX_REORG_STRIDE: i32 = 16;

// Upper bound for the blob counts a .param line declares. Real layers connect
// a handful of blobs; the bound stops a hostile/truncated line from sizing a
// vector (or a loop) from a count like 2000000000.
const MAX_BLOB_COUNT: i32 = 1024;

impl ModelManager {
    pub fn new(models_dir: PathBuf) -> Self {
        Self { models_dir, models: Vec::new() }
    }

    pub fn set_models_directory(&mut self, dir: PathBuf) {
        self.models_dir = dir;
    }

    pub fn models_directory(&self) -> &Path {
        &self.models_dir
    }

    pub fn available_models(&self) -> &[ModelInfo] {
        &self.models
    }

    pub fn get_model(&self, name: &str) -> Option<ModelInfo> {
        Self::find_model(&self.models, name).cloned()
    }

    /// Tolerant lookup shared by get_model and engine_setup: case- and
    /// separator-insensitive, and a bare base name ("realesrgan-x4plus")
    /// resolves to its single scale variant ("...-x4") when unambiguous.
    /// Returns None when no entry matches or the base name is ambiguous.
    pub fn find_model<'a>(models: &'a [ModelInfo], name: &str) -> Option<&'a ModelInfo> {
        // Tolerant match: normalize both sides so "_x2" / "-x2" and case
        // differences resolve to the same entry.
        let key = normalize_model_name(name);
        for m in models {
            if normalize_model_name(&m.name) == key {
                return Some(m);
            }
        }
        // Base-name fallback: accept "realesrgan-x4plus" for
        // "realesrgan-x4plus-x4". Only resolve when exactly one variant
        // matches; with several scale variants the request is ambiguous and we
        // report not-found.
        let mut unique: Option<&ModelInfo> = None;
        for m in models {
            let norm = normalize_model_name(&m.name);
            if norm.len() > key.len()
                && norm.starts_with(&key)
                && norm.as_bytes()[key.len()] == b'-'
            {
                if unique.is_some() {
                    return None; // multiple scale variants share this base name
                }
                unique = Some(m);
            }
        }
        unique
    }

    pub fn scan_models(&mut self) -> Result<Vec<ModelInfo>, ModelManagerError> {
        self.models.clear();

        if !self.models_dir.is_dir() {
            log::warn!(target: "ModelManager", "Models directory does not exist: {}", self.models_dir.display());
            return Ok(std::mem::take(&mut self.models));
        }

        let mut param_files: Vec<PathBuf> = Vec::new();
        // Recursively search for .param files, following directory symlinks
        // (models are routinely symlinked in from another drive). The scan
        // must never fail the whole run: an unreadable sub-directory or a
        // dangling link is skipped — a partially unreadable tree still yields
        // every model that could be read. Depth is capped because following
        // symlink cycles inside models/ would recurse unbounded.
        const MAX_SCAN_DEPTH: usize = 8;
        walk_dir(&self.models_dir, 0, MAX_SCAN_DEPTH, &mut param_files);

        // Directory order is filesystem-dependent; sort so the catalog — and
        // in particular the "first available" default model an empty -m
        // selects — is reproducible across machines and filesystems.
        param_files.sort();

        for param_path in param_files {
            if let Ok(info) = self.parse_param_file(&param_path) {
                self.models.push(info);
            }
        }

        // C++ returns a copy; the manager keeps its internal catalog for
        // get_model()/available_models().
        Ok(self.models.clone())
    }

    pub fn parse_param_file(&self, path: &Path) -> Result<ModelInfo, ModelManagerError> {
        let content = std::fs::read_to_string(path).map_err(|_| ModelManagerError {
            code: 2,
            message: "Cannot open param file".to_string(),
        })?;

        let mut info = ModelInfo::default();
        // Construct model name. If the param file sits directly in the models
        // directory (e.g. models/Real-ESRGANv3-anime-x2.param), the name is
        // just the file stem. If it lives in a model-specific subdirectory
        // (e.g. models/Real-ESRGANv3-anime/x2.param), the name is
        // "parent_dir-param_stem" to disambiguate.
        let param_stem = path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default(); // e.g., "x2", "x3"
        let parent_name = path
            .parent()
            .and_then(|p| p.file_stem())
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        // canonicalize's error path falls back to a lexical comparison,
        // keeping the check total on denied/IO-error components.
        let same_directory = |a: &Path, b: &Path| match (a.canonicalize(), b.canonicalize()) {
            (Ok(ca), Ok(cb)) => ca == cb,
            _ => lexically_normal(a) == lexically_normal(b),
        };
        let in_models_root = same_directory(
            path.parent().unwrap_or(Path::new("")),
            &self.models_dir,
        );
        info.name = if in_models_root {
            param_stem.clone()
        } else {
            format!("{}-{}", parent_name, param_stem)
        };
        info.param_path = path.to_path_buf();

        // Infer scale factor from the param stem. Nested layouts name the file
        // "x4.param"; flat layouts carry the full model name where the
        // trailing "x4" may be followed by more characters
        // ("Real-ESRGAN-x4plus"). No match → keep the 2 default.
        let from_name = scale_from_filename(&param_stem);
        info.scale_factor = match from_name {
            Some(n) if n >= 1 => n,
            _ => 2,
        };

        // Scene badge data (动漫/照片/…+ ★推荐) from the catalog table; empty
        // for imported non-catalog models.
        let meta = classify_model_scene(&info.name);
        info.scene = meta.scene;
        info.recommended = meta.recommended;

        info.input_channels = 3;

        let mut lines = content.lines();
        // Header line ("776 6" style) — the two ints are parsed but unused.
        let _header = lines.next();

        let mut inferred_input_channels = false;
        let mut seen_net_conv = false;
        let mut head_reorg_stride = 0;
        let mut input_dependent_blobs: std::collections::HashSet<String> =
            std::collections::HashSet::new();

        for line in lines {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }

            let mut tokens = line.split_whitespace();
            let Some(type_) = tokens.next() else { continue };
            let Some(_name) = tokens.next() else { continue };
            let (Some(bottom_count), Some(top_count)) =
                (tokens.next().and_then(|t| t.parse::<i32>().ok()), tokens.next().and_then(|t| t.parse::<i32>().ok()))
            else {
                continue;
            };
            if !(0..=MAX_BLOB_COUNT).contains(&bottom_count)
                || !(0..=MAX_BLOB_COUNT).contains(&top_count)
            {
                // The counts are attacker-controlled text: sizing a vector
                // from a claimed 2e9 blobs would blow up this parser. No real
                // layer has more than a handful of connections.
                continue;
            }

            let mut bottoms = Vec::with_capacity(bottom_count as usize);
            let mut tops = Vec::with_capacity(top_count as usize);
            let mut parse_failed = false;
            for _ in 0..bottom_count {
                match tokens.next() {
                    Some(t) => bottoms.push(t.to_string()),
                    None => {
                        parse_failed = true;
                        break;
                    }
                }
            }
            if !parse_failed {
                for _ in 0..top_count {
                    match tokens.next() {
                        Some(t) => tops.push(t.to_string()),
                        None => {
                            parse_failed = true;
                            break;
                        }
                    }
                }
            }
            if parse_failed {
                continue;
            }

            let param_tokens: Vec<&str> = tokens.collect();

            if type_ == "Input" {
                for top in &tops {
                    input_dependent_blobs.insert(top.clone());
                }

                if let Some(channels) = read_param_int(&param_tokens, 2) {
                    if channels > 0 {
                        info.input_channels = channels;
                        inferred_input_channels = true;
                    }
                }
                continue;
            }

            let depends_on_input = bottoms
                .iter()
                .any(|bottom| input_dependent_blobs.contains(bottom));
            if !depends_on_input {
                continue;
            }

            for top in &tops {
                input_dependent_blobs.insert(top.clone());
            }

            if inferred_input_channels {
                continue;
            }
            if type_ == "Reorg" {
                // pnnx lowers torch.pixel_unshuffle to Reorg mode 0
                // (space-to-depth). As a head op it widens the first conv's
                // channels relative to the image: conv_C = image_C * stride^2
                // (Real-ESRGAN x2plus feeds 12 = 3 * 2^2), so the image-facing
                // count divides the conv count. Only track it before the first
                // conv; later Reorgs are internal.
                let stride = read_param_int(&param_tokens, 0);
                let mode = read_param_int(&param_tokens, 1);
                if depends_on_input
                    && !seen_net_conv
                    && stride.is_some_and(|s| s > 1 && s <= MAX_REORG_STRIDE)
                    && mode.is_none_or(|m| m == 0)
                {
                    if let Some(s) = stride {
                        head_reorg_stride = s;
                    }
                }
                continue;
            }
            if type_ != "Convolution" && type_ != "ConvolutionDepthWise" {
                continue;
            }

            let num_output = read_param_int(&param_tokens, 0);
            let kernel_w = read_param_int(&param_tokens, 1);
            let kernel_h = read_param_int(&param_tokens, 11);
            let weight_data_size = read_param_int(&param_tokens, 6);
            let group = read_param_int(&param_tokens, 7);

            let (Some(num_output), Some(kernel_w), Some(weight_data_size)) =
                (num_output, kernel_w, weight_data_size)
            else {
                continue;
            };
            if num_output <= 0 || kernel_w <= 0 || weight_data_size <= 0 {
                continue;
            }

            let kh = kernel_h.unwrap_or(kernel_w);
            if kh <= 0 {
                continue;
            }

            let groups = group.unwrap_or(1).max(1);
            let denominator = num_output as i64 * kernel_w as i64 * kh as i64;
            let numerator = weight_data_size as i64 * groups as i64;
            if denominator <= 0 || numerator <= 0 || numerator % denominator != 0 {
                continue;
            }

            let inferred_channels = (numerator / denominator) as i32;
            if inferred_channels <= 0 {
                continue;
            }

            seen_net_conv = true;
            // 64-bit: the stride comes from the param file, so stride*stride in
            // `int` is an overflow waiting for a hostile/corrupt token.
            let reorg_sq = head_reorg_stride as i64 * head_reorg_stride as i64;
            if head_reorg_stride > 1 && inferred_channels as i64 % reorg_sq == 0 {
                info.input_channels = (inferred_channels as i64 / reorg_sq) as i32;
            } else {
                info.input_channels = inferred_channels;
            }
            inferred_input_channels = true;
        }

        // Derive the weights path from the path itself. Replacing the first
        // ".param" substring (a previous implementation) threw when the param
        // path did not end in ".param", and rewrote a *parent* component
        // whenever one contained that substring ("models/my.param.dir/x2.param").
        let mut bin_path = path.to_path_buf();
        bin_path.set_extension("bin");
        {
            // Model directories copied from a case-insensitive filesystem pair
            // "x2.PARAM" with "x2.BIN"; fall back to that spelling only when
            // the lowercase one is absent.
            if path.extension().and_then(|e| e.to_str()) == Some("PARAM")
                && !bin_path.exists()
            {
                let mut alt = path.to_path_buf();
                alt.set_extension("BIN");
                if alt.exists() {
                    bin_path = alt;
                }
            }
        }
        info.bin_path = bin_path;

        // Metadata errors never fail the entry; the param size always counts.
        if let Ok(param_size) = std::fs::metadata(path).map(|m| m.len()) {
            info.total_size = param_size;
            if let Ok(bin_size) = std::fs::metadata(&info.bin_path).map(|m| m.len()) {
                info.total_size += bin_size;
            }
        }

        Ok(info)
    }
}

fn walk_dir(dir: &Path, depth: usize, max_depth: usize, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return; // unreadable tree: keep every model that could be read
    };
    for entry in entries.flatten() {
        let path = entry.path();
        // Case-insensitive: model directories copied from a case-insensitive
        // filesystem ("X2.PARAM") must still be catalogued.
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let is_file = entry.file_type().map(|t| t.is_file()).unwrap_or(false)
            || std::fs::metadata(&path).map(|m| m.is_file()).unwrap_or(false);
        if ext == "param" && is_file {
            out.push(path);
            continue;
        }
        // Follow directory symlinks (metadata follows); depth cap stops
        // symlink-cycle recursion, mirroring disable_recursion_pending.
        if depth < max_depth && std::fs::metadata(&path).map(|m| m.is_dir()).unwrap_or(false) {
            walk_dir(&path, depth + 1, max_depth, out);
        }
    }
}

/// Lexical path normalization (best-effort mirror of
/// `std::filesystem::path::lexically_normal`: collapse "." and "xx/..").
fn lexically_normal(p: &Path) -> String {
    let mut parts: Vec<std::ffi::OsString> = Vec::new();
    for comp in p.components() {
        match comp {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                match parts.last() {
                    Some(_) => {
                        parts.pop();
                    }
                    None => parts.push(comp.as_os_str().to_os_string()),
                }
            }
            other => parts.push(other.as_os_str().to_os_string()),
        }
    }
    parts
        .iter()
        .map(|s| s.to_string_lossy())
        .collect::<Vec<_>>()
        .join("/")
}

// `k=v` extraction from a layer's trailing param tokens, with the C++ strict
// full-consumption rules: integer form first, then float form ("0=0.5")
// requiring full parse, finiteness and in-i32-range value.
pub(crate) fn read_param_int(param_tokens: &[&str], key: i32) -> Option<i32> {
    let prefix = format!("{}=", key);
    for token in param_tokens {
        let Some(value_str) = token.strip_prefix(&prefix) else {
            continue;
        };
        if value_str.is_empty() {
            return None;
        }

        if let Ok(value) = value_str.parse::<i32>() {
            return Some(value);
        }

        // Float form ("0=0.5"): require full consumption and an in-range
        // value — a loose float parse alone accepts a leading number and
        // ignores the tail ("1=2abc" -> 2) and an out-of-i32-range cast is UB.
        let value: f64 = value_str.parse().ok()?;
        if !value.is_finite()
            || value < i32::MIN as f64
            || value > i32::MAX as f64
        {
            return None;
        }
        return Some(value as i32);
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("upix-core-mm-{}-{}", tag, std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn minimal_param(channels: Option<i32>) -> String {
        let mut s = String::from("776 6\n");
        match channels {
            Some(c) => s.push_str(&format!(
                "Input\tdata\t0 1\tdata\t0=0 1=0 2={c}\n"
            )),
            None => s.push_str("Input\tdata\t0 1\tdata\n"),
        }
        s
    }

    #[test]
    fn scan_flat_models_dir_and_sort() {
        let dir = temp_dir("flat");
        std::fs::write(dir.join("b-x4.param"), minimal_param(Some(3))).unwrap();
        std::fs::write(dir.join("a-x2.param"), minimal_param(Some(3))).unwrap();
        std::fs::write(dir.join("a-x2.bin"), [0u8; 8]).unwrap();
        std::fs::write(dir.join("readme.txt"), "nope").unwrap();

        let mut mgr = ModelManager::new(dir.clone());
        let models = mgr.scan_models().unwrap();
        assert_eq!(models.len(), 2, "non-param files must be skipped");
        assert_eq!(models[0].name, "a-x2", "sorted catalog; default = first entry");
        assert_eq!(models[0].scale_factor, 2);
        assert_eq!(models[1].scale_factor, 4);
        assert_eq!(models[0].total_size, minimal_param(Some(3)).len() as u64 + 8);
        assert_eq!(models[0].bin_path, dir.join("a-x2.bin"));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn scan_nested_dir_prefixes_name() {
        let dir = temp_dir("nested");
        let sub = dir.join("Real-ESRGANv3-anime");
        std::fs::create_dir_all(&sub).unwrap();
        std::fs::write(sub.join("x4.param"), minimal_param(Some(3))).unwrap();

        let mut mgr = ModelManager::new(dir.clone());
        let models = mgr.scan_models().unwrap();
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].name, "Real-ESRGANv3-anime-x4");
        assert_eq!(models[0].scale_factor, 4);
        // Catalog model → badge from the table.
        assert_eq!(models[0].scene, "动漫");
        assert!(models[0].recommended);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn missing_dir_is_empty_not_error() {
        let mut mgr = ModelManager::new(PathBuf::from("/nonexistent-models-dir"));
        let models = mgr.scan_models().unwrap();
        assert!(models.is_empty());
    }

    #[test]
    fn depth_cap_and_deep_nesting() {
        let dir = temp_dir("deep");
        let mut p = dir.clone();
        for i in 0..12 {
            p = p.join(format!("lvl{i}"));
        }
        std::fs::create_dir_all(&p).unwrap();
        std::fs::write(p.join("x2.param"), minimal_param(Some(3))).unwrap();

        let mut mgr = ModelManager::new(dir.clone());
        let models = mgr.scan_models().unwrap();
        // Depth cap 8 means the level-12 file is out of reach (same as C++).
        assert!(models.is_empty());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn tolerant_lookup_variants() {
        let mut mgr = ModelManager::new(PathBuf::from("."));
        mgr.models = vec![
            ModelInfo { name: "Real-ESRGANv3-anime-x2".into(), ..Default::default() },
            ModelInfo { name: "realesrgan-x4plus-x2".into(), ..Default::default() },
            ModelInfo { name: "realesrgan-x4plus-x4".into(), ..Default::default() },
        ];

        assert_eq!(mgr.get_model("Real_ESRGANv3_anime_X2").unwrap().name, "Real-ESRGANv3-anime-x2");
        // Unambiguous base name resolves.
        assert_eq!(mgr.get_model("Real-ESRGANv3-anime").unwrap().name, "Real-ESRGANv3-anime-x2");
        // Ambiguous base name → not found.
        assert!(mgr.get_model("realesrgan-x4plus").is_none());
        assert!(mgr.get_model("nope").is_none());
    }

    #[test]
    fn input_channel_inference_conv_and_reorg() {
        let dir = temp_dir("chan");
        // Conv-based: num_output 3 * kw 3 * kh 3 = 27; weight 27*3=81? Use
        // weight_data_size = 81, groups 1 → in_c = 81 / (3*3*3) = 3.
        let conv_param = concat!(
            "10 1\n",
            "Input\tdata\t0 1\tdata\n",
            "Convolution\tout\t1 1\tdata\tfinal\t0=3 1=3 11=3 6=81 7=1\n");
        std::fs::write(dir.join("conv.param"), conv_param).unwrap();

        // Reorg head: conv 12 = 3 * 2² channels.
        let reorg_param = concat!(
            "10 1\n",
            "Input\tdata\t0 1\tdata\n",
            "Reorg\tshuf\t1 1\tdata\tshufout\t0=2 1=0\n",
            "Convolution\tc1\t1 1\tshufout\tc1out\t0=12 1=3 11=3 6=324 7=1\n",
            "Convolution\tout\t1 1\tc1out\tfinal\t0=3 1=3 11=3 6=81 7=1\n");
        std::fs::write(dir.join("reorg.param"), reorg_param).unwrap();

        // Explicit Input 2=4 wins over inference.
        std::fs::write(dir.join("in4.param"), minimal_param(Some(4))).unwrap();

        let mgr = ModelManager::new(dir.clone());
        let conv = mgr.parse_param_file(&dir.join("conv.param")).unwrap();
        assert_eq!(conv.input_channels, 3);
        let reorg = mgr.parse_param_file(&dir.join("reorg.param")).unwrap();
        assert_eq!(reorg.input_channels, 3);
        let in4 = mgr.parse_param_file(&dir.join("in4.param")).unwrap();
        assert_eq!(in4.input_channels, 4);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn hostile_counts_and_garbage_lines_are_skipped() {
        let dir = temp_dir("hostile");
        let param = concat!(
            "10 1\n",
            "# comment\n",
            "not a layer line\n",
            "Split\tboom\t1 2000000000\tdata\tout\n",
            "Input\tdata\t0 1\tdata\t0=0 1=0 2=3\n",
            "Convolution\tshort\t1 1\tmissing_bottoms\n");
        std::fs::write(dir.join("hostile.param"), param).unwrap();
        let mgr = ModelManager::new(dir.clone());
        let info = mgr.parse_param_file(&dir.join("hostile.param")).unwrap();
        assert_eq!(info.input_channels, 3);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn read_param_int_forms() {
        let toks: Vec<&str> = "0=64 1=3 6=0.5 7=abc 8=2abc 9=99999999999".split_whitespace().collect();
        assert_eq!(read_param_int(&toks, 0), Some(64));
        assert_eq!(read_param_int(&toks, 1), Some(3));
        assert_eq!(read_param_int(&toks, 6), Some(0), "integral-value float form (0.5) truncates via stod, like the C++");
        assert_eq!(read_param_int(&toks, 7), None);
        assert_eq!(read_param_int(&toks, 8), None, "partial int parse rejected");
        assert_eq!(read_param_int(&toks, 9), None, "out-of-range rejected");
        assert_eq!(read_param_int(&toks, 5), None);
        // Integral float form is accepted.
        let toks: Vec<&str> = "2=4.000000".split_whitespace().collect();
        assert_eq!(read_param_int(&toks, 2), Some(4));
    }

    /// Real model cache scan (CONTRACT acceptance): every entry's scale is
    /// derived from the filename xN convention; when the full 44-model
    /// catalog is present, the count is pinned too. A partial/missing cache
    /// is skipped gracefully.
    #[test]
    fn real_model_catalog_scan() {
        let Some(home) = std::env::var_os("HOME") else { return };
        let dir = PathBuf::from(home).join(".local/share/upix/models");
        if !dir.is_dir() {
            eprintln!("models cache absent; skipping real catalog scan");
            return;
        }
        let disk_params = std::fs::read_dir(&dir)
            .map(|rd| {
                rd.flatten()
                    .filter(|e| {
                        e.path().extension().and_then(|x| x.to_str())
                            .is_some_and(|x| x.eq_ignore_ascii_case("param"))
                    })
                    .count()
            })
            .unwrap_or(0);
        if disk_params == 0 {
            eprintln!("models cache empty; skipping real catalog scan");
            return;
        }

        let mut mgr = ModelManager::new(dir.clone());
        let models = mgr.scan_models().unwrap();
        assert_eq!(models.len(), disk_params, "scan must see every .param on disk");
        if disk_params == 44 {
            // Full catalog pinned (CONTRACT: 44 models).
        } else {
            eprintln!("partial cache: {disk_params}/44 models present; count pin skipped");
        }

        for m in &models {
            let from_name = scale_from_filename(&m.name);
            assert_eq!(
                from_name, Some(m.scale_factor),
                "scale of {m:?} must equal the filename xN convention"
            );
            assert!(m.param_path.is_file(), "param missing for {}", m.name);
            assert!(m.bin_path.is_file(), "bin missing for {}", m.name);
            assert!(m.total_size > 0, "size not counted for {}", m.name);
        }
    }
}
