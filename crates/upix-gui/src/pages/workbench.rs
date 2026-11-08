//! Workbench page (`desktop/src/main.cpp` `renderWorkbench`, Figma Frames `4:2`, `71:1129`, `73:1931`):
//! Left Stage Well (corner registration marks, before/after split curtain, 40px telemetry bar,
//! 54px filmstrip dock) + Right 308px Inspector (floating model picker, 5-way format segmented,
//! quality slider with tick marks, inline chroma & speed rows, output directory, GPU/CPU, tile size,
//! warm daemon, force overwrite, sticky 276x44 CTA).

use std::path::PathBuf;

use nsh_core::element::input;
use nsh_core::file_dialog::{FileFilter, OpenDialog};
use nsh_core::prelude::*;

use crate::engine_bridge::{ChromaSubsampling, OutputFormat};
use crate::models_local::{human_bytes, scale_from_filename};
use crate::store::{file_uri_path, InputItem, ItemStatus, JobPhase, Store};
use crate::theme::{palette, FONT_MONO, FONT_SANS};
use crate::ui::{
    primary_button, quality_slider, section_header, segmented, segmented_sized,
    segmented_with_font, svg_icon, toggle_row, toggle_switch, SegmentItem,
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
        .px(px(8.0))
        .py(px(16.0))
        .gap(px(10.0))
        .flex_col()
        .on_file_drop(move |e: &FileDropEvent| {
            let paths: Vec<PathBuf> = e.uris.iter().filter_map(|u| file_uri_path(u)).collect();
            store.add_paths(&paths);
        })
        // Main Stage Well (Figma 4:18 & 71:1133: #18181c, border 1px solid #282830, radius 12px)
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
        .inset(px(28.0))
        .pointer_events(PointerEvents::None)
        // Top-left
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(16.0))
                .h(px(2.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(2.0))
                .h(px(16.0))
                .bg(c),
        )
        // Top-right
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(16.0))
                .h(px(2.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(2.0))
                .h(px(16.0))
                .bg(c),
        )
        // Bottom-left
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(16.0))
                .h(px(2.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(2.0))
                .h(px(16.0))
                .bg(c),
        )
        // Bottom-right
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(16.0))
                .h(px(2.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(2.0))
                .h(px(16.0))
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
        // Centered 32x32 zoom-in icon + 15px title (Figma 7:6)
        .child(
            view()
                .flex_col()
                .items_center()
                .gap(px(14.0))
                .child(svg_icon(
                    crate::ui::icons::ZOOM_IN,
                    32.0,
                    palette::muted(),
                ))
                .child(
                    text("拖入图片，或按 Ctrl+V 粘贴")
                        .text_size(px(15.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(palette::muted()),
                ),
        )
        // Bottom 162x32 batch folder button at 44px from bottom (Figma 12:366)
        .child(
            view()
                .absolute()
                .bottom(px(44.0))
                .left(px(0.0))
                .right(px(0.0))
                .items_center()
                .justify_center()
                .child(
                    view()
                        .w(px(162.0))
                        .h(px(32.0))
                        .rounded(px(8.0))
                        .bg(palette::container_low())
                        .border_1()
                        .border_color(palette::outline_input())
                        .hover(|s| s.bg(palette::high()))
                        .flex_row()
                        .items_center()
                        .justify_center()
                        .gap(px(8.0))
                        .cursor_pointer()
                        .on_press(move || pick_batch_folder(store))
                        .child(svg_icon(crate::ui::icons::FOLDER, 14.0, palette::muted()))
                        .child(
                            text("选择文件夹批量处理")
                                .text_size(px(12.0))
                                .font_weight(FontWeight::MEDIUM)
                                .text_color(palette::muted()),
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
    let stage_bounds = signal(Bounds::<Points>::default());
    let grab_x = stored(None::<f32>);
    let split = store.split_pos;

    let before_src = item.path.to_string_lossy().into_owned();
    let after_src = output_path.to_string_lossy().into_owned();
    let scale = scale_from_filename(&store.selected_model.get()).unwrap_or(4);

    view()
        .size_full()
        .relative()
        .overflow_hidden()
        .cursor_pointer()
        .on_layout(move |b: &Bounds<Points>| {
            stage_bounds.set(*b);
        })
        .claim_touches(TouchClaim::on_start())
        .keep_touches(true)
        .on_responder_grant(move |e: &ResponderEvent| {
            let b = stage_bounds.get_untracked();
            let local_x = e.location.x.0 - b.origin.x.0;
            let _ = grab_x.replace(Some(local_x));
            let w = b.size.width.0.max(1.0);
            split.set((local_x / w).clamp(0.02, 0.98));
        })
        .on_responder_move(move |e: &ResponderEvent| {
            if let Some(start_x) = grab_x.get() {
                let x = start_x + e.gesture.delta.x.0;
                let w = stage_bounds.get_untracked().size.width.0.max(1.0);
                split.set((x / w).clamp(0.02, 0.98));
            }
        })
        .on_responder_release(move |_: &ResponderEvent| {
            let _ = grab_x.replace(None);
        })
        .on_responder_terminate(move |_: &ResponderEvent| {
            let _ = grab_x.replace(None);
        })
        // Layer 1 (Full base): After (Upscaled) image
        .child(
            view()
                .absolute()
                .inset(px(0.0))
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
                .w(move || px(stage_bounds.get().size.width.0.max(1.0) * split.get()))
                .overflow_hidden()
                .child(
                    view()
                        .w(move || px(stage_bounds.get().size.width.0.max(1.0)))
                        .h(move || px(stage_bounds.get().size.height.0.max(1.0)))
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
        // Split divider vertical line (1px)
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .bottom(px(0.0))
                .w(px(1.0))
                .left(move || px((stage_bounds.get().size.width.0.max(1.0) * split.get()).round()))
                .bg(palette::on_surface())
                .pointer_events(PointerEvents::None),
        )
        // 28x28 rounded square drag handle (Figma Frame 71:1129: #141418d9, border #2c2c35, radius 7px)
        .child(
            view()
                .absolute()
                .size(px(28.0))
                .rounded(px(7.0))
                .bg(AdaptiveColor::rgb(0xF4F3EF, 0x141418))
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2C2C35))
                .items_center()
                .justify_center()
                .left(move || {
                    px((stage_bounds.get().size.width.0.max(1.0) * split.get() - 14.0).round())
                })
                .top(move || {
                    px((stage_bounds.get().size.height.0.max(1.0) * 0.5 - 14.0).round())
                })
                .pointer_events(PointerEvents::None)
                .child(
                    text("‹ ›")
                        .text_size(px(10.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(palette::muted()),
                ),
        )
        // Top-left HUD chip (Figma 75:2993: 77x26px, #141418c7, border #2c2c35, radius 6px, "原图 · 1×")
        .child(
            view()
                .absolute()
                .top(px(14.0))
                .left(px(14.0))
                .w(px(77.0))
                .h(px(26.0))
                .rounded(px(6.0))
                .bg(AdaptiveColor::rgb(0xF4F3EF, 0x141418))
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2C2C35))
                .items_center()
                .justify_center()
                .pointer_events(PointerEvents::None)
                .child(
                    text("原图 · 1×")
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .text_color(palette::muted()),
                ),
        )
        // Top-right HUD chip (Figma 75:2995: 77x26px, #141418c7, border #2c2c35, radius 6px, "超分 · 4×")
        .child(
            view()
                .absolute()
                .top(px(14.0))
                .right(px(50.0))
                .w(px(77.0))
                .h(px(26.0))
                .rounded(px(6.0))
                .bg(AdaptiveColor::rgb(0xF4F3EF, 0x141418))
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2C2C35))
                .items_center()
                .justify_center()
                .pointer_events(PointerEvents::None)
                .child(
                    text(format!("超分 · {scale}×"))
                        .text_size(px(11.0))
                        .font_weight(FontWeight::MEDIUM)
                        .font_family(FONT_MONO)
                        .text_color(palette::on_surface()),
                ),
        )
        // Top-right 26x26 close `x` button
        .child(
            view()
                .absolute()
                .top(px(14.0))
                .right(px(14.0))
                .size(px(26.0))
                .rounded(px(6.0))
                .bg(AdaptiveColor::rgb(0xF4F3EF, 0x141418))
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2C2C35))
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
        .child(image(src).size_full().object_fit(ResizeMode::Contain))
        // Horizontal laser scanline during Running
        .child(when(
            move || is_running,
            move || {
                view()
                    .absolute()
                    .left(px(0.0))
                    .right(px(0.0))
                    .top(percentage(frac))
                    .h(px(1.5))
                    .bg(palette::primary())
                    .pointer_events(PointerEvents::None)
            },
        ))
        // Top-right 28x28 close `x` button
        .child(
            view()
                .absolute()
                .top(px(14.0))
                .right(px(14.0))
                .size(px(28.0))
                .rounded(px(6.0))
                .bg(AdaptiveColor::rgb(0xF4F3EF, 0x141418))
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2C2C35))
                .hover(|s| s.bg(palette::high()))
                .items_center()
                .justify_center()
                .cursor_pointer()
                .on_press(move || store.clear_inputs())
                .child(svg_icon(crate::ui::icons::CLOSE, 13.0, palette::muted())),
        )
}

