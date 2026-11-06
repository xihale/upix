//! QR rendering: payload → `qrcode` matrix → grayscale PNG → `data:` URI for
//! the image element. Scanners need a light ground with dark modules (the
//! CLI's terminal QR forces SGR 47/30 for the same polarity reason), so the
//! PNG is always white background with dark modules and a 4-module quiet
//! zone.

use base64::Engine as _;
use qrcode::{Color, EcLevel, QrCode, Version};

/// Renders `payload` as an ECC-M QR (the desktop vendor's level) in a PNG
/// data URI. `Err` carries a user-facing message.
pub fn qr_png_data_uri(payload: &str) -> Result<String, String> {
    let bytes = qr_png(payload)?;
    Ok(format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    ))
}

/// QR matrix → PNG bytes (8-bit grayscale, quiet zone 4). Picks the smallest
/// version that fits (`with_version` answers `Err`, never panics).
pub fn qr_png(payload: &str) -> Result<Vec<u8>, String> {
    let mut picked = None;
    for v in 1..=15i16 {
        if let Ok(code) = QrCode::with_version(
            payload.as_bytes(),
            Version::Normal(v),
            EcLevel::M,
        ) {
            picked = Some(code);
            break;
        }
    }
    let code = picked.ok_or_else(|| "qrcode: payload 太长".to_owned())?;
    let modules = code.width();
    let colors = code.to_colors();
    let quiet = 4usize;
    let size = (modules + quiet * 2) as u32;

    let mut rows: Vec<u8> = Vec::with_capacity((size * size) as usize);
    let dark = |y: usize, x: usize| -> bool {
        let (my, mx) = (y as i64 - quiet as i64, x as i64 - quiet as i64);
        my >= 0
            && mx >= 0
            && (my as usize) < modules
            && (mx as usize) < modules
            && colors[my as usize * modules + mx as usize] == Color::Dark
    };
    for y in 0..size as usize {
        for x in 0..size as usize {
            rows.push(if dark(y, x) { 0 } else { 255 });
        }
    }

    let mut out = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut out, size, size);
        encoder.set_color(png::ColorType::Grayscale);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().map_err(|e| format!("png: {e}"))?;
        writer
            .write_image_data(&rows)
            .map_err(|e| format!("png: {e}"))?;
    }
    Ok(out)
}
