//! 对比: before/after with a draggable divider — the desktop CompareStage.qml
//! semantics ported 1:1:
//!
//! - pos clamps 0.02..0.98 (desktop's drag clamp);
//! - a press only grabs within 14 px of the rule; while grabbed, every move
//!   follows the gesture delta from the grab point, so the divider never
//!   lags fast drags (the desktop's "drag tracking" fix — the grab band
//!   spans the stage, a press elsewhere leaves pos alone);
//! - the seam, the 1 px rule and the 28 pt handle all anchor on ONE rounded
//!   column (`divider_px = round(width * pos)`), never a doubled line;
//! - a new result sweeps 0.04 → 0.62 over ~720 ms with an ease-out curve
//!   (desktop's sweepBg). The desktop's arrow-key nudge is omitted: nsh key
//!   bindings are app-level (`on_key`), not focus-scoped to a view.
//!
//! The after element crops natively: objectFit Cover in a box shorter than
//! the image with a left-top anchor shows exactly the left slice at the
//! same scale as the fully-fitted before image (box aspect = image aspect).

use nsh_base::theme;
use nsh_core::geometry::Points;
use nsh_core::prelude::*;
use nsh_core::{ObjectPosition, PositionAxis};

use crate::store::Store;
use crate::ui;

/// Drag clamp bounds and grab band, straight from CompareStage.qml.
pub const POS_MIN: f32 = 0.02;
pub const POS_MAX: f32 = 0.98;
pub const GRAB_BAND_PX: f32 = 14.0;
pub const SWEEP_FROM: f32 = 0.04;
pub const SWEEP_TO: f32 = 0.62;

/// The one clamp both the drag and the sweep use.
pub fn clamp_pos(v: f32) -> f32 {
    v.clamp(POS_MIN, POS_MAX)
}

/// One rounded column for seam/rule/handle (desktop's `dividerPx`).
pub fn divider_px(width: f32, pos: f32) -> f32 {
    (width * pos).round()
}

pub fn page(store: Store) -> impl IntoElement {
    // Local to the page: divider pos and layout sizes. What must survive a
    // page switch (input/result) lives in the store.
    let pos = signal(clamp_pos(0.62));
    let stage_size = stored((0.0_f32, 0.0_f32));
    let grab_x = stored(0.0_f32);
    let swept_for = stored(None::<String>);

    scroll().flex_1().child(
        view().p_4().gap_4().child(ui::page_title("对比")).child(
            ui::card_with(move || {
                let Some(input) = store.input.get() else {
                    return text("先在「首页」选择一张图片")
                        .text_sm()
                        .text_color(move || theme().muted_foreground)
                        .into_element();
                };
                let result = store.result.get();
                let after_src = result.as_ref().map(|r| r.display_src.clone());

                // Sweep once per new result (CompareStage's sweep()).
                let result_key = result.as_ref().map(|r| r.path.display().to_string());
                if swept_for.get() != result_key {
                    swept_for.replace(result_key.clone());
                    if result_key.is_some() {
                        sweep(pos);
                    }
                }

                stage_view(
                    store,
                    input.display_src.clone(),
                    after_src,
                    pos,
                    stage_size,
                    grab_x,
                )
                .into_element()
            }),
        ),
    )
}