fn telemetry_bar(store: Store) -> View {
    view()
        .w_full()
        .h(px(40.0))
        .bg(palette::surface())
        .border_t_1()
        .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x26262E))
        .px(px(16.0))
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
                                        .size(px(6.0))
                                        .rounded_full()
                                        .bg(palette::primary()),
                                )
                                .child(
                                    text(fname)
                                        .text_size(px(12.0))
                                        .font_family(FONT_MONO)
                                        .text_color(palette::on_surface()),
                                )
                                .child(
                                    text(stage)
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::dim()),
                                ),
                        )
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(10.0))
                                .child(
                                    view()
                                        .w(px(148.0))
                                        .h(px(4.0))
                                        .rounded(px(2.0))
                                        .bg(AdaptiveColor::rgb(0xD6D5CD, 0x121216))
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
                                        .font_weight(FontWeight::MEDIUM)
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::primary()),
                                ),
                        )
                }
                JobPhase::Done { elapsed_ms, .. } => {
                    let elapsed_s = elapsed_ms as f64 / 1000.0;
                    let model_name = store.selected_model.get();
                    let scale = scale_from_filename(&model_name).unwrap_or(4);
                    let fmt_label = match store.format.get() {
                        OutputFormat::Follow => "PNG",
                        other => other.label(),
                    };
                    let badge_txt = format!("{scale}× · {fmt_label}");

                    let (dim_str, out_p, size_str) = if let Some(ref a) = active {
                        if let ItemStatus::Done {
                            ref output_path,
                            out_w,
                            out_h,
                        } = a.status
                        {
                            let sz = std::fs::metadata(output_path)
                                .map(|m| human_bytes(m.len()))
                                .unwrap_or_else(|_| "—".into());
                            (
                                format!("{}×{} → {out_w}×{out_h}", a.width, a.height),
                                Some(output_path.clone()),
                                sz,
                            )
                        } else {
                            (format!("{}×{}", a.width, a.height), None, String::new())
                        }
                    } else {
                        (String::new(), None, String::new())
                    };
                    let meta_str = if size_str.is_empty() {
                        format!("{elapsed_s:.2}s · {model_name}")
                    } else {
                        format!("{elapsed_s:.2}s · {size_str} · {model_name}")
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
                                        .size(px(6.0))
                                        .rounded_full()
                                        .bg(palette::primary()),
                                )
                                .child(
                                    view()
                                        .h(px(20.0))
                                        .px(px(6.0))
                                        .rounded(px(4.0))
                                        .bg(palette::primary_dim_bg())
                                        .border_1()
                                        .border_color(palette::primary_dim_border())
                                        .items_center()
                                        .justify_center()
                                        .child(
                                            text(badge_txt)
                                                .text_size(px(10.0))
                                                .font_family(FONT_MONO)
                                                .text_color(palette::primary()),
                                        ),
                                )
                                .child(
                                    text(dim_str)
                                        .text_size(px(12.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::on_surface()),
                                )
                                .child(
                                    text(meta_str)
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::dim()),
                                ),
                        )
                        .child(if let Some(p) = out_p {
                            let p_open = p.clone();
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(12.0))
                                .child(
                                    view()
                                        .cursor_pointer()
                                        .on_press(move || {
                                            let p_rev = p.clone();
                                            spawn(async move {
                                                let _ = reveal_path(p_rev).await;
                                            });
                                        })
                                        .child(
                                            text("定位文件")
                                                .text_size(px(11.0))
                                                .text_color(AdaptiveColor::rgb(0x706F68, 0x8E8E9A)),
                                        ),
                                )
                                .child(
                                    view()
                                        .w(px(64.0))
                                        .h(px(24.0))
                                        .rounded(px(4.0))
                                        .bg(AdaptiveColor::rgb(0xE3E2DC, 0x24242C))
                                        .border_1()
                                        .border_color(AdaptiveColor::rgb(0xC5C4BC, 0x343440))
                                        .hover(|s| s.bg(palette::highest()))
                                        .items_center()
                                        .justify_center()
                                        .cursor_pointer()
                                        .on_press(move || {
                                            let p_op = p_open.clone();
                                            spawn(async move {
                                                let _ = open_path(p_op).await;
                                            });
                                        })
                                        .child(
                                            text("打开图像")
                                                .text_size(px(11.0))
                                                .text_color(palette::on_surface()),
                                        ),
                                )
                        } else {
                            view()
                        })
                }
                JobPhase::Failed(err_msg) => view()
                    .w_full()
                    .flex_row()
                    .items_center()
                    .justify_between()
                    .child(
                        view()
                            .flex_row()
                            .items_center()
                            .gap(px(10.0))
                            .child(view().size(px(6.0)).rounded_full().bg(palette::error()))
                            .child(
                                view()
                                    .h(px(20.0))
                                    .px(px(6.0))
                                    .rounded(px(4.0))
                                    .bg(palette::error_dim_bg())
                                    .border_1()
                                    .border_color(palette::error_dim_border())
                                    .items_center()
                                    .justify_center()
                                    .child(
                                        text("ERR")
                                            .text_size(px(10.0))
                                            .font_family(FONT_MONO)
                                            .text_color(palette::error()),
                                    ),
                            )
                            .child(
                                text(err_msg)
                                    .text_size(px(12.0))
                                    .text_color(palette::on_surface()),
                            ),
                    )
                    .child(
                        view()
                            .w(px(44.0))
                            .h(px(24.0))
                            .rounded(px(4.0))
                            .bg(palette::container_low())
                            .border_1()
                            .border_color(palette::outline_input())
                            .hover(|s| s.bg(palette::high()))
                            .items_center()
                            .justify_center()
                            .cursor_pointer()
                            .on_press(move || store.start_job())
                            .child(
                                text("重试")
                                    .text_size(px(11.0))
                                    .text_color(palette::on_surface()),
                            ),
                    ),
                JobPhase::Idle | JobPhase::Cancelled => {
                    let ready_str = format!("已就绪 · {} 项输入待处理", inputs.len());
                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .child(
                            text(ready_str)
                                .text_size(px(12.0))
                                .text_color(palette::muted()),
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

    let mut row = view().flex_row().items_center().gap(px(6.0));

    for (idx, item) in items.into_iter().enumerate().take(14) {
        let is_active = idx == active_idx;
        let border_col = if is_active {
            palette::primary()
        } else {
            match item.status {
                ItemStatus::Done { .. } | ItemStatus::Running(_) => palette::primary_dim_border(),
                ItemStatus::Failed(_) => palette::error(),
                _ => AdaptiveColor::rgb(0xD6D5CD, 0x25252D),
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

    view()
        .w_full()
        .h(px(54.0))
        .rounded(px(10.0))
        .bg(palette::surface())
        .border_1()
        .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x26262E))
        .px(px(8.0))
        .flex_row()
        .items_center()
        .justify_between()
        .child(row)
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(10.0))
                .child(
                    text(format!("{done} / {total}"))
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(palette::dim()),
                )
                .child(
                    view()
                        .w(px(1.0))
                        .h(px(18.0))
                        .bg(AdaptiveColor::rgb(0xD6D5CD, 0x26262E)),
                )
                .child(
                    view()
                        .size(px(28.0))
                        .rounded(px(6.0))
                        .bg(AdaptiveColor::rgb(0xECEBE6, 0x202027))
                        .border_1()
                        .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2E2E38))
                        .hover(|s| s.bg(palette::high()))
                        .items_center()
                        .justify_center()
                        .cursor_pointer()
                        .on_press(move || pick_image_files(store))
                        .child(svg_icon(crate::ui::icons::PLUS, 12.0, palette::muted())),
                ),
        )
}

