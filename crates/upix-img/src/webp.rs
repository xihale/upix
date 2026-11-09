//! WebP decode/encode. Port of the WebP paths in `legacy/share/utils/image.cpp`
//! (libwebp + libwebpmux) onto the `webp` crate (vendored libwebp) with a
//! hand-rolled VP8X/"XMP " mux post-pass.

use crate::{Image, ImgError};

/// C++ `load_webp`: decode as RGB (alpha dropped, MODE_RGB semantics).
pub(crate) fn load_webp(buffer: &[u8]) -> Result<Image, ImgError> {
    let decoder = webp::Decoder::new(buffer);
    let img = match decoder.decode() {
        Some(img) => img,
        None => {
            // The crate's simple decoder rejects extended (VP8X) containers,
            // which the C++ WebPDecode path accepted (metadata chunks ride
            // along). Strip to the plain ALPH?+VP8/VP8L bitstream and retry.
            let simple = strip_webp_metadata(buffer)
                .ok_or_else(|| ImgError::new(7, "Invalid WebP header"))?;
            webp::Decoder::new(&simple)
                .decode()
                .ok_or_else(|| ImgError::new(7, "Invalid WebP header"))?
        }
    };
    let (width, height) = (img.width() as i32, img.height() as i32);
    if width <= 0 || height <= 0 {
        return Err(ImgError::new(7, "Invalid WebP header"));
    }
    let rgb = img.to_image().to_rgb8().into_raw();
    Ok(Image {
        width,
        height,
        channels: 3,
        data: rgb,
    })
}

/// Rebuild a simple (non-VP8X) WebP file containing only the image chunks
/// (ALPH/VP8/VP8L) of an extended container. Metadata-only chunks are
/// dropped; decoded pixels are unaffected.
fn strip_webp_metadata(buffer: &[u8]) -> Option<Vec<u8>> {
    if buffer.len() < 20 || &buffer[0..4] != b"RIFF" || &buffer[8..12] != b"WEBP" {
        return None;
    }
    let riff_size = u32::from_le_bytes(buffer[4..8].try_into().ok()?) as usize;
    let body_end = (riff_size + 8).min(buffer.len());
    let mut offset = 12usize;
    let mut chunks = Vec::new();
    while offset + 8 <= body_end {
        let ctype = &buffer[offset..offset + 4];
        let csize =
            u32::from_le_bytes(buffer[offset + 4..offset + 8].try_into().ok()?) as usize;
        let payload = offset + 8;
        let Some(chunk_end) = payload.checked_add(csize + (csize & 1)) else {
            return None;
        };
        if chunk_end > body_end {
            return None;
        }
        if ctype == b"ALPH" || ctype == b"VP8 " || ctype == b"VP8L" {
            chunks.extend_from_slice(&buffer[offset..chunk_end]);
        }
        offset = chunk_end;
    }
    if chunks.is_empty() {
        return None;
    }
    let mut out = Vec::with_capacity(chunks.len() + 12);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&((chunks.len() + 4) as u32).to_le_bytes());
    out.extend_from_slice(b"WEBP");
    out.extend_from_slice(&chunks);
    Some(out)
}

/// C++ WebP save: Picture API with quality/lossless/method knobs
/// (webp_method >= 0 overrides the libwebp default 4, clamped 0-6), then the
/// XMP payload muxed as a "XMP " chunk in an extended (VP8X) container;
/// mux failure degrades to an unmarked file (historical behavior).
pub(crate) fn save_webp(
    image: &Image,
    quality: i32,
    opts: &crate::EncoderOptions,
    xmp: &str,
) -> Result<Vec<u8>, ImgError> {
    let clamped_quality = quality.clamp(1, 100);
    if image.channels != 3 && image.channels != 4 {
        return Err(ImgError::new(18, "WebP save requires 3 or 4 channels"));
    }
    if image.width <= 0 || image.height <= 0 {
        return Err(ImgError::new(19, "WebP encode failed"));
    }

    let mut config = webp::WebPConfig::new()
        .map_err(|_| ImgError::new(19, "WebP encoder init failed"))?;
    config.quality = clamped_quality as f32;
    config.lossless = opts.webp_lossless as std::ffi::c_int;
    // Picture API thread_level: parallelizes entropy coding across tiles
    // like `cwebp -threads` (the C++ code sets it unconditionally).
    config.thread_level = 1;
    if opts.webp_method >= 0 {
        // Trade encode time for file size (libwebp default is 4).
        config.method = opts.webp_method.clamp(0, 6);
    }

    let mem = if image.channels == 3 {
        webp::Encoder::from_rgb(&image.data, image.width as u32, image.height as u32)
    } else {
        webp::Encoder::from_rgba(&image.data, image.width as u32, image.height as u32)
    }
    .encode_advanced(&config)
    .map_err(|_| ImgError::new(19, "WebP encode failed"))?;
    if mem.is_empty() {
        return Err(ImgError::new(19, "WebP encode failed"));
    }
    let mut encoded = mem.to_vec();

    if !xmp.is_empty() && !webp_attach_xmp(&mut encoded, xmp) {
        log::warn!("WebP XMP mux failed; writing without marker");
    }
    Ok(encoded)
}

fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_u24(out: &mut Vec<u8>, value: u32) {
    out.push((value >> 16) as u8);
    out.push((value >> 8) as u8);
    out.push(value as u8);
}

