//! Shared UI widgets matching `desktop/src/ui_widgets.hpp` and Figma "Upix · Desktop 当前布局".

use nsh_core::element::ImageSrc;
use nsh_core::prelude::*;
use nsh_core::{LayoutCurve, LayoutTransition};

use crate::theme::{palette, radius, FONT_MONO};

/// Renders an inline SVG icon at `size_px` tinted with `color`.
pub fn svg_icon(markup: &'static str, size_px: f32, color: AdaptiveColor) -> Svg {
    svg(ImageSrc::svg_markup(markup))
        .size(px(size_px))
        .tint_color(color)
}

/// 12px Medium Section Header (`模型`, `输出`, `引擎与处理` in Figma Inspector).
pub fn section_header(label: impl Into<String>) -> Text {
    text(label.into())
        .text_size(px(12.0))
        .font_weight(FontWeight::MEDIUM)
        .text_color(palette::muted())
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

/// Segmented control matching Figma "Desktop / Segmented" (`#151519` track, `#25252D` border,
/// `8px` radius, `2px` padding, `#2B2B34` active with `#383842` border, `6px` radius,
/// and `1x12px` `#282832` divider between unselected items), with a native spring-animated
/// sliding indicator (`View::follow` + `LayoutCurve::Spring`).
pub fn segmented<T: Copy + PartialEq + 'static>(
    items: &[SegmentItem<'_, T>],
    current: Signal<T>,
    on_change: impl Fn(T) + Clone + 'static,
) -> View {
    segmented_sized(items, current, 32.0, 12.0, on_change)
}

/// Segmented control with custom height and font size.
pub fn segmented_sized<T: Copy + PartialEq + 'static>(
    items: &[SegmentItem<'_, T>],
    current: Signal<T>,
    height_px: f32,
    font_size_px: f32,
    on_change: impl Fn(T) + Clone + 'static,
) -> View {
    segmented_with_font(items, current, height_px, font_size_px, false, on_change)
}

/// Segmented control with custom height, font size, and optional monospace font (`FONT_MONO`).
///
/// The segment nodes remain mounted across clicks (never torn down via `dynamic`), so the
/// active indicator pill glides smoothly between segments using `nsh`'s native layout spring.
pub fn segmented_with_font<T: Copy + PartialEq + 'static>(
    items: &[SegmentItem<'_, T>],
    current: Signal<T>,
    height_px: f32,
    font_size_px: f32,
    mono: bool,
    on_change: impl Fn(T) + Clone + 'static,
) -> View {
    const SEG_SPRING: LayoutTransition =
        LayoutTransition::new(LayoutCurve::Spring { damping: 0.86 }, 250.0);

    let refs: Vec<(T, ElementRef)> = items.iter().map(|it| (it.value, element_ref())).collect();
    let values: Vec<T> = items.iter().map(|it| it.value).collect();
    let refs_for_follow = refs.clone();

    // Sliding active background pill (`#2B2B34`, 1px border `#FFFFFF14`, 6px radius, shadow_sm)
    let pill = view()
        .absolute()
        .top(px(2.0))
        .bottom(px(2.0))
        .rounded(px(6.0))
        .bg(palette::seg_selected())
        .border_1()
        .border_color(AdaptiveColor::new(rgba(0xC5C4BCFF), rgba(0xFFFFFF14)))
        .shadow_sm()
        .pointer_events(PointerEvents::None)
        .follow(move || {
            let cur = current.get();
            refs_for_follow
                .iter()
                .find(|(v, _)| *v == cur)
                .or_else(|| refs_for_follow.first())
                .map(|(_, r)| *r)
        })
        .follow_transition(move || (!reduce_motion()).then_some(SEG_SPRING));

    let mut row = view()
        .flex_row()
        .w_full()
        .h(px(height_px))
        .p(px(2.0))
        .relative()
        .items_center()
        .rounded(px(8.0))
        .bg(palette::recessed())
        .border_1()
        .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x25252D))
        .child(pill);

    for (idx, item) in items.iter().enumerate() {
        let val = item.value;
        let el_ref = refs[idx].1;
        let lbl = item.label.to_string();
        let cb = on_change.clone();

        // Hairline 1x12px divider between every segment; smoothly fades out when adjacent to active item
        // while keeping fixed 1px width so segment layout never shifts.
        if idx > 0 {
            let prev_val = values[idx - 1];
            row = row.child(
                view()
                    .w(px(1.0))
                    .h(px(12.0))
                    .flex_shrink_0()
                    .transition_colors()
                    .duration(150.0)
                    .ease_out()
                    .bg(move || {
                        let cur = current.get();
                        if cur == val || cur == prev_val {
                            AdaptiveColor::TRANSPARENT
                        } else {
                            AdaptiveColor::rgb(0xD6D5CD, 0x282832)
                        }
                    }),
            );
        }

        let mut lbl_text = text(lbl)
            .text_size(px(font_size_px))
            .font_weight(FontWeight::MEDIUM)
            .tabular_nums()
            .transition_colors()
            .duration(150.0)
            .ease_out()
            .text_color(move || {
                if current.get() == val {
                    palette::on_surface()
                } else {
                    palette::muted()
                }
            });
        if mono {
            lbl_text = lbl_text.font_family(FONT_MONO);
        }

        let btn = view()
            .element_ref(&el_ref)
            .flex_1()
            .h_full()
            .rounded(px(6.0))
            .items_center()
            .justify_center()
            .cursor_pointer()
            .transition()
            .duration(140.0)
            .ease_out()
            .hover(|s| s.bg(AdaptiveColor::new(rgba(0x0000000A), rgba(0xFFFFFF07))))
            .active(|s| if reduce_motion() { s } else { s.scale(0.97) })
            .on_press(move || cb(val))
            .child(lbl_text);
        row = row.child(btn);
    }

    row
}

