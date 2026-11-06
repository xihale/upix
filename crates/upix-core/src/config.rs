//! ProcessingConfig + helpers — Rust port of `share/core/config.{hpp,cpp}`.
//!
//! The on-disk JSON shape mirrors the C++ `detail::ConfigJson` glaze layout:
//! field names, declaration order and defaults are byte-identical to what
//! `glz::write_json` emits, so a config file written by either implementation
//! reads back identically in both.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const DEFAULT_TILE_SIZE: i32 = 256;
pub const DEFAULT_TILE_OVERLAP: i32 = 16;
pub const DEFAULT_BATCH_SIZE: i32 = 1;
pub const DEFAULT_MIN_TILE_SIZE: i32 = 64;
pub const DEFAULT_MAX_TILE_SIZE: i32 = 512;
pub const DEFAULT_OUTPUT_QUALITY: i32 = 90;

pub const TILE_SIZE_MIN: i32 = 16;
pub const TILE_SIZE_MAX: i32 = 2048;
pub const BATCH_SIZE_MIN: i32 = 1;
pub const BATCH_SIZE_MAX: i32 = 64;

/// Supported normalized output formats.
pub const SUPPORTED_OUTPUT_FORMATS: [&str; 5] = ["png", "jpg", "jpeg", "webp", "avif"];

// Compile-time sanity pins matching the C++ constants (kept as consts so a
// future edit that breaks the documented defaults fails here, not in prod).
const _: () = assert!(DEFAULT_TILE_SIZE == 256 && DEFAULT_TILE_OVERLAP == 16);
const _: () = assert!(DEFAULT_BATCH_SIZE == 1 && DEFAULT_MIN_TILE_SIZE == 64);
const _: () = assert!(DEFAULT_MAX_TILE_SIZE == 512 && DEFAULT_OUTPUT_QUALITY == 90);
const _: () = assert!(TILE_SIZE_MIN == 16 && TILE_SIZE_MAX == 2048);
const _: () = assert!(BATCH_SIZE_MIN == 1 && BATCH_SIZE_MAX == 64);

/// Per-format advanced encoder knobs (mirror of `upix::EncoderOptions`,
/// share/utils/encoder_options.hpp — the canonical copy lives in upix-img;
/// this wire mirror exists so upix-core stays dependency-free). Field names
/// are the glaze JSON names, verbatim.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct EncoderOptionsWire {
    /// zlib-style 0-9; -1 = derive from output_quality
    pub png_level: i32,
    /// 420 / 422 / 444
    pub jpeg_subsampling: i32,
    /// 0-6; -1 = libwebp default (4)
    pub webp_method: i32,
    pub webp_lossless: bool,
    /// 0-10; 6 = still-image sweet spot
    pub avif_speed: i32,
    /// 420 / 444
    pub avif_subsampling: i32,
    /// 0 = hardware concurrency
    pub avif_threads: i32,
}

impl Default for EncoderOptionsWire {
    fn default() -> Self {
        Self {
            png_level: -1,
            jpeg_subsampling: 420,
            webp_method: -1,
            webp_lossless: false,
            avif_speed: 6,
            avif_subsampling: 420,
            avif_threads: 0,
        }
    }
}

/// On-disk JSON shape for `ProcessingConfig`, kept separate so the in-memory
/// struct can evolve without coupling to the JSON layout (mirrors the C++
/// `detail::ConfigJson` + `ProcessingConfig::load` pair). Field order = glaze
/// declaration order = the exact byte layout of `glz::write_json`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct ProcessingConfig {
    // 瓦片设置
    pub tile_size: i32,
    pub tile_overlap: i32,
    pub batch_size: i32,
    pub auto_tile_size: bool,
    pub min_tile_size: i32,
    pub max_tile_size: i32,

    // 性能设置
    pub use_gpu: bool,

    // 模型设置
    pub model_name: String,

    // 输出设置 (glaze order: output_format before output_quality)
    pub output_format: String,
    pub output_quality: i32,
    pub encoder: EncoderOptionsWire,
}

