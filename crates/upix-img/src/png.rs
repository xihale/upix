//! PNG decode/encode. Port of the PNG paths in `engine/utils/image.cpp`
//! (libdeflate/libpng writers + libpng reader) onto the `png` crate.

use crate::{Image, ImgError};

/// C++ `load_png` (libpng): strip 16→8 bit, palette→RGB, tRNS→alpha,
/// gray→RGB. `png` crate EXPAND does palette/tRNS/sub-8-bit; STRIP_16 does
/// 16→8; gray stays gray and is replicated to RGB below (libpng
/// png_set_gray_to_rgb semantics: channel replication).
pub(crate) fn load_png(buffer: &[u8]) -> Result<Image, ImgError> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(buffer));
    decoder.set_transformations(
        png::Transformations::EXPAND | png::Transformations::STRIP_16,
    );
    let mut reader = decoder
        .read_info()
        .map_err(|_| ImgError::new(5, "Error during PNG reading"))?;
    let mut buf = vec![0u8; reader.output_buffer_size()];
    let info = reader
        .next_frame(&mut buf)
        .map_err(|_| ImgError::new(5, "Error during PNG reading"))?;
    buf.truncate(info.buffer_size());

    let width = info.width as i32;
    let height = info.height as i32;
    let channels = info.color_type.samples() as i32;
    if width <= 0 || height <= 0 {
        return Err(ImgError::new(5, "Invalid PNG dimensions"));
    }
    if channels <= 0 || channels > 4 {
        return Err(ImgError::new(5, "Invalid channel count from PNG"));
    }

    // Gray (±alpha) → RGB(±alpha): libpng png_set_gray_to_rgb replicates the
    // gray sample into R, G, B.
    if channels == 1 || channels == 2 {
        let mut rgb = Image::new(width, height, channels + 2);
        let px = width as usize * height as usize;
        let sc = channels as usize;
        let dc = sc + 2;
        for i in 0..px {
            let g = buf[i * sc];
            rgb.data[i * dc] = g;
            rgb.data[i * dc + 1] = g;
            rgb.data[i * dc + 2] = g;
            if sc == 2 {
                rgb.data[i * 4 + 3] = buf[i * 2 + 1];
            }
        }
        return Ok(rgb);
    }
    Ok(Image {
        width,
        height,
        channels,
        data: buf,
    })
}

/// C++ `png_compression_level_from_quality`.
fn png_compression_level_from_quality(quality: i32) -> i32 {
    let clamped = quality.clamp(1, 100);
    let level = ((100 - clamped) * 9 + 50) / 100;
    level.clamp(0, 9)
}

/// C++ `png_zlib_level`: an explicit EncoderOptions.png_level (0-9)
/// overrides the quality-derived mapping.
pub(crate) fn png_zlib_level(quality: i32, opts: &crate::EncoderOptions) -> i32 {
    if opts.png_level >= 0 {
        return opts.png_level.clamp(0, 9);
    }
    png_compression_level_from_quality(quality)
}

/// PNG color type for the 1-4 channel 8-bit images this codebase produces.
fn color_type(channels: i32) -> Result<png::ColorType, ImgError> {
    match channels {
        1 => Ok(png::ColorType::Grayscale),
        2 => Ok(png::ColorType::GrayscaleAlpha),
        3 => Ok(png::ColorType::Rgb),
        4 => Ok(png::ColorType::Rgba),
        _ => Err(ImgError::new(
            3,
            format!("Unsupported channel count for PNG: {channels}"),
        )),
    }
}

/// Map the zlib level scale (0-9) onto the png crate's fixed presets.
/// The png crate has no per-level control: Huffman ≈ stored (level 0),
/// Fast = 1, Default = 6, Best = 9. Filter choice/level affects only
/// encoded size, never decoded pixels.
fn compression_preset(level: i32) -> png::Compression {
    match level {
        0..=1 => png::Compression::Fast,
        8..=9 => png::Compression::Best,
        _ => png::Compression::Default,
    }
}

/// C++ `save_png_libdeflate` / `save_png_streaming`: 8-bit, 1-4 channels,
/// non-interlaced; XMP rides along as an uncompressed iTXt chunk with the
/// standard "XML:com.adobe.xmp" keyword, empty language tag and translated
/// keyword (C++ `build_png_xmp_itxt` shape).
pub(crate) fn save_png(
    image: &Image,
    quality: i32,
    opts: &crate::EncoderOptions,
    xmp: &str,
) -> Result<Vec<u8>, ImgError> {
    let level = png_zlib_level(quality, opts);
    let color_type = color_type(image.channels)?;

    let mut out = Vec::new();
    let mut encoder = png::Encoder::new(&mut out, image.width as u32, image.height as u32);
    encoder.set_color(color_type);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.set_compression(compression_preset(level));
    if !xmp.is_empty() {
        encoder
            .add_itxt_chunk("XML:com.adobe.xmp".to_string(), xmp.to_string())
            .map_err(|_| ImgError::new(20, "PNG encode failed (metadata rejected)"))?;
    }
    let mut writer = encoder
        .write_header()
        .map_err(|_| ImgError::new(20, "PNG encode failed"))?;
    writer
        .write_image_data(&image.data)
        .map_err(|_| ImgError::new(20, "PNG encode failed"))?;
    writer
        .finish()
        .map_err(|_| ImgError::new(20, "PNG encode failed"))?;
    Ok(out)
}