fn inspector_column(store: Store) -> View {
    view()
        .w(px(308.0))
        .h_full()
        .bg(palette::bg())
        .relative()
        .flex_col()
        .justify_between()
        // Scrollable controls column (inner width 276px with 16px padding)
        .child(
            scroll()
                .flex_1()
                .w_full()
                .p(px(16.0))
                .flex_col()
                .gap(px(18.0))
                // ── Section 1: 模型 ──
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(10.0))
                        .child(section_header("模型"))
                        .child(model_picker_trigger(store)),
                )
                // ── Section 2: 输出 ──
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(12.0))
                        .child(section_header("输出"))
                        .child(segmented(
                            &[
                                SegmentItem {
                                    value: OutputFormat::Follow,
                                    label: "跟随",
                                },
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
                            store.format,
                            move |v| store.format.set(v),
                        ))
                        // Quality slider (when not PNG)
                        .child(when(
                            move || store.format.get().supports_quality(),
                            move || {
                                view()
                                    .w_full()
                                    .flex_col()
                                    .gap(px(8.0))
                                    .child(
                                        view()
                                            .w_full()
                                            .flex_row()
                                            .items_center()
                                            .justify_between()
                                            .child(
                                                text("质量")
                                                    .text_size(px(14.0))
                                                    .font_weight(FontWeight::MEDIUM)
                                                    .text_color(palette::on_surface()),
                                            )
                                            .child(
                                                text(move || store.quality.get().to_string())
                                                    .text_size(px(13.0))
                                                    .font_weight(FontWeight::MEDIUM)
                                                    .font_family(FONT_MONO)
                                                    .tabular_nums()
                                                    .text_color(palette::on_surface()),
                                            ),
                                    )
                                    .child(quality_slider(store.quality, 1, 100))
                            },
                        ))
                        // Chroma subsampling (when AVIF or JPG — Figma 73:1931: inline row, 14px label + 164x28 2-way segmented)
                        .child(when(
                            move || store.format.get().supports_chroma(),
                            move || {
                                view()
                                    .w_full()
                                    .h(px(28.0))
                                    .flex_row()
                                    .items_center()
                                    .justify_between()
                                    .child(
                                        text("色度抽样")
                                            .text_size(px(14.0))
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(palette::on_surface()),
                                    )
                                    .child(
                                        view().w(px(164.0)).child(segmented_with_font(
                                            &[
                                                SegmentItem {
                                                    value: ChromaSubsampling::Yuv420,
                                                    label: "4:2:0",
                                                },
                                                SegmentItem {
                                                    value: ChromaSubsampling::Yuv444,
                                                    label: "4:4:4",
                                                },
                                            ],
                                            store.chroma,
                                            28.0,
                                            12.0,
                                            true,
                                            move |v| store.chroma.set(v),
                                        )),
                                    )
                            },
                        ))
                        // AVIF speed (when AVIF — Figma 73:1931: inline row, 14px label + 164x28 3-way segmented)
                        .child(when(
                            move || store.format.get() == OutputFormat::Avif,
                            move || {
                                view()
                                    .w_full()
                                    .h(px(28.0))
                                    .flex_row()
                                    .items_center()
                                    .justify_between()
                                    .child(
                                        text("编码速度")
                                            .text_size(px(14.0))
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(palette::on_surface()),
                                    )
                                    .child(
                                        view().w(px(164.0)).child(segmented_sized(
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
                                            store.avif_speed,
                                            28.0,
                                            12.0,
                                            move |v| store.avif_speed.set(v),
                                        )),
                                    )
                            },
                        ))
                        // Export location (Figma: 13px #ececee, gap 8px, box 34px #16161a border #282830 radius 8px)
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(8.0))
                                .child(
                                    text("导出位置")
                                        .text_size(px(13.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_surface()),
                                )
                                .child(output_dir_box(store)),
                        ),
                )
                // ── Section 3: 引擎与处理 ──
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(12.0))
                        .child(section_header("引擎与处理"))
                        // Compute device (inline row: 14px #ececee + 164x28 segmented GPU/CPU)
                        .child(
                            view()
                                .w_full()
                                .h(px(28.0))
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    text("计算设备")
                                        .text_size(px(14.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_surface()),
                                )
                                .child(
                                    view().w(px(164.0)).child(segmented_sized(
                                        &[
                                            SegmentItem {
                                                value: true,
                                                label: "GPU",
                                            },
                                            SegmentItem {
                                                value: false,
                                                label: "CPU",
                                            },
                                        ],
                                        store.use_gpu,
                                        28.0,
                                        12.0,
                                        move |v| store.use_gpu.set(v),
                                    )),
                                ),
                        )
                        // Tile size (inline row: 14px #ececee + 164x28 segmented 自动/512/256/128)
                        .child(
                            view()
                                .w_full()
                                .h(px(28.0))
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    text("分块尺寸")
                                        .text_size(px(14.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_surface()),
                                )
                                .child(
                                    view().w(px(164.0)).child(segmented_sized(
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
                                        store.tile_size,
                                        28.0,
                                        12.0,
                                        move |v| store.tile_size.set(v),
                                    )),
                                ),
                        )
                        // Warm daemon toggle (inline row: 14px #ececee + 60fps spinning/active badge + 44x26 animated toggle)
                        .child(
                            view()
                                .flex_row()
                                .w_full()
                                .min_h(px(28.0))
                                .items_center()
                                .justify_between()
                                .gap(px(8.0))
                                .child(
                                    view()
                                        .flex_row()
                                        .items_center()
                                        .gap(px(6.0))
                                        .child(
                                            text("常驻热引擎")
                                                .text_size(px(14.0))
                                                .font_weight(FontWeight::MEDIUM)
                                                .text_color(palette::on_surface()),
                                        )
                                        .child(when(
                                            move || store.daemon_loading.get(),
                                            move || {
                                                view()
                                                    .flex_row()
                                                    .items_center()
                                                    .gap(px(5.0))
                                                    .child(
                                                        svg_icon(
                                                            crate::ui::icons::LOADER_FRAMES[0],
                                                            13.0,
                                                            palette::primary(),
                                                        )
                                                        .animate_spin(),
                                                    )
                                                    .child(
                                                        text("预热中")
                                                            .text_size(px(11.0))
                                                            .font_family(FONT_MONO)
                                                            .text_color(palette::primary()),
                                                    )
                                            },
                                        ))
                                        .child(when(
                                            move || {
                                                store.daemon_running.get()
                                                    && !store.daemon_loading.get()
                                            },
                                            move || {
                                                view()
                                                    .flex_row()
                                                    .items_center()
                                                    .gap(px(3.0))
                                                    .child(svg_icon(
                                                        crate::ui::icons::ZAP,
                                                        12.0,
                                                        palette::primary(),
                                                    ))
                                            },
                                        )),
                                )
                                .child(toggle_switch(
                                    move || store.daemon_running.get() || store.daemon_loading.get(),
                                    move |v| store.toggle_daemon(v),
                                )),
                        )
                        // Force overwrite toggle (inline row: 14px #ececee + 44x26 animated toggle)
                        .child(toggle_row(
                            "覆盖已有结果",
                            "",
                            move || store.force.get(),
                            move |v| store.force.set(v),
                        )),
                ),
        )
        // Bottom sticky CTA bar (16px padding, no top border in Figma)
        .child(
            view()
                .w_full()
                .px(px(16.0))
                .pb(px(16.0))
                .child(inspector_cta_button(store)),
        )
        // Floating model picker popup
        .child(when(
            move || store.show_model_picker.get(),
            move || model_picker_popup(store),
        ))
}

