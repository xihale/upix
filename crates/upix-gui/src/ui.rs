//! The app's styled controls: each base component wrapped once, drawn with
//! theme tokens. Pages call these; they never restyle base parts themselves.

use nsh_base::{button, input, theme};
use nsh_core::prelude::*;

/// Primary action (开始超分, 启动主机…).
pub fn primary_button(
    label: impl IntoReactive<SharedString>,
    on_press: impl Fn() + 'static,
) -> nsh_base::ButtonElement {
    button()
        .on_press(on_press)
        .h(px(36.0))
        .px_4()
        .items_center()
        .justify_center()
        .rounded(move || theme().radius)
        .bg(move || theme().primary)
        .active(|s| s.opacity(0.85))
        .focus(|s| s.outline(px(2.0)).outline_color(move || theme().ring))
        .disabled_style(|s| s.opacity(0.5))
        .child(
            text(label)
                .text_sm()
                .font_weight(FontWeight::MEDIUM)
                .text_color(move || theme().primary_foreground),
        )
}

/// Quiet action (取消, 刷新, 停止主机…).
pub fn ghost_button(
    label: impl IntoReactive<SharedString>,
    on_press: impl Fn() + 'static,
) -> nsh_base::ButtonElement {
    button()
        .on_press(on_press)
        .h(px(36.0))
        .px_4()
        .items_center()
        .justify_center()
        .rounded(move || theme().radius)
        .border_1()
        .border_color(move || theme().border)
        .bg(move || theme().background)
        .hover(|s| s.bg(move || theme().accent))
        .active(|s| s.opacity(0.85))
        .focus(|s| s.outline(px(2.0)).outline_color(move || theme().ring))
        .disabled_style(|s| s.opacity(0.5))
        .child(text(label).text_sm().text_color(move || theme().foreground))
}

/// Destructive action (拒绝配对).
pub fn destructive_button(
    label: impl IntoReactive<SharedString>,
    on_press: impl Fn() + 'static,
) -> nsh_base::ButtonElement {
    button()
        .on_press(on_press)
        .h(px(36.0))
        .px_4()
        .items_center()
        .justify_center()
        .rounded(move || theme().radius)
        .bg(move || theme().destructive)
        .active(|s| s.opacity(0.85))
        .disabled_style(|s| s.opacity(0.5))
        .child(
            text(label)
                .text_sm()
                .text_color(move || theme().background),
        )
}

/// A grouped surface (pairing panel, model rows).
pub fn card() -> nsh_core::element::View {
    view()
        .w_full()
        .rounded(px(12.0))
        .border_1()
        .border_color(move || theme().border)
        .bg(move || theme().card)
}

/// Single-line text field in the app's look (path input fallback).
pub fn field(
    placeholder: &'static str,
    value: Signal<String>,
    on_submit: impl Fn(&str) + 'static + Clone,
) -> nsh_base::Input {
    input()
        .value(move || value.get())
        .on_change(move |text: &str| value.set(text.to_owned()))
        .on_submit(move |text: &str| (on_submit.clone())(text))
        .placeholder(placeholder)
        .placeholder_color(move || color(theme().muted_foreground))
        .caret_color(move || color(theme().primary))
        .h(px(36.0))
        .px_3()
        .text_sm()
        .rounded(move || theme().radius)
        .border_1()
        .border_color(move || theme().input)
        .focus(|s| s.border_color(move || theme().ring))
}

/// Small rounded label: scene chips (动漫/照片…), 推荐, stage markers.
pub fn chip(
    label: impl IntoReactive<SharedString>,
    ink: impl IntoReactive<AdaptiveColor> + 'static,
) -> impl IntoElement {
    view()
        .px_2()
        .py(px(2.0))
        .rounded(px(6.0))
        .bg(move || theme().secondary)
        .child(text(label).text_xs().text_color(ink))
}

/// Page title.
pub fn page_title(label: &'static str) -> impl IntoElement {
    text(label)
        .text_lg()
        .font_weight(FontWeight::MEDIUM)
        .text_color(move || theme().foreground)
}

/// Section caption (已安装 / 可下载 / 设备指纹…).
pub fn section_label(label: &'static str) -> impl IntoElement {
    text(label)
        .text_xs()
        .font_weight(FontWeight::MEDIUM)
        .text_color(move || theme().muted_foreground)
}

/// Monospaced value (fingerprints, paths, ports).
pub fn mono(value: impl IntoReactive<SharedString>) -> nsh_core::element::Text {
    text(value)
        .text_xs()
        .font_family(move || mono_font())
        .tabular_nums()
}

/// A status dot: on = primary, off = border.
pub fn status_dot(on: impl Fn() -> bool + 'static) -> impl IntoElement {
    view()
        .size(px(8.0))
        .rounded_full()
        .bg(move || {
            if on() {
                theme().primary
            } else {
                theme().border
            }
        })
}

/// A card body inside the app's card look. Pages call `ui::card_with(...)`.
pub fn card_with<F, O>(body: F) -> nsh_core::element::View
where
    F: Fn() -> O + 'static,
    O: IntoElement,
{
    card().p_4().child(body())
}

/// Determinate progress bar, 3px track with primary fill (desktop 的 Progress.qml).
pub fn progress_bar(value: impl IntoReactive<f32>) -> impl IntoElement {
    nsh_base::progress()
        .value(value)
        .h(px(3.0))
        .track_style(|s| s.rounded_full().bg(move || theme().secondary))
        .indicator_style(|s| s.rounded_full().bg(move || theme().primary))
}

/// The theme's mono family, or the system monospace when the theme sets none.
pub fn mono_font() -> SharedString {
    theme()
        .font_mono
        .clone()
        .unwrap_or_else(|| SharedString::from("monospace"))
}
