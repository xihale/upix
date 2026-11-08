//! Workbench page (`desktop/src/main.cpp` `render_workbench_tab`):
//! Left Stage Well (corner registration marks, before/after split curtain, 40px telemetry bar,
//! 54px filmstrip dock) + Right 308px Inspector (floating model picker, format/quality/chroma/speed,
//! output directory, GPU/CPU, tile size, warm daemon, force overwrite, sticky CTA).

use std::path::PathBuf;

use nsh_core::element::input;
use nsh_core::file_dialog::{FileFilter, OpenDialog};
use nsh_core::prelude::*;

use crate::engine_bridge::{ChromaSubsampling, OutputFormat};
use crate::models_local::{human_bytes, scale_from_filename, short_name};
use crate::nav::NavTab;
use crate::store::{file_uri_path, InputItem, ItemStatus, JobPhase, Store};
use crate::theme::{palette, FONT_MONO};
use crate::ui::{
    hairline, primary_button, section_header, segmented, svg_icon, toggle_row, SegmentItem,
};

pub const POS_MIN: f32 = 0.02;
pub const POS_MAX: f32 = 0.98;
pub const GRAB_BAND_PX: f32 = 14.0;

#[inline]
pub fn clamp_pos(pos: f32) -> f32 {
    pos.clamp(POS_MIN, POS_MAX)
}

#[inline]
pub fn divider_px(width: f32, pos: f32) -> f32 {
    if width <= 0.0 {
        0.0
    } else {
        (width * clamp_pos(pos)).round()
    }
}

pub fn workbench_page(store: Store) -> View {
    view()
        .flex_1()
        .h_full()
        .flex_row()
        .bg(palette::bg())
        .child(stage_column(store))
        .child(inspector_column(store))
}

fn pick_image_files(store: Store) {
    let picked = pick_paths(
        OpenDialog::files()
            .multiple(true)
            .filter(FileFilter::new(
                "图片",
                ["png", "jpg", "jpeg", "webp", "avif"],
            ))
            .button_label("选择图片"),
    );
    spawn(async move {
        if let Ok(Some(paths)) = picked.await {
            store.add_paths(&paths);
        }
    });
}

fn pick_batch_folder(store: Store) {
    let picked = pick_paths(OpenDialog::folders().button_label("选择文件夹"));
    spawn(async move {
        if let Ok(Some(dirs)) = picked.await {
            let mut files = Vec::new();
            for dir in dirs {
                if let Ok(entries) = std::fs::read_dir(dir) {
                    for entry in entries.flatten() {
                        let p = entry.path();
                        if p.is_file() {
                            files.push(p);
                        }
                    }
                }
            }
            files.sort();
            store.add_paths(&files);
        }
    });
}

fn pick_export_folder(store: Store) {
    let picked = pick_paths(OpenDialog::folders().button_label("选择导出目录"));
    spawn(async move {
        if let Ok(Some(dirs)) = picked.await {
            if let Some(first) = dirs.into_iter().next() {
                store.custom_out_dir.set(Some(first));
            }
        }
    });
}

fn stage_column(store: Store) -> View {
    view()
        .flex_1()
        .h_full()
        .p(px(12.0))
        .gap(px(8.0))
        .flex_col()
        .on_file_drop(move |e: &FileDropEvent| {
            let paths: Vec<PathBuf> = e.uris.iter().filter_map(|u| file_uri_path(u)).collect();
            store.add_paths(&paths);
        })
        // Main Stage Well
        .child(
            view()
                .flex_1()
                .w_full()
                .rounded(px(12.0))
                .bg(palette::surface())
                .border_1()
                .border_color(palette::outline())
                .overflow_hidden()
                .relative()
                .child(dynamic(move || {
                    if store.inputs.get().is_empty() {
                        empty_stage_well(store)
                    } else {
                        active_stage_well(store)
                    }
                })),
        )
        // Minimal Filmstrip Dock (visible when batch > 1 or Running/Failed)
        .child(dynamic(move || {
            let items = store.inputs.get();
            let phase = store.job_phase.get();
            if items.len() > 1
                || matches!(phase, JobPhase::Running { .. } | JobPhase::Failed(_))
            {
                filmstrip_dock(store, items)
            } else {
                view().h(px(0.0))
            }
        }))
}

fn registration_marks() -> View {
    let c = palette::reg_mark();
    view()
        .absolute()
        .inset(px(16.0))
        .pointer_events(PointerEvents::None)
        // Top-left
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(12.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(1.0))
                .h(px(12.0))
                .bg(c),
        )
        // Top-right
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(12.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(1.0))
                .h(px(12.0))
                .bg(c),
        )
        // Bottom-left
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(12.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(1.0))
                .h(px(12.0))
                .bg(c),
        )
        // Bottom-right
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(12.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(1.0))
                .h(px(12.0))
                .bg(c),
        )
}