fn model_picker_trigger(store: Store) -> View {
    view()
        .w_full()
        .h(px(36.0))
        .rounded(px(8.0))
        .px(px(12.0))
        .flex_row()
        .items_center()
        .justify_between()
        .cursor_pointer()
        .bg(AdaptiveColor::rgb(0xFFFFFF, 0x18181D))
        .border_1()
        .border_color(move || {
            if store.show_model_picker.get() {
                palette::primary()
            } else {
                AdaptiveColor::rgb(0xCECDC5, 0x2D2D36)
            }
        })
        .hover(|s| s.bg(palette::container()))
        .active(|s| s.scale(0.99))
        .transition()
        .duration(150.0)
        .ease_out()
        .on_press(move || store.show_model_picker.update(|v| *v = !*v))
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .child(
                    text(move || store.current_model_variant().short_name)
                        .text_size(px(13.0))
                        .font_weight(FontWeight::MEDIUM)
                        .font_family(FONT_MONO)
                        .text_color(palette::on_surface()),
                )
                .child(
                    text(move || {
                        if store.current_model_variant().recommended {
                            "★"
                        } else {
                            ""
                        }
                    })
                    .text_size(px(11.0))
                    .text_color(palette::on_surface()),
                ),
        )
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .child(
                    view()
                        .h(px(20.0))
                        .px(px(6.0))
                        .rounded(px(4.0))
                        .bg(AdaptiveColor::rgb(0xEFECE6, 0x1C1C22))
                        .border_1()
                        .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2E2E37))
                        .items_center()
                        .justify_center()
                        .child(
                            text(move || format!("{}×", store.current_model_variant().scale))
                                .text_size(px(11.0))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .tabular_nums()
                                .text_color(palette::muted()),
                        ),
                )
                .child(
                    svg_icon(
                        crate::ui::icons::CHEVRON_DOWN,
                        14.0,
                        palette::muted(),
                    )
                    .rotate(move || {
                        deg(if store.show_model_picker.get() {
                            180.0
                        } else {
                            0.0
                        })
                    })
                    .transition_transform()
                    .duration(180.0)
                    .ease_out(),
                ),
        )
}

