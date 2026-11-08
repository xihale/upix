#![allow(dead_code)]

use nsh_core::prelude::AdaptiveColor;

pub const FONT_SANS: &str = "Noto Sans CJK SC, Source Han Sans SC, PingFang SC, Microsoft YaHei, Inter, sans-serif";
pub const FONT_MONO: &str = "JetBrains Mono, JetBrainsMonoNL Nerd Font Mono, JetBrainsMono Nerd Font Mono, DejaVu Sans Mono, Noto Sans CJK SC, monospace";

pub mod radius {
    pub const SM: f32 = 4.0;
    pub const MD: f32 = 6.0;
    pub const LG: f32 = 10.0;
    pub const XL: f32 = 14.0;
}

/// 暗房仪器 (Darkroom Instrument) design system tokens.
/// Matches `desktop/src/ui_theme.hpp` in both Dark and Light modes.
pub mod palette {
    use super::AdaptiveColor;

    #[inline]
    pub const fn bg() -> AdaptiveColor {
        AdaptiveColor::rgb(0xF4F3EF, 0x131316)
    }

    #[inline]
    pub const fn recessed() -> AdaptiveColor {
        AdaptiveColor::rgb(0xEAE9E4, 0x151519)
    }

    #[inline]
    pub const fn lowest() -> AdaptiveColor {
        recessed()
    }

    #[inline]
    pub const fn surface() -> AdaptiveColor {
        AdaptiveColor::rgb(0xFFFFFF, 0x18181C)
    }

    #[inline]
    pub const fn container_low() -> AdaptiveColor {
        AdaptiveColor::rgb(0xF0EFEB, 0x1C1C21)
    }

    #[inline]
    pub const fn container() -> AdaptiveColor {
        AdaptiveColor::rgb(0xECEBE6, 0x202026)
    }

    #[inline]
    pub const fn high() -> AdaptiveColor {
        AdaptiveColor::rgb(0xE3E2DC, 0x26262C)
    }

    #[inline]
    pub const fn seg_selected() -> AdaptiveColor {
        AdaptiveColor::rgb(0xFFFFFF, 0x2B2B34)
    }

    #[inline]
    pub const fn seg_border() -> AdaptiveColor {
        AdaptiveColor::rgb(0xC5C4BC, 0x383842)
    }

    #[inline]
    pub const fn highest() -> AdaptiveColor {
        AdaptiveColor::rgb(0xD9D8D1, 0x323239)
    }

    #[inline]
    pub const fn outline() -> AdaptiveColor {
        AdaptiveColor::rgb(0xD6D5CD, 0x282830)
    }

    #[inline]
    pub const fn hairline() -> AdaptiveColor {
        outline()
    }

    #[inline]
    pub const fn border() -> AdaptiveColor {
        outline()
    }

    #[inline]
    pub const fn outline_input() -> AdaptiveColor {
        AdaptiveColor::rgb(0xCECDC5, 0x2D2D34)
    }

    #[inline]
    pub const fn outline_strong() -> AdaptiveColor {
        AdaptiveColor::rgb(0xC5C4BC, 0x32323C)
    }

    #[inline]
    pub const fn border_focus() -> AdaptiveColor {
        outline_strong()
    }

    #[inline]
    pub const fn reg_mark() -> AdaptiveColor {
        AdaptiveColor::rgb(0xBEBDB4, 0x343440)
    }

    #[inline]
    pub const fn on_surface() -> AdaptiveColor {
        AdaptiveColor::rgb(0x18181B, 0xECECEE)
    }

    #[inline]
    pub const fn text() -> AdaptiveColor {
        on_surface()
    }

    #[inline]
    pub const fn muted() -> AdaptiveColor {
        AdaptiveColor::rgb(0x5E5D57, 0xA2A2AC)
    }

    #[inline]
    pub const fn text_muted() -> AdaptiveColor {
        muted()
    }

    #[inline]
    pub const fn dim() -> AdaptiveColor {
        AdaptiveColor::rgb(0x8A8982, 0x6E6E7A)
    }

    #[inline]
    pub const fn text_dim() -> AdaptiveColor {
        dim()
    }

    #[inline]
    pub const fn primary() -> AdaptiveColor {
        AdaptiveColor::rgb(0x0E6B5F, 0x7FDAC9)
    }

    #[inline]
    pub const fn accent() -> AdaptiveColor {
        primary()
    }

    #[inline]
    pub const fn primary_hover() -> AdaptiveColor {
        AdaptiveColor::rgb(0x0B574D, 0x8EE4D4)
    }

    #[inline]
    pub const fn accent_hover() -> AdaptiveColor {
        primary_hover()
    }

    #[inline]
    pub const fn accent_active() -> AdaptiveColor {
        AdaptiveColor::rgb(0x08463E, 0x67C6B5)
    }

    #[inline]
    pub const fn primary_dim_bg() -> AdaptiveColor {
        AdaptiveColor::rgb(0xDDF2EE, 0x152623)
    }

    #[inline]
    pub const fn accent_dim() -> AdaptiveColor {
        primary_dim_bg()
    }

    #[inline]
    pub const fn primary_dim_border() -> AdaptiveColor {
        AdaptiveColor::rgb(0x9FD5CA, 0x244740)
    }

    #[inline]
    pub const fn toggle_on_bg() -> AdaptiveColor {
        AdaptiveColor::rgb(0xC5ECE4, 0x18302C)
    }

    #[inline]
    pub const fn toggle_on_border() -> AdaptiveColor {
        AdaptiveColor::rgb(0x85CABB, 0x2E5E55)
    }

    #[inline]
    pub const fn on_primary() -> AdaptiveColor {
        AdaptiveColor::rgb(0xFFFFFF, 0x00382F)
    }

    #[inline]
    pub const fn on_accent() -> AdaptiveColor {
        on_primary()
    }

    #[inline]
    pub const fn error() -> AdaptiveColor {
        AdaptiveColor::rgb(0xC23A2B, 0xFF7361)
    }

    #[inline]
    pub const fn danger() -> AdaptiveColor {
        error()
    }

    #[inline]
    pub const fn error_dim_bg() -> AdaptiveColor {
        AdaptiveColor::rgb(0xFCE9E7, 0x1E1416)
    }

    #[inline]
    pub const fn danger_dim() -> AdaptiveColor {
        error_dim_bg()
    }

    #[inline]
    pub const fn error_dim_border() -> AdaptiveColor {
        AdaptiveColor::rgb(0xE8B4AE, 0x58282C)
    }

    #[inline]
    pub const fn star_gold() -> AdaptiveColor {
        AdaptiveColor::rgb(0xC69214, 0xF5D08A)
    }

    #[inline]
    pub const fn scene_anime() -> AdaptiveColor {
        AdaptiveColor::rgb(0x8B5CF6, 0xC4B5FD)
    }

    #[inline]
    pub const fn scene_photo() -> AdaptiveColor {
        AdaptiveColor::rgb(0xD97706, 0xFCD34D)
    }

    #[inline]
    pub const fn scene_general() -> AdaptiveColor {
        AdaptiveColor::rgb(0x0E6B5F, 0x7FDAC9)
    }

    #[inline]
    pub const fn scene_restore() -> AdaptiveColor {
        AdaptiveColor::rgb(0xDB2777, 0xF9A8D4)
    }

    #[inline]
    pub const fn qr_paper() -> AdaptiveColor {
        AdaptiveColor::rgb(0xF4F3EF, 0xF4F3EF)
    }

    #[inline]
    pub const fn qr_ink() -> AdaptiveColor {
        AdaptiveColor::rgb(0x131316, 0x131316)
    }
}