fn empty_stage_well(store: Store) -> View {
    view()
        .size_full()
        .relative()
        .items_center()
        .justify_center()
        .cursor_pointer()
        .on_press(move || pick_image_files(store))
        .child(registration_marks())
        // Centered icon + title
        .child(
            view()
                .flex_col()
                .items_center()
                .gap(px(14.0))
                .child(
                    view()
                        .size(px(40.0))
                        .rounded(px(10.0))
                        .bg(palette::container_low())
                        .border_1()
                        .border_color(palette::outline())
                        .items_center()
                        .justify_center()
                        .child(svg_icon(
                            crate::ui::icons::WORKBENCH,
                            18.0,
                            palette::muted(),
                        )),
                )
                .child(
                    text("拖入图片，或按 Ctrl+V 粘贴")
                        .text_size(px(13.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(palette::muted()),
                ),
        )
        // Bottom batch folder pill button
        .child(
            view()
                .absolute()
                .bottom(px(20.0))
                .left(px(0.0))
                .right(px(0.0))
                .items_center()
                .justify_center()
                .child(
                    view()
                        .h(px(28.0))
                        .px(px(14.0))
                        .rounded(px(7.0))
                        .bg(palette::container_low())
                        .border_1()
                        .border_color(palette::outline())
                        .hover(|s| s.bg(palette::high()))
                        .flex_row()
                        .items_center()
                        .gap(px(6.0))
                        .cursor_pointer()
                        .on_press(move || pick_batch_folder(store))
                        .child(svg_icon(crate::ui::icons::FOLDER, 12.0, palette::dim()))
                        .child(
                            text("选择文件夹批量处理")
                                .text_size(px(11.0))
                                .text_color(palette::dim()),
                        ),
                ),
        )
}

fn active_stage_well(store: Store) -> View {
    view()
        .size_full()
        .flex_col()
        .child(
            view()
                .flex_1()
                .w_full()
                .relative()
                .overflow_hidden()
                .child(dynamic(move || {
                    let inputs = store.inputs.get();
                    let idx = store.active_input.get().min(inputs.len().saturating_sub(1));
                    let Some(active) = inputs.get(idx).cloned() else {
                        return view();
                    };
                    if let ItemStatus::Done {
                        ref output_path, ..
                    } = active.status
                    {
                        compare_viewport(store, active.clone(), output_path.clone())
                    } else {
                        preview_viewport(store, active)
                    }
                })),
        )
        .child(telemetry_bar(store))
}

fn compare_viewport(store: Store, item: InputItem, output_path: PathBuf) -> View {
    let stage_w = signal(800.0f32);
    let stage_h = signal(540.0f32);
    let grab_x = signal(400.0f32);
    let split = store.split_pos;

    let before_src = item.path.to_string_lossy().into_owned();
    let after_src = output_path.to_string_lossy().into_owned();
    let scale = scale_from_filename(&store.selected_model.get()).unwrap_or(2);

    view()
        .size_full()
        .relative()
        .overflow_hidden()
        .cursor_pointer()
        .on_layout(move |b: &Bounds<Points>| {
            stage_w.set(b.size.width.0.max(1.0));
            stage_h.set(b.size.height.0.max(1.0));
        })
        .claim_touches(TouchClaim::on_start())
        .on_responder_grant(move |e: &ResponderEvent| {
            let x = e.location.x.0;
            grab_x.set(x);
            let w = stage_w.get().max(1.0);
            split.set((x / w).clamp(0.02, 0.98));
        })
        .on_responder_move(move |e: &ResponderEvent| {
            let x = grab_x.get() + e.gesture.delta.x.0;
            let w = stage_w.get().max(1.0);
            split.set((x / w).clamp(0.02, 0.98));
        })
        // Layer 1 (Full base): After (Upscaled) image
        .child(
            view()
                .absolute()
                .inset(px(0.0))
                .p(px(20.0))
                .items_center()
                .justify_center()
                .child(
                    image(after_src)
                        .size_full()
                        .object_fit(ResizeMode::Contain),
                ),
        )
        // Layer 2 (Left clipped): Before (Original) image
        .child(
            view()
                .absolute()
                .left(px(0.0))
                .top(px(0.0))
                .bottom(px(0.0))
                .w(move || px(stage_w.get() * split.get()))
                .overflow_hidden()
                .child(
                    view()
                        .w(move || px(stage_w.get()))
                        .h(move || px(stage_h.get()))
                        .p(px(20.0))
                        .items_center()
                        .justify_center()
                        .bg(palette::surface())
                        .child(
                            image(before_src)
                                .size_full()
                                .object_fit(ResizeMode::Contain),
                        ),
                ),
        )
        // Split divider vertical line (1.5px)
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .bottom(px(0.0))
                .w(px(1.5))
                .left(move || px((stage_w.get() * split.get()).round()))
                .bg(palette::on_surface())
                .pointer_events(PointerEvents::None),
        )
        // 28x28 circular drag handle with `‹ ›`
        .child(
            view()
                .absolute()
                .size(px(28.0))
                .rounded_full()
                .bg(palette::bg())
                .border_1()
                .border_color(palette::on_surface())
                .items_center()
                .justify_center()
                .left(move || px((stage_w.get() * split.get() - 14.0).round()))
                .top(move || px((stage_h.get() * 0.5 - 14.0).round()))
                .pointer_events(PointerEvents::None)
                .child(
                    text("‹ ›")
                        .text_size(px(10.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(palette::on_surface()),
                ),
        )
        // Top-left HUD chip: `原图 · 1×`
        .child(
            view()
                .absolute()
                .top(px(14.0))
                .left(px(14.0))
                .h(px(24.0))
                .px(px(10.0))
                .rounded(px(6.0))
                .bg(palette::bg())
                .border_1()
                .border_color(palette::outline())
                .items_center()
                .justify_center()
                .pointer_events(PointerEvents::None)
                .child(
                    text("原图 · 1×")
                        .text_size(px(11.0))
                        .text_color(palette::muted()),
                ),
        )
        // Top-right HUD chip: `超分 · N×`
        .child(
            view()
                .absolute()
                .top(px(14.0))
                .right(px(46.0))
                .h(px(24.0))
                .px(px(10.0))
                .rounded(px(6.0))
                .bg(palette::bg())
                .border_1()
                .border_color(palette::primary_dim_border())
                .items_center()
                .justify_center()
                .pointer_events(PointerEvents::None)
                .child(
                    text(format!("超分 · {scale}×"))
                        .text_size(px(11.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(palette::primary()),
                ),
        )
        // Top-right 26x26 close `x` button
        .child(
            view()
                .absolute()
                .top(px(13.0))
                .right(px(14.0))
                .size(px(26.0))
                .rounded(px(6.0))
                .bg(palette::bg())
                .border_1()
                .border_color(palette::outline())
                .hover(|s| s.bg(palette::high()))
                .items_center()
                .justify_center()
                .cursor_pointer()
                .on_press(move || store.clear_inputs())
                .child(svg_icon(crate::ui::icons::CLOSE, 12.0, palette::muted())),
        )
}

fn preview_viewport(store: Store, item: InputItem) -> View {
    let src = item.path.to_string_lossy().into_owned();
    let phase = store.job_phase.get();
    let is_running = matches!(phase, JobPhase::Running { .. });
    let frac = match phase {
        JobPhase::Running { fraction, .. } => fraction.clamp(0.05, 0.95),
        _ => 0.0,
    };

    view()
        .size_full()
        .relative()
        .items_center()
        .justify_center()
        .p(px(20.0))
        .child(image(src).size_full().object_fit(ResizeMode::Contain))
        // Horizontal laser scanline during Running
        .child(when(
            move || is_running,
            move || {
                view()
                    .absolute()
                    .left(px(20.0))
                    .right(px(20.0))
                    .top(percentage(frac))
                    .h(px(2.0))
                    .bg(palette::primary())
                    .pointer_events(PointerEvents::None)
            },
        ))
        // Top-right 26x26 close `x` button (when not running)
        .child(when(
            move || !is_running,
            move || {
                view()
                    .absolute()
                    .top(px(13.0))
                    .right(px(14.0))
                    .size(px(26.0))
                    .rounded(px(6.0))
                    .bg(palette::bg())
                    .border_1()
                    .border_color(palette::outline())
                    .hover(|s| s.bg(palette::high()))
                    .items_center()
                    .justify_center()
                    .cursor_pointer()
                    .on_press(move || store.clear_inputs())
                    .child(svg_icon(crate::ui::icons::CLOSE, 12.0, palette::muted()))
            },
        ))
}

fn telemetry_bar(store: Store) -> View {
    view()
        .w_full()
        .h(px(40.0))
        .bg(palette::recessed())
        .border_t_1()
        .border_color(palette::outline())
        .px(px(14.0))
        .flex_row()
        .items_center()
        .justify_between()
        .child(dynamic(move || {
            let phase = store.job_phase.get();
            let inputs = store.inputs.get();
            let idx = store.active_input.get().min(inputs.len().saturating_sub(1));
            let active = inputs.get(idx).cloned();

            match phase {
                JobPhase::Running {
                    fraction, stage, ..
                } => {
                    let pct_v = (fraction * 100.0).round() as i32;
                    let fname = active
                        .as_ref()
                        .map(|a| a.filename.clone())
                        .unwrap_or_default();
                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(10.0))
                                .child(
                                    view()
                                        .size(px(7.0))
                                        .rounded_full()
                                        .bg(palette::primary()),
                                )
                                .child(
                                    text("正在超分")
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .text_color(palette::on_surface()),
                                )
                                .child(text("·").text_size(px(11.0)).text_color(palette::dim()))
                                .child(
                                    text(fname)
                                        .text_size(px(11.0))
                                        .text_color(palette::muted()),
                                )
                                .child(text("·").text_size(px(11.0)).text_color(palette::dim()))
                                .child(
                                    text(stage)
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::primary()),
                                ),
                        )
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(10.0))
                                .child(
                                    view()
                                        .w(px(140.0))
                                        .h(px(4.0))
                                        .rounded(px(2.0))
                                        .bg(palette::outline())
                                        .overflow_hidden()
                                        .child(
                                            view()
                                                .h_full()
                                                .w(percentage(fraction.clamp(0.0, 1.0)))
                                                .bg(palette::primary()),
                                        ),
                                )
                                .child(
                                    text(format!("{pct_v}%"))
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::primary()),
                                ),
                        )
                }
                JobPhase::Done { elapsed_ms, .. } => {
                    let elapsed_s = elapsed_ms as f64 / 1000.0;
                    let (dim_str, out_p) = if let Some(ref a) = active {
                        if let ItemStatus::Done {
                            ref output_path,
                            out_w,
                            out_h,
                        } = a.status
                        {
                            (
                                format!("{} × {}  →  {out_w} × {out_h}", a.width, a.height),
                                Some(output_path.clone()),
                            )
                        } else {
                            (format!("{} × {}", a.width, a.height), None)
                        }
                    } else {
                        (String::new(), None)
                    };

                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(10.0))
                                .child(
                                    view()
                                        .size(px(7.0))
                                        .rounded_full()
                                        .bg(palette::primary()),
                                )
                                .child(
                                    text("已完成")
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .text_color(palette::primary()),
                                )
                                .child(text("·").text_size(px(11.0)).text_color(palette::dim()))
                                .child(
                                    text(dim_str)
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::on_surface()),
                                )
                                .child(text("·").text_size(px(11.0)).text_color(palette::dim()))
                                .child(
                                    text(format!("{elapsed_s:.2}s"))
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::muted()),
                                ),
                        )
                        .child(if let Some(p) = out_p {
                            view()
                                .h(px(26.0))
                                .px(px(10.0))
                                .rounded(px(6.0))
                                .bg(palette::container_low())
                                .border_1()
                                .border_color(palette::outline())
                                .hover(|s| s.bg(palette::high()))
                                .flex_row()
                                .items_center()
                                .gap(px(6.0))
                                .cursor_pointer()
                                .on_press(move || {
                                    let p_reveal = p.clone();
                                    spawn(async move {
                                        let _ = reveal_path(p_reveal).await;
                                    });
                                })
                                .child(svg_icon(
                                    crate::ui::icons::FOLDER,
                                    12.0,
                                    palette::on_surface(),
                                ))
                                .child(
                                    text("在文件夹中显示")
                                        .text_size(px(11.0))
                                        .text_color(palette::on_surface()),
                                )
                        } else {
                            view()
                        })
                }
                JobPhase::Failed(err_msg) => {
                    let msg_copy = err_msg.clone();
                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(8.0))
                                .child(
                                    view()
                                        .size(px(7.0))
                                        .rounded_full()
                                        .bg(palette::error()),
                                )
                                .child(
                                    text("处理中断")
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .text_color(palette::error()),
                                )
                                .child(text("·").text_size(px(11.0)).text_color(palette::dim()))
                                .child(
                                    text(err_msg)
                                        .text_size(px(11.0))
                                        .text_color(palette::muted()),
                                ),
                        )
                        .child(
                            view()
                                .h(px(26.0))
                                .px(px(10.0))
                                .rounded(px(6.0))
                                .bg(palette::container_low())
                                .border_1()
                                .border_color(palette::outline())
                                .hover(|s| s.bg(palette::high()))
                                .flex_row()
                                .items_center()
                                .gap(px(6.0))
                                .cursor_pointer()
                                .on_press(move || set_clipboard(msg_copy.clone()))
                                .child(
                                    text("复制诊断")
                                        .text_size(px(11.0))
                                        .text_color(palette::muted()),
                                ),
                        )
                }
                JobPhase::Idle | JobPhase::Cancelled => {
                    let scale =
                        scale_from_filename(&store.selected_model.get()).unwrap_or(2) as u32;
                    let (fname, dim_info) = if let Some(ref a) = active {
                        let d = if a.width > 0 && a.height > 0 {
                            format!(
                                "{} × {}  →  {} × {}",
                                a.width,
                                a.height,
                                a.width * scale,
                                a.height * scale
                            )
                        } else {
                            format!("预计放大 {scale}×")
                        };
                        (a.filename.clone(), d)
                    } else {
                        (String::new(), format!("预计放大 {scale}×"))
                    };

                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(8.0))
                                .child(
                                    text(fname)
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_surface()),
                                )
                                .child(text("·").text_size(px(11.0)).text_color(palette::dim()))
                                .child(
                                    text(dim_info)
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::muted()),
                                ),
                        )
                        .child(
                            text(if matches!(phase, JobPhase::Cancelled) {
                                "已取消"
                            } else {
                                "就绪"
                            })
                            .text_size(px(11.0))
                            .text_color(palette::dim()),
                        )
                }
            }
        }))
}