/// Quality slider with 5 tick marks and magnetic snapping matching Figma "Desktop / Slider / Quality"
/// (`276x18`, `2px` track `#2D2D34`, `2px` `#7FDAC9` fill, `12x12` `#131316` thumb with `2.5px` `#7FDAC9` ring,
/// and 5 `1x3px` `#3A3A44` ticks at `top: 13px`).
pub fn quality_slider(
    value: Signal<i32>,
    min_val: i32,
    max_val: i32,
) -> View {
    let rail = signal(Bounds::<Points>::default());
    let drag_origin = stored(None::<f32>);
    let range = (max_val - min_val).max(1) as f32;

    // 5 quantile snap targets: 0%, 25%, 50%, 75%, 100%
    const SNAP_FRACS: [f32; 5] = [0.0, 0.25, 0.50, 0.75, 1.0];
    // Magnetic snap threshold (~2.2% of track width, i.e. ~6px on a 276px track)
    const SNAP_THRESHOLD: f32 = 0.022;

    let set_at = move |local_x: f32| {
        let w = rail.get_untracked().size.width.0.max(12.0);
        let mut frac = (local_x / w).clamp(0.0, 1.0);
        for &target in &SNAP_FRACS {
            if (frac - target).abs() <= SNAP_THRESHOLD {
                frac = target;
                break;
            }
        }
        let next = if frac <= 0.0 {
            min_val
        } else if frac >= 1.0 {
            max_val
        } else {
            ((frac * 100.0).round() as i32).clamp(min_val, max_val)
        };
        if value.get_untracked() != next {
            value.set(next);
        }
    };

    let mut ticks_row = view()
        .absolute()
        .left(px(1.0))
        .right(px(1.0))
        .top(px(13.0))
        .h(px(3.0))
        .flex_row()
        .items_center()
        .justify_between()
        .pointer_events(PointerEvents::None);
    for _ in 0..5 {
        ticks_row = ticks_row.child(
            view()
                .w(px(1.0))
                .h(px(3.0))
                .bg(AdaptiveColor::rgb(0xBEBDB4, 0x3A3A44)),
        );
    }

    view()
        .w_full()
        .h(px(18.0))
        .relative()
        .cursor_pointer()
        .on_layout(move |b: &Bounds<Points>| {
            rail.set(*b);
        })
        .claim_touches(TouchClaim::on_start())
        .keep_touches(true)
        .on_responder_grant(move |e: &ResponderEvent| {
            let r = rail.get_untracked();
            let local_x = e.location.x.0 - r.origin.x.0;
            let _ = drag_origin.replace(Some(local_x));
            set_at(local_x);
        })
        .on_responder_move(move |e: &ResponderEvent| {
            if let Some(start_x) = drag_origin.get() {
                set_at(start_x + e.gesture.delta.x.0);
            }
        })
        .on_responder_release(move |_: &ResponderEvent| {
            let _ = drag_origin.replace(None);
        })
        .on_responder_terminate(move |_: &ResponderEvent| {
            let _ = drag_origin.replace(None);
        })
        // 2px track background at y = 8px
        .child(
            view()
                .absolute()
                .left(px(0.0))
                .right(px(0.0))
                .top(px(8.0))
                .h(px(2.0))
                .rounded(px(1.0))
                .bg(palette::outline_input())
                .pointer_events(PointerEvents::None),
        )
        // 2px active progress fill at y = 8px
        .child(
            view()
                .absolute()
                .left(px(0.0))
                .top(px(8.0))
                .h(px(2.0))
                .w(move || {
                    let w = rail.get().size.width.0.max(12.0);
                    let v = value.get();
                    let t = ((v - min_val) as f32 / range).clamp(0.0, 1.0);
                    px((w * t).round())
                })
                .rounded(px(1.0))
                .bg(palette::primary())
                .pointer_events(PointerEvents::None),
        )
        // 5 tick marks at top = 13px
        .child(ticks_row)
        // 12x12 thumb centered on (t * width, y = 9px)
        .child(
            view()
                .absolute()
                .top(px(3.0))
                .left(move || {
                    let w = rail.get().size.width.0.max(12.0);
                    let v = value.get();
                    let t = ((v - min_val) as f32 / range).clamp(0.0, 1.0);
                    let cx = (t * w).clamp(6.0, w - 6.0);
                    px((cx - 6.0).round())
                })
                .size(px(12.0))
                .rounded_full()
                .bg(palette::bg())
                .border(px(2.5))
                .border_color(palette::primary())
                .pointer_events(PointerEvents::None),
        )
}

