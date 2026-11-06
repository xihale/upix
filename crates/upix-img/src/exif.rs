//! EXIF orientation: JPEG APP1 parse + pixel bake-in. Port of
//! `share/utils/exif.cpp`.
//!
//! The PC decoders hand back the *stored* pixel grid; for portrait shots
//! with orientation 5-8 that grid is rotated relative to what the camera
//! saw. Outputs are re-encoded from pixels and therefore carry no EXIF, so
//! the transform is baked into the decoded pixels — the equivalent of PIL
//! exif_transpose / libvips autorotate. (Other metadata — capture date, GPS
//! — is lost on output either way; pre-existing behavior.)

use crate::Image;

fn be16(p: &[u8]) -> u16 {
    ((p[0] as u16) << 8) | p[1] as u16
}

fn le16(p: &[u8]) -> u16 {
    ((p[1] as u16) << 8) | p[0] as u16
}

/// Orientation out of a TIFF header + IFD0, everything bounds-checked
/// against the segment. Any malformation returns 1 (no-op semantics).
fn orientation_from_tiff(tiff: &[u8]) -> u8 {
    if tiff.len() < 8 {
        return 1;
    }
    let little = match (tiff[0], tiff[1]) {
        (b'I', b'I') => true,
        (b'M', b'M') => false,
        _ => return 1,
    };
    let rd16 = |p: &[u8]| -> u16 {
        if little {
            le16(p)
        } else {
            be16(p)
        }
    };
    let rd32 = |p: &[u8]| -> u32 {
        if little {
            u32::from_le_bytes([p[0], p[1], p[2], p[3]])
        } else {
            u32::from_be_bytes([p[0], p[1], p[2], p[3]])
        }
    };
    if rd16(&tiff[2..]) != 42 {
        return 1; // TIFF magic
    }
    let ifd_off = rd32(&tiff[4..]) as usize;
    if ifd_off + 2 > tiff.len() {
        return 1;
    }
    let entries = rd16(&tiff[ifd_off..]);
    // 12 bytes per entry, accumulated in usize so a hostile entry count can
    // neither overflow nor walk past the segment.
    if ifd_off + 2 + 12 * entries as usize > tiff.len() {
        return 1;
    }
    for i in 0..entries as usize {
        let e = &tiff[ifd_off + 2 + 12 * i..];
        if rd16(e) != 0x0112 {
            continue; // Orientation
        }
        if rd16(&e[2..]) != 3 {
            continue; // type must be SHORT
        }
        if rd32(&e[4..]) < 1 {
            continue; // count
        }
        // count == 1: the value sits in the first two bytes of the value
        // field (left-justified, endian-following).
        let value = rd16(&e[8..]);
        return if (1..=8).contains(&value) {
            value as u8
        } else {
            1
        };
    }
    1
}

/// Scan a JPEG byte stream for the APP1 "Exif\0\0" segment and read the
/// IFD0 Orientation tag. Returns 1-8; returns 1 (no-op semantics) for
/// non-JPEG input, missing/unreadable EXIF, or any parse error — callers
/// treat 1 as "nothing to do". Only the header region (up to SOS) is read;
/// hand-rolled with strict bounds checks, safe on truncated/hostile input.
pub fn parse_jpeg_exif_orientation(data: &[u8]) -> u8 {
    if data.len() < 4 {
        return 1;
    }
    if data[0] != 0xFF || data[1] != 0xD8 {
        return 1; // not a JPEG (SOI)
    }
    let len = data.len();
    let mut pos = 2usize;
    while pos + 4 <= len {
        if data[pos] != 0xFF {
            return 1; // desynced from the marker stream
        }
        let mut m = pos + 1;
        while m < len && data[m] == 0xFF {
            m += 1; // fill bytes before the marker code
        }
        if m >= len {
            return 1;
        }
        let marker = data[m];
        // Standalone markers (TEM, RSTn) carry no length; SOI mid-stream is
        // malformed — all bail or skip without payload.
        if marker == 0x01 || (0xD0..=0xD8).contains(&marker) {
            pos = m + 1;
            continue;
        }
        if marker == 0xDA || marker == 0xD9 {
            return 1; // SOS/EOI: EXIF must precede the entropy-coded data
        }
        if len - m < 3 {
            return 1; // truncated length field
        }
        let seg_len = be16(&data[m + 1..]) as usize; // includes its own 2 bytes
        if seg_len < 2 {
            return 1;
        }
        let payload = seg_len - 2;
        // Layout: FF(at pos) marker(at m) len_hi len_lo payload... — the
        // payload starts three bytes past the marker code.
        let seg_start = m + 3;
        if payload > len - seg_start {
            return 1; // truncated segment
        }
        if marker == 0xE1
            && payload >= 6
            && &data[seg_start..seg_start + 6] == b"Exif\0\0"
        {
            return orientation_from_tiff(&data[seg_start + 6..seg_start + payload]);
        }
        pos = seg_start + payload;
    }
    1
}

