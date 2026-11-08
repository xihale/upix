//! Shared UI widgets matching `desktop/src/ui_widgets.hpp` ("Darkroom Instrument" aesthetic).

use nsh_core::element::ImageSrc;
use nsh_core::prelude::*;

use crate::theme::{palette, radius, FONT_MONO};

/// Renders an inline SVG icon at `size_px` tinted with `color`.
pub fn svg_icon(markup: &'static str, size_px: f32, color: AdaptiveColor) -> Svg {
    svg(ImageSrc::svg_markup(markup))
        .size(px(size_px))
        .tint_color(color)
}

/// 10px Bold Tracking Section Header (`section_header` in `ui_widgets.hpp`).
pub fn section_header(label: impl Into<String>) -> Text {
    text(label.into().to_uppercase())
        .text_size(px(10.0))
        .font_weight(FontWeight::BOLD)
        .text_color(palette::text_dim())
}

/// Hairline horizontal divider (`hairline_divider` in `ui_widgets.hpp`).
pub fn hairline() -> View {
    view().w_full().h(px(1.0)).bg(palette::hairline())
}

/// Vertical hairline divider.
pub fn v_hairline() -> View {
    view().w(px(1.0)).h_full().bg(palette::hairline())
}

/// Technical status badge (`badge` in `ui_widgets.hpp`).
pub fn badge(label: impl Into<String>, fg: AdaptiveColor, bg: AdaptiveColor) -> View {
    view()
        .px(px(6.0))
        .py(px(2.0))
        .rounded(px(radius::SM))
        .bg(bg)
        .items_center()
        .justify_center()
        .child(
            text(label.into())
                .text_size(px(10.0))
                .font_weight(FontWeight::SEMIBOLD)
                .font_family(FONT_MONO)
                .tabular_nums()
                .text_color(fg),
        )
}

/// Segmented control item descriptor.
pub struct SegmentItem<'a, T: Copy + PartialEq + 'static> {
    pub value: T,
    pub label: &'a str,
}

/// Segmented pill control (`segmented` in `ui_widgets.hpp`).
pub fn segmented<T: Copy + PartialEq + 'static>(
    items: &[SegmentItem<'_, T>],
    current: T,
    on_change: impl Fn(T) + Clone + 'static,
) -> View {
    let mut row = view()
        .flex_row()
        .w_full()
        .h(px(30.0))
        .p(px(2.0))
        .gap(px(2.0))
        .rounded(px(radius::MD))
        .bg(palette::lowest())
        .border_1()
        .border_color(palette::border());

    for item in items {
        let val = item.value;
        let selected = val == current;
        let lbl = item.label.to_string();
        let cb = on_change.clone();

        let btn = view()
            .flex_1()
            .h_full()
            .rounded(px(radius::SM))
            .items_center()
            .justify_center()
            .cursor_pointer()
            .bg(if selected {
                palette::highest()
            } else {
                AdaptiveColor::TRANSPARENT
            })
            .border_1()
            .border_color(if selected {
                palette::border_focus()
            } else {
                AdaptiveColor::TRANSPARENT
            })
            .hover(move |s| {
                s.bg(if selected {
                    palette::highest()
                } else {
                    palette::high()
                })
            })
            .on_press(move || cb(val))
            .child(
                text(lbl)
                    .text_size(px(11.0))
                    .font_weight(if selected {
                        FontWeight::SEMIBOLD
                    } else {
                        FontWeight::NORMAL
                    })
                    .text_color(if selected {
                        palette::accent()
                    } else {
                        palette::text_muted()
                    }),
            );
        row = row.child(btn);
    }

    row
}

/// Primary CTA button (`primary_button` in `ui_widgets.hpp`).
pub fn primary_button(
    label: impl Into<String>,
    enabled: bool,
    on_click: impl Fn() + 'static,
) -> View {
    let lbl = label.into();
    let mut v = view()
        .w_full()
        .h(px(38.0))
        .rounded(px(radius::MD))
        .items_center()
        .justify_center()
        .bg(if enabled {
            palette::accent()
        } else {
            palette::high()
        })
        .border_1()
        .border_color(if enabled {
            palette::accent_hover()
        } else {
            palette::border()
        });

    if enabled {
        v = v
            .cursor_pointer()
            .hover(|s| s.bg(palette::accent_hover()))
            .active(|s| s.bg(palette::accent_active()))
            .on_press(move || on_click());
    }

    v.child(
        text(lbl)
            .text_size(px(13.0))
            .font_weight(FontWeight::BOLD)
            .text_color(if enabled {
                palette::on_accent()
            } else {
                palette::text_dim()
            }),
    )
}

/// Compact Primary button (fixed or auto width).
pub fn primary_button_sm(
    label: impl Into<String>,
    enabled: bool,
    on_click: impl Fn() + 'static,
) -> View {
    let lbl = label.into();
    let mut v = view()
        .h(px(28.0))
        .px(px(12.0))
        .rounded(px(radius::MD))
        .items_center()
        .justify_center()
        .bg(if enabled {
            palette::accent()
        } else {
            palette::high()
        })
        .border_1()
        .border_color(if enabled {
            palette::accent_hover()
        } else {
            palette::border()
        });

    if enabled {
        v = v
            .cursor_pointer()
            .hover(|s| s.bg(palette::accent_hover()))
            .active(|s| s.bg(palette::accent_active()))
            .on_press(move || on_click());
    }

    v.child(
        text(lbl)
            .text_size(px(11.0))
            .font_weight(FontWeight::BOLD)
            .text_color(if enabled {
                palette::on_accent()
            } else {
                palette::text_dim()
            }),
    )
}