fn filmstrip_dock(store: Store, items: Vec<InputItem>) -> View {
    let total = items.len();
    let done = items
        .iter()
        .filter(|it| matches!(it.status, ItemStatus::Done { .. } | ItemStatus::Skipped))
        .count();
    let active_idx = store.active_input.get();

    let mut row = view().flex_row().items_center().gap(px(8.0));

    for (idx, item) in items.into_iter().enumerate().take(12) {
        let is_active = idx == active_idx;
        let border_col = if is_active {
            palette::primary()
        } else {
            match item.status {
                ItemStatus::Done { .. } | ItemStatus::Running(_) => palette::primary_dim_border(),
                ItemStatus::Failed(_) => palette::error(),
                _ => palette::outline(),
            }
        };
        let src = item.path.to_string_lossy().into_owned();

        let thumb = view()
            .size(px(40.0))
            .rounded(px(6.0))
            .bg(palette::container_low())
            .border_1()
            .border_color(border_col)
            .overflow_hidden()
            .relative()
            .items_center()
            .justify_center()
            .cursor_pointer()
            .on_press(move || store.active_input.set(idx))
            .child(image(src).size_full().object_fit(ResizeMode::Cover));

        row = row.child(thumb);
    }

    row = row.child(
        view()
            .size(px(40.0))
            .rounded(px(6.0))
            .bg(palette::container_low())
            .border_1()
            .border_color(palette::outline())
            .hover(|s| s.bg(palette::high()))
            .items_center()
            .justify_center()
            .cursor_pointer()
            .on_press(move || pick_image_files(store))
            .child(svg_icon(crate::ui::icons::PLUS, 14.0, palette::muted())),
    );

    view()
        .w_full()
        .h(px(54.0))
        .rounded(px(10.0))
        .bg(palette::surface())
        .border_1()
        .border_color(palette::outline())
        .px(px(10.0))
        .flex_row()
        .items_center()
        .justify_between()
        .child(row)
        .child(
            text(format!("{done} / {total}"))
                .text_size(px(11.0))
                .font_family(FONT_MONO)
                .tabular_nums()
                .text_color(palette::muted()),
        )
}