/// Canvas dimensions out of the leading chunks of a (simple, possibly
/// alpha-bearing) encoded WebP bitstream. Chunk layout is type(4) +
/// size(4) + payload, so dimensions live at payload offsets. VP8L: 14-bit
/// LE dims+1; VP8 keyframe: 14-bit dims at the sync-code offset; an ALPH
/// chunk precedes the opaque VP8 chunk in lossy+alpha files.
fn bitstream_dimensions(chunks: &[u8]) -> Option<(u32, u32)> {
    if chunks.len() < 8 {
        return None;
    }
    let ctype = &chunks[0..4];
    let csize = u32::from_le_bytes(chunks[4..8].try_into().ok()?) as usize;
    if ctype == b"VP8L" {
        return vp8l_dims(&chunks[8..]);
    }
    if ctype == b"VP8 " {
        return keyframe_dims(&chunks[8..]);
    }
    if ctype == b"ALPH" {
        let next = 8 + csize + (csize & 1);
        let rest = chunks.get(next..)?;
        if rest.len() < 8 || &rest[0..4] != b"VP8 " {
            return None;
        }
        return keyframe_dims(&rest[8..]);
    }
    None
}

/// VP8L bitstream: signature 0x2f, then 14-bit LE width-1 / height-1.
fn vp8l_dims(p: &[u8]) -> Option<(u32, u32)> {
    if p.is_empty() || p[0] != 0x2F {
        return None;
    }
    let bits = u32::from_le_bytes(p[1..5].try_into().ok()?);
    Some(((bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1))
}

/// VP8 keyframe header: 3-byte frame tag, sync 0x9d 0x01 0x2a, then 14-bit
/// LE width / height (bit 15 of each is the scale flag).
fn keyframe_dims(p: &[u8]) -> Option<(u32, u32)> {
    if p.len() < 10 || p[3] != 0x9D || p[4] != 0x01 || p[5] != 0x2A {
        return None;
    }
    let w = ((p[6] as u32) | (p[7] as u32) << 8) & 0x3FFF;
    let h = ((p[8] as u32) | (p[9] as u32) << 8) & 0x3FFF;
    Some((w, h))
}

/// Wrap the encoded WebP into the extended (VP8X) container and attach the
/// XMP payload as its "XMP " chunk (C++ `webp_attach_xmp`). Returns false
/// (encoded untouched) on mux errors; callers degrade to an unmarked file.
fn webp_attach_xmp(encoded: &mut Vec<u8>, xmp: &str) -> bool {
    if encoded.len() < 20 || &encoded[0..4] != b"RIFF" || &encoded[8..12] != b"WEBP" {
        return false;
    }
    let riff_size = u32::from_le_bytes(encoded[4..8].try_into().unwrap()) as usize;
    let body_end = (riff_size + 8).min(encoded.len());

    // Walk chunks to the first image chunk (VP8/VP8L); note any ALPH chunk
    // so the VP8X alpha flag reflects the reused bitstream.
    let mut offset = 12usize;
    let mut has_alpha = false;
    let mut image_chunks_start = 12usize;
    let mut image_chunks_end = body_end;
    let mut saw_image = false;
    while offset + 8 <= body_end {
        let ctype = &encoded[offset..offset + 4];
        let csize =
            u32::from_le_bytes(encoded[offset + 4..offset + 8].try_into().unwrap()) as usize;
        let payload = offset + 8;
        let Some(payload_end) = payload.checked_add(csize) else {
            return false;
        };
        if payload_end > body_end {
            return false;
        }
        if ctype == b"VP8 " || ctype == b"VP8L" {
            saw_image = true;
            image_chunks_start = offset;
            image_chunks_end = payload_end + (csize & 1); // even-size padding
            break;
        }
        if ctype == b"ALPH" {
            has_alpha = true;
        }
        offset = payload_end + (csize & 1);
    }
    if !saw_image {
        return false;
    }

    // VP8X inputs would need chunk-level rewrite; WebPMuxSetImage in C++
    // rebuilds the container, so mirror that by bailing on already-extended
    // files and dropping the marker (encode path here never produces VP8X).
    let image_chunks = &encoded[image_chunks_start..image_chunks_end];
    if image_chunks.windows(4).any(|w| w == b"VP8X") {
        return false;
    }
    let Some((w, h)) = bitstream_dimensions(image_chunks) else {
        return false;
    };
    if w == 0 || h == 0 {
        return false;
    }

    let xmp_bytes = xmp.as_bytes();
    let mut out = Vec::with_capacity(encoded.len() + xmp_bytes.len() + 30);
    out.extend_from_slice(b"RIFF");
    let riff_placeholder = out.len();
    put_u32(&mut out, 0);
    out.extend_from_slice(b"WEBP");

    // VP8X: flags byte (XMP 0x04, alpha 0x10) + 3 reserved + canvas
    // w-1/h-1 as 24-bit LE.
    out.extend_from_slice(b"VP8X");
    put_u32(&mut out, 10);
    out.push(0x04 | if has_alpha { 0x10 } else { 0x00 });
    out.extend_from_slice(&[0, 0, 0]);
    put_u24(&mut out, w - 1);
    put_u24(&mut out, h - 1);

    // "XMP " chunk (padded to even size like every RIFF chunk).
    out.extend_from_slice(b"XMP ");
    put_u32(&mut out, xmp_bytes.len() as u32);
    out.extend_from_slice(xmp_bytes);
    if xmp_bytes.len() & 1 == 1 {
        out.push(0);
    }

    out.extend_from_slice(image_chunks);
    let riff_size = (out.len() - 8) as u32;
    out[riff_placeholder..riff_placeholder + 4].copy_from_slice(&riff_size.to_le_bytes());

    *encoded = out;
    true
}
