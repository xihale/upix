//! upix-img — Rust port of `engine/utils/{image,image_avif,exif,sr_marker,
//! encoder_options}`: decode/encode (png/jpeg/webp + avif via runtime-dlopen
//! system libavif), EXIF orientation, SR marker (urn:upix:sr:1).
//!
//! Porting contract: docs/rewrite/CONTRACT.md §img.

use serde::{Deserialize, Serialize};

/// Canonical per-format encoder knobs (mirror of `upix::EncoderOptions`,
/// engine/utils/encoder_options.hpp). Defaults match EncoderOptionsWire and
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

mod avif;
pub mod exif;
mod jpeg;
mod png;
pub mod sr_marker;
mod webp;

/// Mirror of `upix::ImageError` (code numbers preserved from image.cpp).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImgError {
    pub code: i32,
    pub message: String,
}

impl ImgError {
    pub(crate) fn new(code: i32, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

impl std::fmt::Display for ImgError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "[{}] {}", self.code, self.message)
    }
}

impl std::error::Error for ImgError {}

pub type ImgResult<T> = Result<T, ImgError>;

/// 8-bit interleaved image (`upix::Image` minus the C++ +16 upload slack,
/// which exists only for the Vulkan FFI read past a 1-D Mat).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Image {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub data: Vec<u8>,
}

impl Image {
    pub fn new(width: i32, height: i32, channels: i32) -> Self {
        Self {
            width,
            height,
            channels,
            data: vec![0; (width.max(0) as usize)
                * (height.max(0) as usize)
                * (channels.max(0) as usize)],
        }
    }

    /// Pixel bytes (w*h*c).
    pub fn size(&self) -> usize {
        self.width as usize * self.height as usize * self.channels as usize
    }

    pub fn valid(&self) -> bool {
        !self.data.is_empty() && self.width > 0 && self.height > 0
    }
}

/// C++ `normalize_image_format`: strips '.' and whitespace, lowercases.
fn normalize_image_format(format: &str) -> String {
    format
        .chars()
        .filter(|ch| *ch != '.' && !ch.is_whitespace())
        .flat_map(|ch| ch.to_lowercase())
        .collect()
}

/// Decode an in-memory encoded file by magic number (png/jpg/webp; AVIF
/// when the buffer is an ISOBMFF file with avif/avis brands).
pub fn decode(data: &[u8]) -> ImgResult<Image> {
    if data.len() >= 8 && data.starts_with(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]) {
        return png::load_png(data);
    }
    if data.len() >= 2 && data[0] == 0xFF && data[1] == 0xD8 {
        return jpeg::load_jpeg(data);
    }
    if data.len() >= 12 && &data[0..4] == b"RIFF" && &data[8..12] == b"WEBP" {
        return webp::load_webp(data);
    }
    if avif::looks_like_avif(data) {
        return avif::load_avif(data);
    }
    Err(ImgError::new(8, "Unsupported image format"))
}

/// C++ `Image::load`: file bytes + decode. Format preference follows the
/// path extension (normalized); AVIF decode also triggers on magic bytes.
/// Returns the pixel frame plus the baked-in-ready EXIF orientation
/// (JPEG APP1 tag 0x0112; `None` = orientation 1 or no EXIF / not JPEG).
pub fn decode_path(path: &std::path::Path) -> ImgResult<(Image, Option<u8>)> {
    let file_data = std::fs::read(path)
        .map_err(|_| ImgError::new(1, format!("File not found: {}", path.display())))?;
    let fmt = normalize_image_format(
        path.extension()
            .and_then(|e| e.to_str())
            .unwrap_or_default(),
    );
    let img = match fmt.as_str() {
        "png" => png::load_png(&file_data)?,
        "jpg" | "jpeg" => jpeg::load_jpeg(&file_data)?,
        "webp" => webp::load_webp(&file_data)?,
        "avif" => avif::load_avif(&file_data)?,
        _ => {
            if avif::looks_like_avif(&file_data) {
                avif::load_avif(&file_data)?
            } else {
                return Err(ImgError::new(
                    8,
                    format!("Unsupported image format: {fmt}"),
                ));
            }
        }
    };
    let orientation = exif::parse_jpeg_exif_orientation(&file_data);
    Ok((img, if orientation >= 2 { Some(orientation) } else { None }))
}