fn model_picker_popup(store: Store) -> View {
    view()
        .absolute()
        .top(px(82.0))
        .left(px(16.0))
        .right(px(16.0))
        .h(px(268.0))
        .rounded(px(8.0))
        .bg(AdaptiveColor::rgb(0xFFFFFF, 0x1C1C22))
        .border_1()
        .border_color(palette::outline_strong())
        .shadow_lg()
        .p(px(4.0))
        .flex_col()
        .gap(px(2.0))
        // Search box (34px, #1a1a1f, bottom border #7fdac9)
        .child(
            view()
                .w_full()
                .h(px(34.0))
                .rounded(px(6.0))
                .bg(AdaptiveColor::rgb(0xEAE9E4, 0x1A1A1F))
                .border_b_1()
                .border_color(palette::primary())
                .px(px(10.0))
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(svg_icon(crate::ui::icons::SEARCH, 14.0, palette::muted()))
                .child(
                    input()
                        .flex_1()
                        .placeholder("搜索模型…")
                        .placeholder_color(rgb(0xA2A2AC))
                        .caret_color(rgb(0x7FDAC9))
                        .value(move || store.picker_search.get())
                        .on_change(move |v: &str| store.picker_search.set(v.to_owned()))
                        .text_size(px(12.0))
                        .font_family(FONT_SANS)
                        .text_color(palette::on_surface())
                        .bg(AdaptiveColor::TRANSPARENT)
                        .border(px(0.0)),
                ),
        )
        // Scrollable 42px model rows
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
                            let sub_meta = format!(
                                "{} · x{}",
                                if m.size_bytes > 0 {
                                    human_bytes(m.size_bytes)
                                } else {
                                    "—".into()
                                },
                                m.scale
                            );

                            col = col.child(
                                view()
                                    .w_full()
                                    .h(px(42.0))
                                    .rounded(px(6.0))
                                    .px(px(10.0))
                                    .py(px(6.0))
                                    .flex_row()
                                    .items_center()
                                    .justify_between()
                                    .cursor_pointer()
                                    .bg(if is_sel {
                                        AdaptiveColor::rgb(0xE3E2DC, 0x26262F)
                                    } else {
                                        AdaptiveColor::TRANSPARENT
                                    })
                                    .border_1()
                                    .border_color(if is_sel {
                                        AdaptiveColor::rgb(0xC5C4BC, 0x343440)
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
                                            .flex_col()
                                            .gap(px(2.0))
                                            .child(
                                                text(m.name)
                                                    .text_size(px(12.0))
                                                    .font_weight(FontWeight::MEDIUM)
                                                    .font_family(FONT_MONO)
                                                    .text_color(palette::on_surface()),
                                            )
                                            .child(
                                                text(sub_meta)
                                                    .text_size(px(11.0))
                                                    .font_family(FONT_MONO)
                                                    .text_color(AdaptiveColor::rgb(0x706F68, 0x8E8E9A)),
                                            ),
                                    )
                                    .child(if is_sel {
                                        view()
                                            .size(px(6.0))
                                            .rounded_full()
                                            .bg(palette::primary())
                                    } else {
                                        view()
                                    }),
                            );
                        }
                    }
                    col
                })),
        )
}