impl Default for ProcessingConfig {
    fn default() -> Self {
        Self {
            tile_size: DEFAULT_TILE_SIZE,
            tile_overlap: DEFAULT_TILE_OVERLAP,
            auto_tile_size: false,
            min_tile_size: DEFAULT_MIN_TILE_SIZE,
            max_tile_size: DEFAULT_MAX_TILE_SIZE,
            batch_size: DEFAULT_BATCH_SIZE,
            use_gpu: true,
            model_name: String::new(),
            output_quality: DEFAULT_OUTPUT_QUALITY,
            output_format: String::new(),
            encoder: EncoderOptionsWire::default(),
        }
    }
}

// Locale-independent ASCII helpers matching C-locale isspace()/tolower()
// exactly (0x09-0x0D + 0x20, A-Z).
fn ascii_tolower(ch: u8) -> u8 {
    if ch.is_ascii_uppercase() {
        ch - b'A' + b'a'
    } else {
        ch
    }
}

fn ascii_isspace(ch: u8) -> bool {
    ch == b' ' || (b'\t'..=b'\r').contains(&ch)
}

fn normalize_output_extension(format: &str) -> &str {
    if format == "jpeg" {
        "jpg"
    } else {
        format
    }
}

/// Lowercase, strip '.' and whitespace (C-locale set only).
pub fn normalize_output_format(format: &str) -> String {
    let mut normalized = String::with_capacity(format.len());
    for &raw in format.as_bytes() {
        if raw == b'.' || ascii_isspace(raw) {
            continue;
        }
        normalized.push(ascii_tolower(raw) as char);
    }
    normalized
}

pub fn is_supported_output_format(format: &str) -> bool {
    SUPPORTED_OUTPUT_FORMATS.contains(&format)
}

impl ProcessingConfig {
    /// Parse a config file; the output_format field is normalized on load
    /// exactly like the C++ loader.
    pub fn load(path: &Path) -> Result<ProcessingConfig, String> {
        let json = std::fs::read_to_string(path)
            .map_err(|_| format!("Cannot open config file: {}", path.display()))?;
        let mut parsed: ProcessingConfig = serde_json::from_str(&json)
            .map_err(|e| format!("Failed to parse config JSON: {e}"))?;
        parsed.output_format = normalize_output_format(&parsed.output_format);
        Ok(parsed)
    }

    /// Field-by-field validation, error strings byte-identical to the C++
    /// `validate()`.
    pub fn validate(&self) -> Vec<String> {
        let mut errors = Vec::new();

        if self.tile_size < TILE_SIZE_MIN || self.tile_size > TILE_SIZE_MAX {
            errors.push("tile_size must be between 16 and 2048".to_string());
        }

        if self.tile_overlap < 0 || self.tile_overlap >= self.tile_size / 2 {
            errors.push("tile_overlap must be between 0 and tile_size/2".to_string());
        }

        if self.batch_size < BATCH_SIZE_MIN || self.batch_size > BATCH_SIZE_MAX {
            errors.push("batch_size must be between 1 and 64".to_string());
        }

        if self.min_tile_size > self.max_tile_size {
            errors.push("min_tile_size cannot exceed max_tile_size".to_string());
        }

        if self.tile_size < self.min_tile_size || self.tile_size > self.max_tile_size {
            errors.push("tile_size must be within [min_tile_size, max_tile_size]".to_string());
        }

        if self.output_quality < 1 || self.output_quality > 100 {
            errors.push("output_quality must be between 1 and 100".to_string());
        }
        let normalized_format = normalize_output_format(&self.output_format);
        if !normalized_format.is_empty() && !is_supported_output_format(&normalized_format) {
            errors.push("output_format must be one of: png, jpg, jpeg, webp, avif".to_string());
        }

        if self.encoder.png_level < -1 || self.encoder.png_level > 9 {
            errors.push("encoder.png_level must be -1 (auto) or 0-9".to_string());
        }
        if self.encoder.jpeg_subsampling != 420
            && self.encoder.jpeg_subsampling != 422
            && self.encoder.jpeg_subsampling != 444
        {
            errors.push("encoder.jpeg_subsampling must be 420, 422 or 444".to_string());
        }
        if self.encoder.webp_method < -1 || self.encoder.webp_method > 6 {
            errors.push("encoder.webp_method must be -1 (auto) or 0-6".to_string());
        }
        if self.encoder.avif_speed < 0 || self.encoder.avif_speed > 10 {
            errors.push("encoder.avif_speed must be between 0 and 10".to_string());
        }
        if self.encoder.avif_subsampling != 420 && self.encoder.avif_subsampling != 444 {
            errors.push("encoder.avif_subsampling must be 420 or 444".to_string());
        }
        if self.encoder.avif_threads < 0 || self.encoder.avif_threads > 64 {
            errors.push("encoder.avif_threads must be between 0 (auto) and 64".to_string());
        }

        errors
    }