/// Secondary ghost / tonal button (`secondary_button` in `ui_widgets.hpp`).
pub fn secondary_button(
    label: impl Into<String>,
    on_click: impl Fn() + 'static,
) -> View {
    view()
        .h(px(28.0))
        .px(px(10.0))
        .rounded(px(radius::MD))
        .items_center()
        .justify_center()
        .bg(palette::high())
        .border_1()
        .border_color(palette::border())
        .cursor_pointer()
        .hover(|s| s.bg(palette::highest()))
        .active(|s| s.bg(palette::surface()))
        .on_press(move || on_click())
        .child(
            text(label.into())
                .text_size(px(11.0))
                .font_weight(FontWeight::MEDIUM)
                .text_color(palette::text()),
        )
}

/// Danger button (`danger_button` in `ui_widgets.hpp`).
pub fn danger_button(
    label: impl Into<String>,
    on_click: impl Fn() + 'static,
) -> View {
    view()
        .h(px(28.0))
        .px(px(10.0))
        .rounded(px(radius::MD))
        .items_center()
        .justify_center()
        .bg(palette::danger_dim())
        .border_1()
        .border_color(palette::danger())
        .cursor_pointer()
        .hover(|s| s.bg(palette::danger()))
        .on_press(move || on_click())
        .child(
            text(label.into())
                .text_size(px(11.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(palette::text()),
        )
}

/// Toggle switch row (`toggle_row` in `ui_widgets.hpp`).
pub fn toggle_row(
    label: impl Into<String>,
    sublabel: impl Into<String>,
    checked: bool,
    on_toggle: impl Fn(bool) + 'static,
) -> View {
    let lbl = label.into();
    let sub = sublabel.into();

    let pill = view()
        .w(px(36.0))
        .h(px(20.0))
        .p(px(2.0))
        .rounded(px(10.0))
        .bg(if checked {
            palette::accent_dim()
        } else {
            palette::lowest()
        })
        .border_1()
        .border_color(if checked {
            palette::accent()
        } else {
            palette::border()
        })
        .flex_row()
        .items_center()
        .justify_start()
        .child(
            view()
                .ml(px(if checked { 16.0 } else { 0.0 }))
                .size(px(14.0))
                .rounded(px(7.0))
                .bg(if checked {
                    palette::accent()
                } else {
                    palette::text_dim()
                }),
        );

    let mut left = view().flex_col().gap(px(2.0)).flex_1().child(
        text(lbl)
            .text_size(px(12.0))
            .font_weight(FontWeight::MEDIUM)
            .text_color(palette::text()),
    );
    if !sub.is_empty() {
        left = left.child(
            text(sub)
                .text_size(px(10.0))
                .text_color(palette::text_muted()),
        );
    }

    view()
        .flex_row()
        .w_full()
        .py(px(4.0))
        .items_center()
        .justify_between()
        .gap(px(8.0))
        .cursor_pointer()
        .on_press(move || on_toggle(!checked))
        .child(left)
        .child(pill)
}

/// Thin technical progress bar (`progress_bar` in `ui_widgets.hpp`).
pub fn progress_bar(fraction: f32) -> View {
    let f = fraction.clamp(0.0, 1.0);
    view()
        .w_full()
        .h(px(6.0))
        .rounded(px(3.0))
        .bg(palette::lowest())
        .border_1()
        .border_color(palette::border())
        .overflow_hidden()
        .child(
            view()
                .h_full()
                .w(percentage(f))
                .rounded(px(3.0))
                .bg(palette::accent()),
        )
}

/// Key-value technical telemetry row.
pub fn kv_row(key: impl Into<String>, val: impl Into<String>) -> View {
    view()
        .flex_row()
        .w_full()
        .items_center()
        .justify_between()
        .py(px(2.0))
        .child(
            text(key.into())
                .text_size(px(11.0))
                .text_color(palette::text_muted()),
        )
        .child(
            text(val.into())
                .text_size(px(11.0))
                .font_family(FONT_MONO)
                .tabular_nums()
                .text_color(palette::text()),
        )
}

pub mod icons {
    /// Camera / Aperture icon for Workbench nav tab.
    pub const WORKBENCH: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8.5" cy="8.5" r="1.5"/><path d="m21 15-5-5L5 21"/></svg>"#;

    /// Layers / Cube icon for Models nav tab.
    pub const MODELS: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/></svg>"#;

    /// Radio / Server icon for Remote Host nav tab.
    pub const HOST: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="2" width="20" height="8" rx="2"/><rect x="2" y="14" width="20" height="8" rx="2"/><line x1="6" y1="6" x2="6.01" y2="6"/><line x1="6" y1="18" x2="6.01" y2="18"/></svg>"#;

    /// Plus icon.
    pub const PLUS: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>"#;

    /// Close / X icon.
    pub const CLOSE: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>"#;

    /// Folder icon.
    pub const FOLDER: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>"#;

    /// Sparkles / Wand icon.
    pub const SPARKLES: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="m12 3-1.9 5.8a2 2 0 0 1-1.3 1.3L3 12l5.8 1.9a2 2 0 0 1 1.3 1.3L12 21l1.9-5.8a2 2 0 0 1 1.3-1.3L21 12l-5.8-1.9a2 2 0 0 1-1.3-1.3Z"/></svg>"#;

    /// Check icon.
    pub const CHECK: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>"#;

    /// Refresh icon.
    pub const REFRESH: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>"#;
}