fn output_dir_box(store: Store) -> View {
    view()
        .w_full()
        .h(px(34.0))
        .rounded(px(8.0))
        .bg(AdaptiveColor::rgb(0xEAE9E4, 0x16161A))
        .border_1()
        .border_color(palette::outline())
        .pl(px(10.0))
        .pr(px(4.0))
        .flex_row()
        .items_center()
        .justify_between()
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .overflow_hidden()
                .child(svg_icon(
                    crate::ui::icons::FOLDER,
                    13.0,
                    AdaptiveColor::rgb(0x706F68, 0x7E7E8A),
                ))
                .child(
                    text(move || match store.custom_out_dir.get() {
                        Some(ref p) => p
                            .file_name()
                            .and_then(|s| s.to_str())
                            .map(str::to_owned)
                            .unwrap_or_else(|| p.display().to_string()),
                        None => "跟随原图目录".to_string(),
                    })
                    .text_size(px(12.0))
                    .text_color(AdaptiveColor::rgb(0x27272A, 0xD0D0D8)),
                ),
        )
        .child(
            view()
                .h(px(24.0))
                .px(px(8.0))
                .rounded(px(6.0))
                .bg(palette::container())
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2E2E38))
                .hover(|s| s.bg(palette::high()))
                .active(|s| s.scale(0.96))
                .transition()
                .duration(140.0)
                .ease_out()
                .items_center()
                .justify_center()
                .cursor_pointer()
                .on_press(move || pick_export_folder(store))
                .child(
                    text(move || {
                        if store.custom_out_dir.get().is_some() {
                            "更改"
                        } else {
                            "指定…"
                        }
                    })
                    .text_size(px(11.0))
                    .font_weight(FontWeight::MEDIUM)
                    .text_color(palette::muted()),
                ),
        )
}

