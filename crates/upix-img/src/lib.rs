//! upix-img — Rust port of `share/utils/{image,image_avif,exif,sr_marker,
//! encoder_options}`: decode/encode (png/jpeg/webp + avif via runtime-dlopen
//! system libavif), EXIF orientation, SR marker (urn:upix:sr:1).
//!
//! Porting contract: docs/rewrite/CONTRACT.md §img.

use serde::{Deserialize, Serialize};

/// Canonical per-format encoder knobs (mirror of `upix::EncoderOptions`,
/// share/utils/encoder_options.hpp). Defaults match EncoderOptionsWire and
/// the C++ defaults exactly; the wire mirror lives in upix-net.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct EncoderOptions {
    /// -1 = auto (derive from output_quality)
    pub png_level: i32,
    /// 420 default
    pub jpeg_subsampling: i32,
    /// -1 = auto (libwebp default)
    pub webp_method: i32,
    pub webp_lossless: bool,
    /// 6 default
    pub avif_speed: i32,
    /// 420 default
    pub avif_subsampling: i32,
    /// 0 = auto (hardware concurrency)
    pub avif_threads: i32,
}

impl Default for EncoderOptions {
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