/// The image stage: before fills the box; the after element is as wide as
/// the divider column and crops at its own bounds (left-anchored cover),
/// so seam, rule and handle share one column.
fn stage_view(
    store: Store,
    before_src: String,
    after_src: Option<String>,
    pos: Signal<f32>,
    stage_size: Stored<(f32, f32)>,
    grab_x: Stored<f32>,
) -> impl IntoElement {
    // Box aspect = image aspect (after when present, else the input), so the
    // cover crop of the after element matches the fitted before image 1:1.
    let aspect = move || {
        let dims = store
            .result
            .get()
            .map(|r| (r.width as f32, r.height as f32))
            .or_else(|| store.input.get().map(|i| (i.width as f32, i.height as f32)));
        match dims {
            Some((w, h)) if h > 0.0 => w / h,
            _ => 1.0,
        }
    };
    let stage_w = move || stage_size.get().0;
    let stage_h = move || stage_size.get().1;
    let box_w = move || {
        let (sw, sh) = (stage_w(), stage_h());
        if sw > 0.0 && sh > 0.0 {
            sw.min(sh * aspect()).max(120.0)
        } else {
            320.0
        }
    };
    let box_h = move || box_w() / aspect().max(0.0001);
    let divider = move || divider_px(box_w(), pos.get());
    let after = after_src.clone();

    view()
        .w_full()
        .items_center()
        .justify_center()
        // A fixed stage height keeps the fitted box math non-circular.
        .h(px(420.0))
        .on_layout(move |bounds: &Bounds<Points>| {
            stage_size.replace((bounds.size.width.0, bounds.size.height.0));
        })
        .child(
            view()
                .relative()
                .overflow_hidden()
                .rounded(px(12.0))
                .w(move || px(box_w()))
                .h(move || px(box_h()))
                .bg(move || theme().muted)
                // Before: the whole fitted image.
                .child(
                    image(move || before_src.clone())
                        .absolute()
                        .top(px(0.0))
                        .left(px(0.0))
                        .w(move || px(box_w()))
                        .h(move || px(box_h()))
                        .object_fit(ResizeMode::Stretch),
                )
                // After: width = divider column; left-anchored cover crops the
                // rest at the element bounds — the left slice at full scale.
                .child(
                    image(move || after.clone().unwrap_or_default())
                        .absolute()
                        .top(px(0.0))
                        .left(px(0.0))
                        .w(move || px(divider()))
                        .h(move || px(box_h()))
                        .object_fit(ResizeMode::Cover)
                        .object_position(ObjectPosition::new(
                            PositionAxis::START,
                            PositionAxis::START,
                        )),
                )
                // The 1 px rule on the same column.
                .child(
                    view()
                        .absolute()
                        .top(px(0.0))
                        .bottom(px(0.0))
                        .left(move || px(divider()))
                        .w(px(1.0))
                        .bg(move || theme().foreground)
                        .opacity(0.9),
                )
                // The 28 pt handle, centered on the column.
                .child(
                    view()
                        .absolute()
                        .size(px(28.0))
                        .left(move || px(divider() - 14.0))
                        .top(move || px(box_h() / 2.0 - 14.0))
                        .rounded(px(8.0))
                        .bg(move || theme().card)
                        .border_1()
                        .border_color(move || theme().border)
                        .items_center()
                        .justify_center()
                        .child(
                            text("‹ ›")
                                .text_xs()
                                .text_color(move || theme().muted_foreground),
                        ),
                )
                .child(chip_label(Side::Left, "原图"))
                .child(chip_label(Side::Right, "结果"))
                .child(result_pills(store))
                // The grab band spans the whole stage (the desktop's fix: a
                // band riding on the divider collapses pos on every press).
                .child(
                    view()
                        .absolute()
                        .top(px(0.0))
                        .left(px(0.0))
                        .right(px(0.0))
                        .bottom(px(0.0))
                        .cursor_pointer()
                        .claim_touches(TouchClaim::on_start().ask(move |request: &ClaimRequest| {
                            // Grab only within 14 px of the rule; anything
                            // else keeps presses for chips/pills.
                            let dx = (request.event.location.x.0 - divider()).abs();
                            if dx <= GRAB_BAND_PX {
                                request.claim();
                            }
                        }))
                        .on_responder_grant(move |e: &ResponderEvent| {
                            grab_x.replace(e.location.x.0);
                            pos.set(clamp_pos(e.location.x.0 / box_w().max(1.0)));
                        })
                        .on_responder_move(move |e: &ResponderEvent| {
                            // Follow the delta from the grab point: the
                            // pointer cannot "leave the band" mid-drag.
                            let x = grab_x.get() + e.gesture.delta.x.0;
                            pos.set(clamp_pos(x / box_w().max(1.0)));
                        }),
                ),
        )
        .child(
            text(move || format!("分割线 {:.0}% · 拖动分割线对比", pos.get() * 100.0))
                .text_xs()
                .text_color(move || theme().muted_foreground),
        )
}

#[derive(Clone, Copy)]
enum Side {
    Left,
    Right,
}

fn chip_label(side: Side, label: &'static str) -> nsh_core::element::View {
    let base = view()
        .absolute()
        .top(px(12.0))
        .px_2()
        .py(px(3.0))
        .rounded(px(6.0))
        .bg(move || theme().card)
        .border_1()
        .border_color(move || theme().border)
        .opacity(0.92)
        .child(text(label).text_xs().text_color(move || theme().muted_foreground));
    match side {
        Side::Left => base.left(px(12.0)),
        Side::Right => base.right(px(12.0)),
    }
}

/// 打开 / 显示 over the result path (desktop pills).
fn result_pills(store: Store) -> nsh_core::element::View {
    let Some(result) = store.result.get() else {
        return view();
    };
    let path_for_open = result.path.clone();
    let path_for_reveal = result.path.clone();
    view()
        .absolute()
        .bottom(px(12.0))
        .right(px(12.0))
        .flex_row()
        .gap(px(6.0))
        .child(ui::ghost_button("打开", move || {
            let path = path_for_open.clone();
            spawn(async move {
                let _ = open_path(path).await;
            });
        }))
        .child(ui::ghost_button("显示", move || {
            let path = path_for_reveal.clone();
            spawn(async move {
                let _ = reveal_path(path).await;
            });
        }))
}

/// 0.04 → 0.62, ease-out cubic, ~720 ms in 24 frames (sweepBg).
fn sweep(pos: Signal<f32>) {
    spawn(async move {
        let steps = 24u32;
        for step in 1..=steps {
            let t = step as f32 / steps as f32;
            let eased = 1.0 - (1.0 - t).powi(3);
            pos.set(SWEEP_FROM + (SWEEP_TO - SWEEP_FROM) * eased);
            timer(std::time::Duration::from_millis(30)).await;
        }
    });
}