fn inspector_column(store: Store) -> View {
    view()
        .w(px(308.0))
        .h_full()
        .bg(palette::bg())
        .border_l_1()
        .border_color(palette::outline())
        .relative()
        .flex_col()
        .justify_between()
        // Scrollable controls column
        .child(
            scroll()
                .flex_1()
                .w_full()
                .p(px(16.0))
                .flex_col()
                .gap(px(14.0))
                // ── Section 1: 模型 ──
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(8.0))
                        .child(section_header("模型"))
                        .child(model_picker_trigger(store)),
                )
                .child(hairline())
                // ── Section 2: 输出 ──
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(10.0))
                        .child(section_header("输出"))
                        .child(
                            view()
                                .w_full()
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    text("格式")
                                        .text_size(px(12.0))
                                        .text_color(palette::muted()),
                                ),
                        )
                        .child(dynamic(move || {
                            segmented(
                                &[
                                    SegmentItem {
                                        value: OutputFormat::Png,
                                        label: "PNG",
                                    },
                                    SegmentItem {
                                        value: OutputFormat::Jpg,
                                        label: "JPG",
                                    },
                                    SegmentItem {
                                        value: OutputFormat::Webp,
                                        label: "WebP",
                                    },
                                    SegmentItem {
                                        value: OutputFormat::Avif,
                                        label: "AVIF",
                                    },
                                ],
                                store.format.get(),
                                move |v| store.format.set(v),
                            )
                        }))
                        // Quality slider (when not PNG)
                        .child(when(
                            move || store.format.get().supports_quality(),
                            move || {
                                view()
                                    .w_full()
                                    .flex_col()
                                    .gap(px(6.0))
                                    .child(
                                        view()
                                            .w_full()
                                            .flex_row()
                                            .items_center()
                                            .justify_between()
                                            .child(
                                                text("编码质量")
                                                    .text_size(px(12.0))
                                                    .text_color(palette::muted()),
                                            )
                                            .child(
                                                text(move || format!("{}%", store.quality.get()))
                                                    .text_size(px(12.0))
                                                    .font_weight(FontWeight::SEMIBOLD)
                                                    .font_family(FONT_MONO)
                                                    .tabular_nums()
                                                    .text_color(palette::primary()),
                                            ),
                                    )
                                    .child(
                                        nsh_base::slider()
                                            .min(1.0)
                                            .max(100.0)
                                            .step(1.0)
                                            .value(move || {
                                                Some(nsh_base::SliderValue::Single(
                                                    store.quality.get() as f64,
                                                ))
                                            })
                                            .on_change(move |v: &nsh_base::SliderValue| {
                                                store.quality.set(v.start().round() as i32)
                                            })
                                            .track_style(|s| s.h(px(6.0)).bg(palette::recessed()))
                                            .indicator_style(|s| s.bg(palette::primary()))
                                            .thumb_style(|s| {
                                                s.size(px(14.0))
                                                    .bg(palette::primary())
                                                    .border_1()
                                                    .border_color(palette::surface())
                                            }),
                                    )
                            },
                        ))
                        // Chroma subsampling (when AVIF or JPG)
                        .child(when(
                            move || store.format.get().supports_chroma(),
                            move || {
                                view()
                                    .w_full()
                                    .flex_col()
                                    .gap(px(6.0))
                                    .child(
                                        text("色度抽样")
                                            .text_size(px(12.0))
                                            .text_color(palette::muted()),
                                    )
                                    .child(dynamic(move || {
                                        segmented(
                                            &[
                                                SegmentItem {
                                                    value: ChromaSubsampling::Yuv420,
                                                    label: "4:2:0",
                                                },
                                                SegmentItem {
                                                    value: ChromaSubsampling::Yuv422,
                                                    label: "4:2:2",
                                                },
                                                SegmentItem {
                                                    value: ChromaSubsampling::Yuv444,
                                                    label: "4:4:4",
                                                },
                                            ],
                                            store.chroma.get(),
                                            move |v| store.chroma.set(v),
                                        )
                                    }))
                            },
                        ))
                        // AVIF speed (when AVIF)
                        .child(when(
                            move || store.format.get() == OutputFormat::Avif,
                            move || {
                                view()
                                    .w_full()
                                    .flex_col()
                                    .gap(px(6.0))
                                    .child(
                                        text("编码速度")
                                            .text_size(px(12.0))
                                            .text_color(palette::muted()),
                                    )
                                    .child(dynamic(move || {
                                        segmented(
                                            &[
                                                SegmentItem {
                                                    value: 4,
                                                    label: "精细",
                                                },
                                                SegmentItem {
                                                    value: 6,
                                                    label: "均衡",
                                                },
                                                SegmentItem {
                                                    value: 8,
                                                    label: "快速",
                                                },
                                            ],
                                            store.avif_speed.get(),
                                            move |v| store.avif_speed.set(v),
                                        )
                                    }))
                            },
                        ))
                        // Export location
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(6.0))
                                .child(
                                    text("导出位置")
                                        .text_size(px(12.0))
                                        .text_color(palette::muted()),
                                )
                                .child(output_dir_box(store)),
                        ),
                )
                .child(hairline())
                // ── Section 3: 引擎与处理 ──
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(10.0))
                        .child(section_header("引擎与处理"))
                        // Compute device
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(6.0))
                                .child(
                                    text("计算设备")
                                        .text_size(px(12.0))
                                        .text_color(palette::muted()),
                                )
                                .child(dynamic(move || {
                                    segmented(
                                        &[
                                            SegmentItem {
                                                value: true,
                                                label: "GPU (Vulkan)",
                                            },
                                            SegmentItem {
                                                value: false,
                                                label: "CPU",
                                            },
                                        ],
                                        store.use_gpu.get(),
                                        move |v| store.use_gpu.set(v),
                                    )
                                })),
                        )
                        // Tile size
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(6.0))
                                .child(
                                    view()
                                        .w_full()
                                        .flex_row()
                                        .items_center()
                                        .justify_between()
                                        .child(
                                            text("分块尺寸")
                                                .text_size(px(12.0))
                                                .text_color(palette::muted()),
                                        )
                                        .child(
                                            text(move || {
                                                let t = store.tile_size.get();
                                                if t == 0 {
                                                    "自动".to_string()
                                                } else {
                                                    format!("{t} px")
                                                }
                                            })
                                            .text_size(px(11.0))
                                            .font_family(FONT_MONO)
                                            .text_color(palette::dim()),
                                        ),
                                )
                                .child(dynamic(move || {
                                    segmented(
                                        &[
                                            SegmentItem {
                                                value: 0,
                                                label: "自动",
                                            },
                                            SegmentItem {
                                                value: 512,
                                                label: "512",
                                            },
                                            SegmentItem {
                                                value: 256,
                                                label: "256",
                                            },
                                            SegmentItem {
                                                value: 128,
                                                label: "128",
                                            },
                                        ],
                                        store.tile_size.get(),
                                        move |v| store.tile_size.set(v),
                                    )
                                })),
                        )
                        // Warm daemon toggle
                        .child(dynamic(move || {
                            toggle_row(
                                "常驻热引擎",
                                "后台保活模型，消除冷启动",
                                store.daemon_running.get(),
                                move |v| store.toggle_daemon(v),
                            )
                        }))
                        // Force overwrite toggle
                        .child(dynamic(move || {
                            toggle_row(
                                "覆盖已有结果",
                                "忽略已超分标记，强制重跑",
                                store.force.get(),
                                move |v| store.force.set(v),
                            )
                        })),
                ),
        )
        // Bottom sticky CTA bar
        .child(
            view()
                .w_full()
                .p(px(16.0))
                .pt(px(10.0))
                .border_t_1()
                .border_color(palette::outline())
                .child(inspector_cta_button(store)),
        )
        // Floating model picker popup
        .child(when(
            move || store.show_model_picker.get(),
            move || model_picker_popup(store),
        ))
}