/// Bake the EXIF orientation transform into the decoded pixels
/// (`exif::autorotate_image`); orientations 5-8 swap width/height.
/// Returns true when the image changed.
pub fn apply_orientation(img: &mut Image, orientation: u8) -> bool {
    exif::autorotate_image(img, orientation as i32)
}

/// Encode to a container in memory. `xmp`: optional XMP payload embedded as
/// a metadata marker (PNG iTXt, JPEG APP1, WebP "XMP " chunk, AVIF XMP
/// item). Empty = no marker. `opts`: per-format advanced encoder knobs;
/// default = historical behavior.
pub fn encode(
    img: &Image,
    format: &str,
    quality: i32,
    opts: &EncoderOptions,
    xmp: Option<&str>,
) -> ImgResult<Vec<u8>> {
    let normalized = normalize_image_format(format);
    let output_format = if normalized.is_empty() {
        "png".to_string()
    } else {
        normalized
    };
    let clamped_quality = quality.clamp(1, 100);
    let xmp = xmp.unwrap_or_default();
    match output_format.as_str() {
        "png" => png::save_png(img, clamped_quality, opts, xmp),
        "jpg" | "jpeg" => jpeg::save_jpeg(img, clamped_quality, opts, xmp),
        "webp" => webp::save_webp(img, clamped_quality, opts, xmp),
        "avif" => avif::save_avif(img, clamped_quality, xmp, opts),
        other => Err(ImgError::new(
            22,
            format!("Unsupported save format: {other}"),
        )),
    }
}

pub use avif::{
    avif_available, avif_decode_available, avif_encode_available, looks_like_avif,
    sniff_dimensions,
};

/// Channel-planar conversion: gray(±alpha) ↔ RGB(±alpha), alpha composited
/// onto black only when actually dropping it (alpha is dropped, not
/// composited, for the RGB targets — historical CLI behavior kept byte
/// identical). Errors for channel counts outside 1..4 on either side.
pub fn convert_image_channels(source: &Image, target_channels: i32) -> ImgResult<Image> {
    let src_channels = source.channels;
    if !(1..=4).contains(&src_channels) || !(1..=4).contains(&target_channels) {
        return Err(ImgError::new(30, "unsupported channel count"));
    }

    let mut converted = Image::new(source.width, source.height, target_channels);
    let pixel_count = source.width as usize * source.height as usize;
    let src = &source.data;

    for i in 0..pixel_count {
        let src_px = &src[i * src_channels as usize..];
        let (r, g, b) = match src_channels {
            1 => (src_px[0], src_px[0], src_px[0]),
            2 => (src_px[0], src_px[0], src_px[0]),
            3 => (src_px[0], src_px[1], src_px[2]),
            _ => (src_px[0], src_px[1], src_px[2]),
        };
        let a: u8 = if src_channels == 2 || src_channels == 4 {
            src_px[(src_channels - 1) as usize]
        } else {
            255
        };

        // Truncating grayscale luma, matching the C++ integer math.
        let gray = ((77u32 * r as u32 + 150u32 * g as u32 + 29u32 * b as u32 + 128u32) >> 8) as u8;

        let dst_px = &mut converted.data[i * target_channels as usize..];
        match target_channels {
            1 => dst_px[0] = gray,
            2 => {
                dst_px[0] = gray;
                dst_px[1] = a;
            }
            3 => {
                // Dropping alpha copies RGB straight through.
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
            }
            _ => {
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                dst_px[3] = a;
            }
        }
    }
    Ok(converted)
}

/// Normalize model input channels in place, retaining RGBA for RGB models:
/// the tile processor sends RGB through inference and scales alpha
/// separately. Failures keep the image intact and log the reason.
pub fn normalize_image_channels(
    image: &mut Image,
    expected_channels: i32,
    what: &str,
) -> bool {
    if expected_channels <= 0
        || image.channels == expected_channels
        || (image.channels == 4 && expected_channels == 3)
    {
        return true;
    }
    match convert_image_channels(image, expected_channels) {
        Ok(converted) => {
            log::info!(
                "converted input channels {} -> {} for {}",
                image.channels,
                expected_channels,
                what
            );
            *image = converted;
            image.channels == expected_channels
        }
        Err(err) => {
            log::error!(
                "Input channel count {} incompatible with model input channels {}: {} ({} {})",
                image.channels,
                expected_channels,
                what,
                err.code,
                err.message
            );
            false
        }
    }
}