    /// Serialize to the exact glaze compact JSON form (all fields, declaration
    /// order — serde_json emits struct fields in declaration order too).
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).expect("config serialization cannot fail")
    }
}

/// Filesystem-safe tag from a model name ("Real-ESRGANv3-anime" stays as-is,
/// separators/spaces become '_'). Used to derive default output filenames.
pub fn sanitize_model_for_filename(model: &str) -> String {
    let mut out = String::with_capacity(model.len());
    for c in model.chars() {
        match c {
            '/' | '\\' | ' ' | '\t' => out.push('_'),
            _ => out.push(c),
        }
    }
    if out.is_empty() {
        "model".to_string()
    } else {
        out
    }
}

/// Replace the requested path's extension with the config's normalized output
/// format ("jpeg" folds to "jpg"); empty format keeps the path untouched.
pub fn resolve_output_path(requested: &Path, cfg: &ProcessingConfig) -> PathBuf {
    let normalized_format = normalize_output_format(&cfg.output_format);
    if normalized_format.is_empty() {
        return requested.to_path_buf();
    }
    let extension = normalize_output_extension(&normalized_format);
    let mut resolved = requested.to_path_buf();
    resolved.set_extension(extension);
    resolved
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Fixture written by hand to match the C++ glaze output byte-for-byte:
    /// compact, all fields, declaration order (tile_size, tile_overlap,
    /// batch_size, auto_tile_size, min_tile_size, max_tile_size, use_gpu,
    /// model_name, output_format, output_quality, encoder{...}).
    const GLAZE_FIXTURE: &str = concat!(
        r#"{"tile_size":256,"tile_overlap":16,"batch_size":1,"auto_tile_size":false,"#,
        r#""min_tile_size":64,"max_tile_size":512,"use_gpu":true,"model_name":"Real-ESRGANv3-anime-x2","#,
        r#""output_format":"png","output_quality":90,"#,
        r#""encoder":{"png_level":-1,"jpeg_subsampling":420,"webp_method":-1,"#,
        r#""webp_lossless":false,"avif_speed":6,"avif_subsampling":420,"avif_threads":0}}"#,
    );

    #[test]
    fn roundtrip_matches_glaze_layout() {
        let cfg: ProcessingConfig =
            serde_json::from_str(GLAZE_FIXTURE).expect("fixture must parse");
        assert_eq!(cfg.to_json(), GLAZE_FIXTURE);

        // Default-constructed config serializes to the same schema.
        let def = ProcessingConfig::default();
        let def_json = def.to_json();
        let back: ProcessingConfig =
            serde_json::from_str(&def_json).expect("own output must parse");
        assert_eq!(back, def);
    }

    #[test]
    fn unknown_keys_ignored_like_glaze() {
        // Unknown keys are ignored (glz read with error_on_unknown_keys=false),
        // prefixed before the existing body so nothing is duplicated.
        let with_extra = format!(r#"{{"future_field":42,{}"#, &GLAZE_FIXTURE[1..]);
        let cfg: ProcessingConfig =
            serde_json::from_str(&with_extra).expect("unknown keys must be ignored");
        assert_eq!(cfg.tile_size, 256);
        assert_eq!(cfg.batch_size, 1);
    }

    #[test]
    fn normalize_output_format_matches_c_locale_semantics() {
        assert_eq!(normalize_output_format(" JPEG "), "jpeg");
        assert_eq!(normalize_output_format(".Png"), "png");
        assert_eq!(normalize_output_format("Web\tP."), "webp");
        assert_eq!(normalize_output_format(""), "");
        assert!(is_supported_output_format("jpeg"));
        assert!(!is_supported_output_format("bmp"));
    }

    #[test]
    fn validate_boundaries() {
        let mut cfg = ProcessingConfig::default();
        assert!(cfg.validate().is_empty());

        // 15: range error AND outside the default [64,512] window; overlap 16
        // also crosses the 15/2 bound.
        cfg.tile_size = 15;
        assert_eq!(
            cfg.validate(),
            vec![
                "tile_size must be between 16 and 2048",
                "tile_overlap must be between 0 and tile_size/2",
                "tile_size must be within [min_tile_size, max_tile_size]",
            ]
        );
        cfg.tile_size = 2049;
        assert_eq!(
            cfg.validate(),
            vec![
                "tile_size must be between 16 and 2048",
                "tile_size must be within [min_tile_size, max_tile_size]",
            ]
        );
        // 2048 is the inclusive range max but still above the default window.
        cfg.tile_size = 2048;
        assert_eq!(
            cfg.validate(),
            vec!["tile_size must be within [min_tile_size, max_tile_size]"]
        );
        cfg.tile_size = 256;
        cfg.tile_overlap = 128; // == tile_size/2 → rejected
        assert_eq!(
            cfg.validate(),
            vec!["tile_overlap must be between 0 and tile_size/2"]
        );

        cfg.tile_overlap = 16;
        cfg.batch_size = 0;
        assert_eq!(cfg.validate(), vec!["batch_size must be between 1 and 64"]);
        cfg.batch_size = 65;
        assert_eq!(cfg.validate(), vec!["batch_size must be between 1 and 64"]);
        cfg.batch_size = 1;

        cfg.min_tile_size = 300;
        cfg.max_tile_size = 256;
        assert_eq!(
            cfg.validate(),
            vec![
                "min_tile_size cannot exceed max_tile_size",
                "tile_size must be within [min_tile_size, max_tile_size]",
            ]
        );
        cfg.min_tile_size = 64;
        cfg.max_tile_size = 512;

        cfg.output_quality = 0;
        assert_eq!(
            cfg.validate(),
            vec!["output_quality must be between 1 and 100"]
        );
        cfg.output_quality = 90;

        cfg.output_format = "bmp".to_string();
        assert_eq!(
            cfg.validate(),
            vec!["output_format must be one of: png, jpg, jpeg, webp, avif"]
        );
        cfg.output_format = "png".to_string();

        cfg.encoder.png_level = 10;
        assert_eq!(
            cfg.validate(),
            vec!["encoder.png_level must be -1 (auto) or 0-9"]
        );
        cfg.encoder.png_level = -1;
        cfg.encoder.jpeg_subsampling = 411;
        assert_eq!(
            cfg.validate(),
            vec!["encoder.jpeg_subsampling must be 420, 422 or 444"]
        );
        cfg.encoder.jpeg_subsampling = 420;
        cfg.encoder.webp_method = 7;
        assert_eq!(
            cfg.validate(),
            vec!["encoder.webp_method must be -1 (auto) or 0-6"]
        );
        cfg.encoder.webp_method = -1;
        cfg.encoder.avif_speed = 11;
        assert_eq!(
            cfg.validate(),
            vec!["encoder.avif_speed must be between 0 and 10"]
        );
        cfg.encoder.avif_speed = 6;
        cfg.encoder.avif_subsampling = 422;
        assert_eq!(
            cfg.validate(),
            vec!["encoder.avif_subsampling must be 420 or 444"]
        );
        cfg.encoder.avif_subsampling = 420;
        cfg.encoder.avif_threads = 65;
        assert_eq!(
            cfg.validate(),
            vec!["encoder.avif_threads must be between 0 (auto) and 64"]
        );
    }

    #[test]
    fn sanitize_and_resolve() {
        assert_eq!(sanitize_model_for_filename("Real-ESRGANv3-anime"), "Real-ESRGANv3-anime");
        assert_eq!(sanitize_model_for_filename("a/b\\c d\te"), "a_b_c_d_e");
        assert_eq!(sanitize_model_for_filename(""), "model");

        let mut cfg = ProcessingConfig::default();
        cfg.output_format = "JPEG".to_string();
        assert_eq!(
            resolve_output_path(Path::new("/tmp/out.png"), &cfg),
            PathBuf::from("/tmp/out.jpg")
        );
        cfg.output_format = String::new();
        assert_eq!(
            resolve_output_path(Path::new("/tmp/out.png"), &cfg),
            PathBuf::from("/tmp/out.png")
        );
    }
}