fn model_picker_trigger(store: Store) -> AnyElement {
    dynamic(move || {
        let is_open = store.show_model_picker.get();
        let model_name = store.selected_model.get();
        let scale = scale_from_filename(&model_name).unwrap_or(2);

        view()
            .w_full()
            .h(px(36.0))
            .rounded(px(8.0))
            .px(px(12.0))
            .flex_row()
            .items_center()
            .justify_between()
            .cursor_pointer()
            .bg(palette::container_low())
            .border_1()
            .border_color(if is_open {
                palette::primary_dim_border()
            } else {
                palette::outline_input()
            })
            .hover(|s| s.bg(palette::high()))
            .on_press(move || store.show_model_picker.update(|v| *v = !*v))
            .child(
                text(model_name)
                    .text_size(px(12.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(palette::on_surface()),
            )
            .child(
                view()
                    .flex_row()
                    .items_center()
                    .gap(px(6.0))
                    .child(
                        view()
                            .h(px(18.0))
                            .px(px(6.0))
                            .rounded(px(4.0))
                            .bg(palette::primary_dim_bg())
                            .border_1()
                            .border_color(palette::primary_dim_border())
                            .items_center()
                            .justify_center()
                            .child(
                                text(format!("{scale}×"))
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .font_family(FONT_MONO)
                                    .tabular_nums()
                                    .text_color(palette::primary()),
                            ),
                    )
                    .child(
                        text("▾")
                            .text_size(px(11.0))
                            .text_color(palette::muted()),
                    ),
            )
    })
}

fn model_picker_popup(store: Store) -> View {
    view()
        .absolute()
        .top(px(82.0))
        .left(px(16.0))
        .right(px(16.0))
        .h(px(340.0))
        .rounded(px(10.0))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline_strong())
        .shadow_lg()
        .p(px(8.0))
        .flex_col()
        .gap(px(6.0))
        // Search box
        .child(
            view()
                .w_full()
                .h(px(30.0))
                .rounded(px(6.0))
                .bg(palette::recessed())
                .border_1()
                .border_color(palette::outline())
                .px(px(8.0))
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .child(
                    input()
                        .flex_1()
                        .placeholder("搜索已安装模型...")
                        .value(move || store.picker_search.get())
                        .on_change(move |v: &str| store.picker_search.set(v.to_owned()))
                        .text_size(px(11.0))
                        .text_color(palette::on_surface())
                        .bg(AdaptiveColor::TRANSPARENT)
                        .border(px(0.0)),
                ),
        )
        // Scrollable model list
        .child(
            scroll()
                .flex_1()
                .w_full()
                .child(dynamic(move || {
                    let q = store.picker_search.get().to_ascii_lowercase();
                    let current = store.selected_model.get();
                    let groups = store.installed_groups.get();
                    let mut col = view().w_full().flex_col().gap(px(2.0));

                    for g in groups {
                        for m in g.items {
                            if !q.is_empty()
                                && !m.name.to_ascii_lowercase().contains(&q)
                                && !m.scene.to_ascii_lowercase().contains(&q)
                                && !g.title.to_ascii_lowercase().contains(&q)
                            {
                                continue;
                            }
                            let is_sel = m.name == current;
                            let name_sel = m.name.clone();
                            let short = short_name(&m.name);

                            col = col.child(
                                view()
                                    .w_full()
                                    .h(px(30.0))
                                    .rounded(px(6.0))
                                    .px(px(8.0))
                                    .flex_row()
                                    .items_center()
                                    .justify_between()
                                    .cursor_pointer()
                                    .bg(if is_sel {
                                        palette::seg_selected()
                                    } else {
                                        AdaptiveColor::TRANSPARENT
                                    })
                                    .hover(|s| s.bg(palette::high()))
                                    .on_press(move || {
                                        store.selected_model.set(name_sel.clone());
                                        store.show_model_picker.set(false);
                                    })
                                    .child(
                                        view()
                                            .flex_row()
                                            .items_center()
                                            .gap(px(6.0))
                                            .child(
                                                text(format!("{} · {short}", g.title))
                                                    .text_size(px(11.0))
                                                    .font_weight(if is_sel {
                                                        FontWeight::SEMIBOLD
                                                    } else {
                                                        FontWeight::MEDIUM
                                                    })
                                                    .text_color(if is_sel {
                                                        palette::primary()
                                                    } else {
                                                        palette::on_surface()
                                                    }),
                                            )
                                            .child(if m.recommended {
                                                text("★")
                                                    .text_size(px(10.0))
                                                    .text_color(palette::star_gold())
                                                    .into_element()
                                            } else {
                                                view().into_element()
                                            }),
                                    )
                                    .child(
                                        view()
                                            .flex_row()
                                            .items_center()
                                            .gap(px(6.0))
                                            .child(
                                                text(format!("{}×", m.scale))
                                                    .text_size(px(10.0))
                                                    .font_family(FONT_MONO)
                                                    .tabular_nums()
                                                    .text_color(palette::muted()),
                                            )
                                            .child(
                                                text(human_bytes(m.size_bytes))
                                                    .text_size(px(10.0))
                                                    .font_family(FONT_MONO)
                                                    .tabular_nums()
                                                    .text_color(palette::dim()),
                                            ),
                                    ),
                            );
                        }
                    }
                    col
                })),
        )
        // Bottom link to Models tab
        .child(
            view()
                .w_full()
                .h(px(28.0))
                .rounded(px(6.0))
                .border_t_1()
                .border_color(palette::outline())
                .items_center()
                .justify_center()
                .cursor_pointer()
                .hover(|s| s.bg(palette::high()))
                .on_press(move || {
                    store.show_model_picker.set(false);
                    store.nav.set(NavTab::Models);
                })
                .child(
                    text("管理与下载更多模型 →")
                        .text_size(px(11.0))
                        .text_color(palette::primary()),
                ),
        )
}

fn output_dir_box(store: Store) -> AnyElement {
    dynamic(move || {
        let custom = store.custom_out_dir.get();
        let label = match custom {
            Some(ref p) => p
                .file_name()
                .and_then(|s| s.to_str())
                .map(str::to_owned)
                .unwrap_or_else(|| p.display().to_string()),
            None => "跟随原图目录".to_string(),
        };
        let has_custom = custom.is_some();

        view()
            .w_full()
            .h(px(32.0))
            .rounded(px(7.0))
            .bg(palette::container_low())
            .border_1()
            .border_color(palette::outline_input())
            .px(px(10.0))
            .flex_row()
            .items_center()
            .justify_between()
            .child(
                view()
                    .flex_row()
                    .items_center()
                    .gap(px(6.0))
                    .overflow_hidden()
                    .child(svg_icon(crate::ui::icons::FOLDER, 13.0, palette::muted()))
                    .child(
                        text(label)
                            .text_size(px(11.0))
                            .text_color(palette::on_surface()),
                    ),
            )
            .child(
                view()
                    .flex_row()
                    .items_center()
                    .gap(px(6.0))
                    .child(if has_custom {
                        view()
                            .size(px(18.0))
                            .rounded(px(4.0))
                            .items_center()
                            .justify_center()
                            .cursor_pointer()
                            .hover(|s| s.bg(palette::high()))
                            .on_press(move || store.custom_out_dir.set(None))
                            .child(svg_icon(crate::ui::icons::CLOSE, 10.0, palette::dim()))
                    } else {
                        view()
                    })
                    .child(
                        view()
                            .px(px(6.0))
                            .py(px(2.0))
                            .rounded(px(4.0))
                            .cursor_pointer()
                            .hover(|s| s.bg(palette::high()))
                            .on_press(move || pick_export_folder(store))
                            .child(
                                text(if has_custom { "更改" } else { "指定…" })
                                    .text_size(px(11.0))
                                    .text_color(palette::primary()),
                            ),
                    ),
            )
    })
}

fn inspector_cta_button(store: Store) -> AnyElement {
    dynamic(move || {
        let phase = store.job_phase.get();
        let has_inputs = !store.inputs.get().is_empty();

        match phase {
            JobPhase::Running { .. } => view()
                .w_full()
                .h(px(38.0))
                .rounded(px(6.0))
                .bg(palette::container_low())
                .border_1()
                .border_color(palette::outline_input())
                .hover(|s| s.bg(palette::high()))
                .items_center()
                .justify_center()
                .cursor_pointer()
                .on_press(move || store.cancel_job())
                .child(
                    text("取消超分  Ctrl+.")
                        .text_size(px(13.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(palette::on_surface()),
                ),
            _ => {
                if has_inputs {
                    primary_button("开始超分", true, move || store.start_job())
                } else {
                    primary_button("选择图片开始", true, move || pick_image_files(store))
                }
            }
        }
    })
}