/// Square icon button matching `iconButton` in `ui_widgets.hpp`.
pub fn icon_button(
    icon_markup: &'static str,
    box_size: f32,
    icon_size: f32,
    radius_px: f32,
    on_click: impl Fn() + 'static,
) -> View {
    view()
        .size(px(box_size))
        .rounded(px(radius_px))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline())
        .transition()
        .duration(140.0)
        .ease_out()
        .hover(|s| s.bg(palette::high()).border_color(palette::outline_strong()))
        .active(|s| if reduce_motion() { s } else { s.scale(0.94) })
        .items_center()
        .justify_center()
        .cursor_pointer()
        .on_press(move || on_click())
        .child(svg_icon(icon_markup, icon_size, palette::muted()))
}

/// Toggle switch matching Figma "Desktop / Toggle" (`44x26`, `20x20` thumb, `3px` padding).
///
/// Keeps the track and thumb nodes alive across toggles so the thumb glides smoothly
/// via `translate_x` (`ease-bezier(0.22, 1.0, 0.36, 1.0)`) and colors cross-fade.
pub fn toggle_switch(
    is_checked: impl Fn() -> bool + Copy + 'static,
    on_toggle: impl Fn(bool) + 'static,
) -> View {
    view()
        .w(px(44.0))
        .h(px(26.0))
        .p(px(2.0))
        .rounded(px(13.0))
        .transition_colors()
        .duration(180.0)
        .ease_out()
        .bg(move || {
            if is_checked() {
                palette::toggle_on_bg()
            } else {
                palette::high()
            }
        })
        .border_1()
        .border_color(move || {
            if is_checked() {
                palette::toggle_on_border()
            } else {
                palette::outline_input()
            }
        })
        .flex_row()
        .items_center()
        .justify_start()
        .cursor_pointer()
        .active(|s| if reduce_motion() { s } else { s.scale(0.96) })
        .on_press(move || {
            let cur = untracked(is_checked);
            on_toggle(!cur);
        })
        .child(
            view()
                .translate_x(move || px(if is_checked() { 18.0 } else { 0.0 }))
                .transition()
                .duration(185.0)
                .ease_bezier(0.22, 1.0, 0.36, 1.0)
                .size(px(20.0))
                .rounded(px(10.0))
                .bg(move || {
                    if is_checked() {
                        palette::primary()
                    } else {
                        palette::on_surface()
                    }
                }),
        )
}

