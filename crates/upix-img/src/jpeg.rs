//! JPEG decode/encode. Port of the JPEG paths in `legacy/share/utils/image.cpp`
//! (libjpeg reader/writer) onto `zune-jpeg` (decode) / `jpeg-encoder`
//! (encode).

use crate::{Image, ImgError};

/// C++ `load_jpeg`: out_color_space = JCS_RGB (grayscale/CMYK inputs are
/// converted by the decoder), so output is always 3-channel RGB.
pub(crate) fn load_jpeg(buffer: &[u8]) -> Result<Image, ImgError> {
    // JCS_RGB semantics: grayscale/CMYK inputs convert to 3-channel RGB.
    // (zune-jpeg itself lacks a Luma→RGB converter; see the fallback below.)
    let options = zune_jpeg::zune_core::options::DecoderOptions::default()
        .jpeg_set_out_colorspace(zune_jpeg::zune_core::colorspace::ColorSpace::RGB);
    let mut decoder = zune_jpeg::JpegDecoder::new_with_options(buffer, options);
    // zune-jpeg requires headers decoded before dimension queries.
    decoder
        .decode_headers()
        .map_err(|e| ImgError::new(6, format!("Invalid JPEG header: {e}")))?;
    let (width, height) = decoder
        .dimensions()
        .ok_or_else(|| ImgError::new(6, "Invalid JPEG header"))?;
    let pixels = decoder
        .decode()
        .map_err(|e| ImgError::new(6, format!("JPEG decode failed: {e}")))?;
    let width = width as usize;
    let height = height as usize;
    if width == 0 || height == 0 {
        return Err(ImgError::new(6, "Invalid JPEG header"));
    }
    let px_count = width * height;
    // zune-jpeg converts YCbCr→RGB, but has no Luma→RGB path: a grayscale
    // JPEG comes back 1-channel. libjpeg JCS_RGB replicates the gray sample
    // into R, G, B — mirror that here.
    let pixels = if pixels.len() == px_count {
        let mut rgb = Vec::with_capacity(px_count * 3);
        for g in &pixels {
            rgb.push(*g);
            rgb.push(*g);
            rgb.push(*g);
        }
        rgb
    } else {
        pixels
    };
    if pixels.len() != px_count * 3 {
        return Err(ImgError::new(6, "JPEG decode failed: non-RGB output"));
    }
    Ok(Image {
        width: width as i32,
        height: height as i32,
        channels: 3,
        data: pixels,
    })
}

/// C++ JPEG encode: quality 1-100 (clamped), chroma subsampling via sampling
/// factors — anything outside {420,422,444} degrades to the 4:2:0 default;
/// 1-channel input encodes grayscale.
pub(crate) fn save_jpeg(
    image: &Image,
    quality: i32,
    opts: &crate::EncoderOptions,
    xmp: &str,
) -> Result<Vec<u8>, ImgError> {
    let clamped_quality = quality.clamp(1, 100);
    if image.channels != 1 && image.channels != 3 {
        return Err(ImgError::new(15, "JPEG only supports 1 or 3 channels"));
    }
    if image.width <= 0
        || image.height <= 0
        || image.width > 65535
        || image.height > 65535
    {
        return Err(ImgError::new(21, "JPEG encode failed: bad dimensions"));
    }

    let mut encoded = Vec::new();
    let mut encoder = jpeg_encoder::Encoder::new(&mut encoded, clamped_quality as u8);
    if image.channels == 3 {
        // libjpeg default is Y=2x2, Cb/Cr=1x1 = 4:2:0; 422/444 widen the
        // chroma resolution at the cost of file size. jpeg-encoder factor
        // names give the luma component's h/v sampling (chroma stays 1x1).
        let subsampling = if opts.jpeg_subsampling != 422 && opts.jpeg_subsampling != 444 {
            420
        } else {
            opts.jpeg_subsampling
        };
        encoder.set_sampling_factor(match subsampling {
            444 => jpeg_encoder::SamplingFactor::F_1_1,
            422 => jpeg_encoder::SamplingFactor::F_2_1,
            _ => jpeg_encoder::SamplingFactor::F_2_2,
        });
    } else {
        // libjpeg grayscale: single component 1x1.
        encoder.set_sampling_factor(jpeg_encoder::SamplingFactor::F_1_1);
    }

    if !xmp.is_empty() {
        // APP1 with the standard XMP namespace URI prefix (C++
        // jpeg_write_marker with JPEG_APP0+1): the payload includes the NUL
        // terminator of the namespace; the segment length covers the two
        // length bytes. Segment capacity is the 16-bit length field minus
        // those two bytes; an oversized payload is dropped rather than
        // written corrupt (the SR marker is ~200 bytes in practice).
        let payload_len = JPEG_XMP_NAMESPACE.len() + xmp.len();
        if payload_len <= 65533 {
            let mut payload = Vec::with_capacity(payload_len);
            payload.extend_from_slice(&JPEG_XMP_NAMESPACE);
            payload.extend_from_slice(xmp.as_bytes());
            encoder
                .add_app_segment(1, &payload)
                .map_err(|e| ImgError::new(21, format!("JPEG encode failed: {e}")))?;
        }
    }

    let color_type = if image.channels == 1 {
        jpeg_encoder::ColorType::Luma
    } else {
        jpeg_encoder::ColorType::Rgb
    };
    encoder
        .encode(&image.data, image.width as u16, image.height as u16, color_type)
        .map_err(|e| ImgError::new(21, format!("JPEG encode failed: {e}")))?;
    Ok(encoded)
}

/// Standard XMP namespace URI prefix including the NUL (C++
/// `kJpegXmpNamespace`).
const JPEG_XMP_NAMESPACE: &[u8] = b"http://ns.adobe.com/xap/1.0/\0";