// Mirror left-right (orientation 2), in place.
fn mirror_horizontal(p: &mut [u8], w: usize, h: usize, c: usize) {
    let stride = w * c;
    for y in 0..h {
        let row = &mut p[y * stride..(y + 1) * stride];
        for x in 0..w / 2 {
            let a = x * c;
            let b = (w - 1 - x) * c;
            for k in 0..c {
                row.swap(a + k, b + k);
            }
        }
    }
}

// Mirror top-bottom (orientation 4), in place.
fn mirror_vertical(p: &mut [u8], w: usize, h: usize, c: usize) {
    let stride = w * c;
    let mut tmp = vec![0u8; stride];
    for y in 0..h / 2 {
        let a = y * stride;
        let b = (h - 1 - y) * stride;
        tmp.copy_from_slice(&p[a..a + stride]);
        p.copy_within(b..b + stride, a);
        p[a..a + stride].copy_from_slice(&tmp);
    }
}

// 180° (orientation 3), in place: reverse the whole pixel sequence.
fn rotate_180(p: &mut [u8], w: usize, h: usize, c: usize) {
    let n = w * h;
    for i in 0..n / 2 {
        let a = i * c;
        let b = (n - 1 - i) * c;
        for k in 0..c {
            p.swap(a + k, b + k);
        }
    }
}

// Transposing orientations (5-8): dst receives the swapped-geometry frame
// (width' = h, height' = w).
//
//   out(x, y) = in(y, x)               orientation 5 (transpose)
//   out(x, y) = in(y, h-1-x)           orientation 6 (rotate 90° CW)
//   out(x, y) = in(w-1-y, h-1-x)       orientation 7 (transverse)
//   out(x, y) = in(w-1-y, x)           orientation 8 (rotate 90° CCW)
fn transpose_transform(src: &[u8], dst: &mut [u8], w: usize, h: usize, c: usize, orientation: u8) {
    let ow = h;
    let oh = w;
    let ostride = ow * c;
    for y in 0..oh {
        let row = y * ostride;
        for x in 0..ow {
            let (sx, sy) = match orientation {
                5 => (y, x),
                6 => (y, h - 1 - x),
                7 => (w - 1 - y, h - 1 - x),
                _ => (w - 1 - y, x), // 8
            };
            let s = (sy * w + sx) * c;
            let d = row + x * c;
            dst[d..d + c].copy_from_slice(&src[s..s + c]);
        }
    }
}

/// Apply the orientation transform in place to 8-bit interleaved pixels
/// (1/3/4 channels). `pixels` must hold at least w*h*channels bytes; extra
/// tail bytes are preserved. Orientations 2-4 are geometry-preserving flips;
/// 5-8 transpose and swap w/h. Returns true when a transform was applied;
/// false (pixels untouched) for orientation 1 or invalid arguments.
pub fn apply_exif_orientation_u8(
    pixels: &mut Vec<u8>,
    w: &mut i32,
    h: &mut i32,
    channels: i32,
    orientation: i32,
) -> bool {
    if !(2..=8).contains(&orientation) {
        return false;
    }
    if *w <= 0 || *h <= 0 || channels <= 0 {
        return false;
    }
    let (wu, hu, cu) = (*w as usize, *h as usize, channels as usize);
    let needed = wu * hu * cu;
    if pixels.len() < needed {
        return false;
    }
    match orientation {
        2 => mirror_horizontal(pixels, wu, hu, cu),
        3 => rotate_180(pixels, wu, hu, cu),
        4 => mirror_vertical(pixels, wu, hu, cu),
        o => {
            // 5-8 swap the geometry; transform through a scratch frame and
            // copy back so any tail slack beyond w*h*c survives untouched.
            let mut dst = vec![0u8; needed];
            transpose_transform(&pixels[..needed], &mut dst, wu, hu, cu, o as u8);
            pixels[..needed].copy_from_slice(&dst);
            std::mem::swap(w, h);
        }
    }
    true
}

/// Same transform for a decoded [`Image`]: flips run in place, the
/// transposing orientations rebuild the frame. Returns true when the image
/// changed.
pub fn autorotate_image(image: &mut Image, orientation: i32) -> bool {
    if !(2..=8).contains(&orientation) {
        return false;
    }
    let w = image.width;
    let h = image.height;
    let c = image.channels;
    if w <= 0 || h <= 0 || c <= 0 {
        return false;
    }
    if orientation <= 4 {
        match orientation {
            2 => mirror_horizontal(&mut image.data, w as usize, h as usize, c as usize),
            3 => rotate_180(&mut image.data, w as usize, h as usize, c as usize),
            _ => mirror_vertical(&mut image.data, w as usize, h as usize, c as usize),
        }
        return true;
    }
    // 5-8 transpose: build a fresh frame and move it in — one pixel copy.
    let mut rotated = Image::new(h, w, c);
    transpose_transform(
        &image.data,
        &mut rotated.data,
        w as usize,
        h as usize,
        c as usize,
        orientation as u8,
    );
    *image = rotated;
    true
}