/// Toggle switch row (`toggle_row` in `ui_widgets.hpp`, matching Figma Inspector rows).
pub fn toggle_row(
    label: impl Into<String>,
    sublabel: impl Into<String>,
    is_checked: impl Fn() -> bool + Copy + 'static,
    on_toggle: impl Fn(bool) + 'static,
) -> View {
    let lbl = label.into();
    let sub = sublabel.into();

    let mut left = view().flex_col().gap(px(2.0)).flex_1().child(
        text(lbl)
            .text_size(px(14.0))
            .font_weight(FontWeight::MEDIUM)
            .text_color(palette::on_surface()),
    );
    if !sub.is_empty() {
        left = left.child(
            text(sub)
                .text_size(px(11.0))
                .text_color(palette::muted()),
        );
    }

    view()
        .flex_row()
        .w_full()
        .min_h(px(28.0))
        .items_center()
        .justify_between()
        .gap(px(8.0))
        .child(left)
        .child(toggle_switch(is_checked, on_toggle))
}

/// Primary CTA button (`276x44` in Figma Inspector).
pub fn primary_button(
    label: impl Into<String>,
    enabled: bool,
    on_click: impl Fn() + 'static,
) -> View {
    let lbl = label.into();
    let mut v = view()
        .w_full()
        .h(px(44.0))
        .rounded(px(8.0))
        .flex_row()
        .items_center()
        .justify_center()
        .gap(px(8.0))
        .transition()
        .duration(140.0)
        .ease_out()
        .bg(if enabled {
            palette::primary()
        } else {
            palette::high()
        });

    if enabled {
        v = v
            .cursor_pointer()
            .hover(|s| s.bg(palette::primary_hover()))
            .active(|s| {
                let s = s.bg(palette::accent_active());
                if reduce_motion() { s } else { s.scale(0.985) }
            })
            .on_press(move || on_click());
    }

    let ink = if enabled {
        palette::on_primary()
    } else {
        palette::text_dim()
    };

    v.child(svg_icon(icons::PLAY, 15.0, ink)).child(
        text(lbl)
            .text_size(px(14.0))
            .font_weight(FontWeight::MEDIUM)
            .text_color(ink),
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
        .transition()
        .duration(140.0)
        .ease_out()
        .hover(|s| s.bg(palette::highest()))
        .active(|s| {
            let s = s.bg(palette::surface());
            if reduce_motion() { s } else { s.scale(0.97) }
        })
        .on_press(move || on_click())
        .child(
            text(label.into())
                .text_size(px(11.0))
                .font_weight(FontWeight::MEDIUM)
                .text_color(palette::text()),
        )
}

/// Thin technical progress bar (`progress_bar` in `ui_widgets.hpp`).
pub fn progress_bar(fraction: f32) -> View {
    let f = fraction.clamp(0.0, 1.0);
    view()
        .w_full()
        .h(px(4.0))
        .rounded(px(2.0))
        .bg(palette::lowest())
        .overflow_hidden()
        .child(
            view()
                .h_full()
                .w(percentage(f))
                .rounded(px(2.0))
                .bg(palette::accent()),
        )
}

pub mod icons {
    /// Zoom-in (magnifying glass with `+`) for Workbench nav tab (`放大`) and Empty Stage.
    pub const ZOOM_IN: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="7.5"/><line x1="21" y1="21" x2="16.4" y2="16.4"/><line x1="11" y1="8" x2="11" y2="14"/><line x1="8" y1="11" x2="14" y2="11"/></svg>"#;

    /// Alias kept for compatibility.
    pub const WORKBENCH: &str = ZOOM_IN;

    /// Layers (stacked diamonds) for Models nav tab (`模型`).
    pub const LAYERS: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 3 21 7.5 12 12 3 7.5 12 3"/><polyline points="3 12.5 12 17 21 12.5"/><polyline points="3 17 12 21.5 21 17"/></svg>"#;

    /// Alias kept for compatibility.
    pub const MODELS: &str = LAYERS;

    /// Smartphone for Remote Host nav tab (`远程`) and Pairing Sheet modal.
    pub const SMARTPHONE: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="6.5" y="2.5" width="11" height="19" rx="2.2"/><circle cx="12" cy="18.2" r="1" fill="currentColor"/></svg>"#;

    /// Alias kept for compatibility.
    pub const HOST: &str = SMARTPHONE;

    /// Filled Play triangle (`启动服务` / `开始超分`).
    pub const PLAY: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="currentColor" stroke="none"><polygon points="6.5 4.8 19 12 6.5 19.2"/></svg>"#;

    /// Outlined Play triangle (`启动服务`).
    pub const PLAY_OUTLINE: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><polygon points="6.5 4.8 19 12 6.5 19.2"/></svg>"#;

    /// Filled Stop square (`停止服务` / `取消超分`).
    pub const STOP: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>"#;

    /// Chevron-down icon.
    pub const CHEVRON_DOWN: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>"#;

    /// Search magnifying glass icon.
    pub const SEARCH: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="6.8"/><line x1="20" y1="20" x2="15.8" y2="15.8"/></svg>"#;

    /// Download / Import arrow icon.
    pub const DOWNLOAD: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="4" x2="12" y2="15.5"/><polyline points="7.5 11 12 15.5 16.5 11"/><line x1="5" y1="19.5" x2="19" y2="19.5"/></svg>"#;

    /// Trash can icon (`#5E5E6A`).
    pub const TRASH: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><line x1="4" y1="6.5" x2="20" y2="6.5"/><line x1="9" y1="4" x2="15" y2="4"/><polygon points="6.5 6.5 17.5 6.5 16.5 20 7.5 20"/><line x1="10.2" y1="10.5" x2="10.2" y2="16.5"/><line x1="13.8" y1="10.5" x2="13.8" y2="16.5"/></svg>"#;

    /// Refresh circular arrow icon.
    pub const REFRESH: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>"#;

    /// Copy overlapping rectangles icon.
    pub const COPY: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="8.5" y="8.5" width="11" height="11" rx="1.5"/><path d="M15.5 4.5H6a1.5 1.5 0 0 0-1.5 1.5v9.5"/></svg>"#;

    /// QR Code icon (`qr-code` in `ui_widgets.hpp`).
    pub const QR_CODE: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="6.5" height="6.5" rx="1"/><rect x="14.5" y="3" width="6.5" height="6.5" rx="1"/><rect x="3" y="14.5" width="6.5" height="6.5" rx="1"/><rect x="14.5" y="14.5" width="3" height="3" fill="currentColor" stroke="none"/><rect x="18" y="18" width="3" height="3" fill="currentColor" stroke="none"/></svg>"#;

    /// Zap lightning bolt icon (warm daemon active indicator).
    pub const ZAP: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="currentColor" stroke="none"><polygon points="13.5 2.5 4.5 13.5 12 13.5 10.5 21.5 19.5 10.5 12 10.5"/></svg>"#;

    /// Plus icon.
    pub const PLUS: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>"#;

    /// Close / X icon.
    pub const CLOSE: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>"#;

    /// Folder icon.
    pub const FOLDER: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>"#;

    /// Check icon.
    pub const CHECK: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>"#;

    /// Filled 5-point Star icon (matching `desktop/src/ui_widgets.hpp` `"star"`).
    pub const STAR: &str = r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="currentColor" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"><polygon points="12 2.5 14.9 8.6 21.6 9.5 16.7 14.2 17.9 20.9 12 17.7 6.1 20.9 7.3 14.2 2.4 9.5 9.1 8.6 12 2.5"/></svg>"#;

    /// 4-frame rotating loader arc for warm daemon startup animation.
    pub const LOADER_FRAMES: [&str; 4] = [
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 3a9 9 0 1 1-6.36 2.64" opacity="0.25"/><path d="M12 3a9 9 0 0 1 9 9"/></svg>"#,
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-2.64-6.36" opacity="0.25"/><path d="M21 12a9 9 0 0 1-9 9"/></svg>"#,
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 21a9 9 0 1 1 6.36-2.64" opacity="0.25"/><path d="M12 21a9 9 0 0 1-9-9"/></svg>"#,
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M3 12a9 9 0 1 1 2.64 6.36" opacity="0.25"/><path d="M3 12a9 9 0 0 1 9-9"/></svg>"#,
    ];
}
