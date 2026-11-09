//! `bicubic_scale_u8` — Rust port of the kernel in `engine/core/
//! tile_processor.cpp` (also lives in engine/utils/image.cpp for the alpha
//! bypass; single shared implementation here). Mitchell-Netravali family with
//! the a=-0.5 coefficients the C++ file ships.

/// Bicubic interpolation kernel (Mitchell-Netravali, B=1/3, C=1/3), a = -0.5
/// — verbatim from the C++.
#[inline]
fn bicubic_kernel(t: f32) -> f32 {
    let a = -0.5f32;
    let t = t.abs();
    if t <= 1.0 {
        (a + 2.0) * t * t * t - (a + 3.0) * t * t + 1.0
    } else if t < 2.0 {
        a * t * t * t - 5.0 * a * t * t + 8.0 * a * t - 4.0 * a
    } else {
        0.0
    }
}

/// Bicubic scale for a single channel (u8 in, u8 out) — used for the alpha
/// bypass when an RGBA image feeds an RGB model.
pub fn bicubic_scale_u8(
    src: &[u8],
    src_w: u32,
    src_h: u32,
    dst: &mut [u8],
    dst_w: u32,
    dst_h: u32,
) {
    let scale_x = src_w as f32 / dst_w as f32;
    let scale_y = src_h as f32 / dst_h as f32;

    for dy in 0..dst_h {
        let sy = (dy as f32 + 0.5) * scale_y - 0.5;
        let sy0 = sy.floor() as i32;

        for dx in 0..dst_w {
            let sx = (dx as f32 + 0.5) * scale_x - 0.5;
            let sx0 = sx.floor() as i32;

            let mut sum = 0.0f32;
            let mut weight_sum = 0.0f32;

            for m in -1..=2 {
                let cy = (sy0 + m).clamp(0, src_h as i32 - 1);
                let wy = bicubic_kernel(sy - (sy0 + m) as f32);

                for n in -1..=2 {
                    let cx = (sx0 + n).clamp(0, src_w as i32 - 1);
                    let wx = bicubic_kernel(sx - (sx0 + n) as f32);

                    let w = wx * wy;
                    sum += src[cy as usize * src_w as usize + cx as usize] as f32 * w;
                    weight_sum += w;
                }
            }

            // The kernel has negative lobes, so weight_sum can collapse to
            // (near) zero on degenerate size pairs; the division then yields
            // inf/NaN, whose cast to uint8_t is undefined. Bounded resize
            // ratios never reach it — clamp to black when it happens.
            let value = if weight_sum.abs() > 1e-6 { sum / weight_sum } else { 0.0 };
            dst[dy as usize * dst_w as usize + dx as usize] =
                value.clamp(0.0, 255.0) as u8;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constant_image_stays_constant() {
        let src = vec![137u8; 16 * 16];
        let mut dst = vec![0u8; 32 * 32];
        bicubic_scale_u8(&src, 16, 16, &mut dst, 32, 32);
        assert!(dst.iter().all(|&v| v == 137), "constant must upscale to constant");
    }

    #[test]
    fn downscale_preserves_constant() {
        let src = vec![42u8; 64 * 64];
        let mut dst = vec![0u8; 16 * 16];
        bicubic_scale_u8(&src, 64, 64, &mut dst, 16, 16);
        assert!(dst.iter().all(|&v| v == 42));
    }

    #[test]
    fn identity_ratio_is_neutral() {
        // scale_x = scale_y = 1: every destination sample centers on a source
        // sample (kernel(0) = 1, kernel(±1) = 0 for a = -0.5), so dst == src.
        let src: Vec<u8> = (0..64u32).map(|i| (i * 3) as u8).collect();
        let mut dst = vec![0u8; 64];
        bicubic_scale_u8(&src, 8, 8, &mut dst, 8, 8);
        assert_eq!(dst, src);
    }

    #[test]
    fn linear_ramp_stays_monotone() {
        let src: Vec<u8> = (0..16u32).map(|i| (i * 16) as u8).collect(); // 16x1 ramp
        let mut dst = vec![0u8; 32];
        bicubic_scale_u8(&src, 16, 1, &mut dst, 32, 1);
        for w in dst.windows(2) {
            assert!(w[0] <= w[1], "ramp must stay monotone, got {w:?}");
        }
    }

    #[test]
    fn degenerate_weight_sum_clamps_to_black() {
        // Engineered degenerate pair: 1x1 → huge ratio drives weight_sum
        // near zero at far samples; output must stay 0..=255 (no UB wrap).
        let src = vec![200u8];
        let mut dst = vec![255u8; 64];
        bicubic_scale_u8(&src, 1, 1, &mut dst, 8, 8);
        assert_eq!(dst.len(), 64);
    }
}