fn inspector_cta_button(store: Store) -> AnyElement {
    dynamic(move || {
        let phase = store.job_phase.get();
        let has_inputs = !store.inputs.get().is_empty();

        match phase {
            JobPhase::Running { .. } => view()
                .w_full()
                .h(px(44.0))
                .rounded(px(8.0))
                .bg(palette::error_dim_bg())
                .border_1()
                .border_color(palette::error_dim_border())
                .hover(|s| s.bg(palette::high()))
                .flex_row()
                .items_center()
                .justify_center()
                .gap(px(8.0))
                .cursor_pointer()
                .on_press(move || store.cancel_job())
                .child(
                    view()
                        .size(px(9.0))
                        .rounded(px(2.0))
                        .bg(palette::error()),
                )
                .child(
                    text("取消超分")
                        .text_size(px(14.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(palette::error()),
                )
                .child(
                    view()
                        .w(px(44.0))
                        .h(px(18.0))
                        .rounded(px(4.0))
                        .bg(AdaptiveColor::rgb(0xFCE9E7, 0x2A181A))
                        .border_1()
                        .border_color(AdaptiveColor::rgb(0xE8B4AE, 0x4A2528))
                        .items_center()
                        .justify_center()
                        .child(
                            text("Ctrl+.")
                                .text_size(px(10.0))
                                .font_family(FONT_MONO)
                                .text_color(AdaptiveColor::rgb(0xC23A2B, 0xFF897A)),
                        ),
                ),
            JobPhase::Done { .. } => primary_button("重新超分", true, move || store.start_job()),
            JobPhase::Failed(_) => primary_button("继续超分", true, move || store.start_job()),
            _ => primary_button("开始超分", true, move || {
                if has_inputs {
                    store.start_job();
                } else {
                    pick_image_files(store);
                }
            }),
        }
    })
}
