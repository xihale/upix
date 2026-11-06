//! 首页: pick an image (drop, dialog, or path input), choose format /
//! quality / scale / model, and start the upscale. Copy follows the desktop
//! Workbench/Inspector (拖入图片 / 开始超分 / 质量 …).

use std::path::PathBuf;

use nsh_base::{select, theme, SelectItem};
use nsh_core::file_dialog::{FileFilter, OpenDialog};
use nsh_core::prelude::*;

use crate::store::{JobState, Store};
use crate::ui;

pub fn page(store: Store) -> impl IntoElement {
    scroll()
        .flex_1()
        .child(
            view()
                .p_4()
                .gap_4()
                .child(ui::page_title("首页"))
                .child(source_card(store))
                .child(settings_card(store))
                .child(start_row(store)),
        )
}

/// The drop zone, preview, and the three ways in: drag-drop, the system
/// dialog, and a path field (the fallback when no portal/dialog exists).
fn source_card(store: Store) -> impl IntoElement {
    ui::card_with(
        move || match store.input.get() {
            Some(img) => view()
                .gap_3()
                .child(
                    image(move || img.display_src.clone())
                        .w_full()
                        .h(px(240.0))
                        .object_fit(ResizeMode::Contain)
                        .rounded(move || theme().radius)
                        .bg(move || theme().muted),
                )
                .child(
                    text(move || {
                        let dims = if img.width > 0 {
                            format!(" · {}×{}", img.width, img.height)
                        } else {
                            String::new()
                        };
                        format!("{}{}", img.path.display(), dims)
                    })
                    .text_xs()
                    .number_of_lines(1)
                    .text_color(move || theme().muted_foreground),
                )
                .into_element(),
            None => drop_zone(store).into_element(),
        },
    )
}

fn drop_zone(store: Store) -> impl IntoElement {
    let draft = store.path_draft;
    view()
        .w_full()
        .h(px(240.0))
        .items_center()
        .justify_center()
        .gap_3()
        .rounded(move || theme().radius)
        .border_1()
        .border_color(move || theme().border)
        .bg(move || theme().muted)
        // Files from the desktop's file manager land here.
        .on_file_drop(move |e: &FileDropEvent| {
            if let Some(path) = e.uris.first().and_then(|uri| file_uri_path(uri)) {
                store.set_source(path);
            }
        })
        .child(
            text("拖入图片到此，或点击下方选择")
                .text_sm()
                .text_color(move || theme().muted_foreground),
        )
        .child(
            text("支持 PNG / JPEG / WebP（粘贴路径亦可）")
                .text_xs()
                .text_color(move || theme().muted_foreground),
        )
        .child(
            view()
                .flex_row()
                .items_center()
                .gap_2()
                .child(
                    ui::primary_button("选择图片", move || {
                        let picked = pick_paths(
                            OpenDialog::files()
                                .filter(FileFilter::new("图片", ["png", "jpg", "jpeg", "webp", "avif"]))
                                .button_label("选择图片"),
                        );
                        let store = store;
                        spawn(async move {
                            match picked.await {
                                Ok(Some(paths)) if !paths.is_empty() => {
                                    store.set_source(paths[0].clone());
                                }
                                Ok(_) => {} // cancelled
                                Err(error) => {
                                    log::warn!("没有文件对话框，请粘贴路径: {error}");
                                }
                            }
                        });
                    }),
                )
                .child(
                    ui::field("或输入图片路径…", draft, {
                        let store = store;
                        move |text: &str| {
                            let path = PathBuf::from(text.trim());
                            if !path.as_os_str().is_empty() {
                                store.set_source(path);
                            }
                        }
                    })
                    .w(px(300.0)),
                ),
        )
}

/// 格式 / 质量 / 倍数 / 模型.
fn settings_card(store: Store) -> impl IntoElement {
    ui::card_with(move || {
        view()
            .gap_4()
            .child(
                view()
                    .flex_row()
                    .gap_4()
                    .child(
                        view()
                            .gap_2()
                            .child(ui::section_label("输出格式"))
                            .child(
                                app_select(
                                    &[
                                        SelectItem::new("png", "PNG"),
                                        SelectItem::new("jpg", "JPEG"),
                                        SelectItem::new("webp", "WebP"),
                                        SelectItem::new("avif", "AVIF"),
                                    ],
                                    move || {
                                        let v = store.format.get();
                                        if v.is_empty() {
                                            None
                                        } else {
                                            Some(SharedString::from(v))
                                        }
                                    },
                                    move |v: &Option<SharedString>| {
                                        if let Some(v) = v {
                                            store.format.set(v.to_string());
                                        }
                                    },
                                ),
                            ),
                    )
                    .child(
                        view()
                            .gap_2()
                            .child(ui::section_label("放大倍数"))
                            .child(
                                app_select(
                                    &[SelectItem::new("2", "x2"), SelectItem::new("4", "x4")],
                                    move || {
                                        Some(SharedString::from(store.scale.get().to_string()))
                                    },
                                    move |v: &Option<SharedString>| {
                                        if let Some(v) = v {
                                            store.scale.set(v.parse().unwrap_or(2));
                                        }
                                    },
                                ),
                            ),
                    )
                    .child(
                        view()
                            .gap_2()
                            .child(ui::section_label("模型"))
                            .child(
                                app_select(
                                    &model_items(&store),
                                    move || store.model.get().map(SharedString::from),
                                    move |v: &Option<SharedString>| {
                                        store.model.set(v.as_ref().map(|v| v.to_string()));
                                    },
                                ),
                            ),
                    ),
            )
            .child(
                view()
                    .gap_2()
                    .child(
                        view()
                            .flex_row()
                            .items_center()
                            .gap_2()
                            .child(text("质量").text_sm())
                            .child(
                                text(move || format!("{}", store.quality.get() as i32))
                                    .text_sm()
                                    .tabular_nums()
                                    .text_color(move || theme().muted_foreground),
                            ),
                    )
                    .child(
                        nsh_base::slider()
                            .value(move || nsh_base::SliderValue::Single(store.quality.get()))
                            .min(1.0)
                            .max(100.0)
                            .step(1.0)
                            .on_change(move |v: &nsh_base::SliderValue| {
                                if let nsh_base::SliderValue::Single(v) = v {
                                    store.quality.set(*v);
                                }
                            })
                            .track_style(|s| s.h(px(4.0)).rounded_full().bg(move || theme().secondary))
                            .indicator_style(|s| s.rounded_full().bg(move || theme().primary))
                            .thumb_style(|s| {
                                s.size(px(16.0))
                                    .rounded_full()
                                    .bg(move || theme().background)
                                    .border_1()
                                    .border_color(move || theme().border)
                            }),
                    ),
            )
    })
}

fn start_row(store: Store) -> impl IntoElement {
    let busy = move || matches!(store.job.get(), JobState::Running { .. });
    view()
        .flex_row()
        .items_center()
        .gap_3()
        .child({
            let busy2 = busy;
            ui::primary_button(
                move || {
                    if busy2() {
                        SharedString::from("处理中…")
                    } else {
                        SharedString::from("开始超分")
                    }
                },
                move || store.start_job(),
            )
            .disabled(move || store.input.get().is_none() || busy2())
        })
        .child(
            text(move || {
                if store.input.get().is_none() {
                    "先选一张图片".to_owned()
                } else if busy() {
                    "有任务在跑".to_owned()
                } else {
                    format!(
                        "输出 {} ×{}，质量 {}",
                        store.format.get().to_uppercase(),
                        store.scale.get(),
                        store.quality.get() as i32
                    )
                }
            })
            .text_xs()
            .text_color(move || theme().muted_foreground),
        )
        .child(
            text("当前为模拟引擎（PNG 输入、进度与对比全链路可试）")
                .text_xs()
                .text_color(move || theme().muted_foreground),
        )
}

fn model_items(store: &Store) -> Vec<SelectItem> {
    let mut items = vec![SelectItem::new("", "默认（引擎自动）")];
    for row in store.installed.get() {
        items.push(SelectItem::new(row.name.clone(), row.name.clone()));
    }
    items
}

/// The app's select: trigger + popup look, both required (parts draw nothing
/// until styled).
pub fn app_select(
    items: &[SelectItem],
    value: impl Fn() -> Option<SharedString> + Clone + 'static,
    on_change: impl Fn(&Option<SharedString>) + 'static,
) -> impl IntoElement {
    let chosen = value.clone();
    let trigger_text = chosen.clone();
    select()
        .items(items.to_vec())
        .value(chosen)
        .on_change(on_change)
        .accessibility_label("选择")
        .popup_style(|s| {
            s.p_1()
                .rounded_lg()
                .border_1()
                .border_color(move || theme().border)
                .bg(move || theme().popover)
                .shadow_md()
        })
        .item_class("rounded-md focus:bg-accent selected:font-medium")
        .render_item(|item: &nsh_base::SelectEntry| {
            view().px_2().py_1p5().child(text(item.title.clone()).text_sm())
        })
        .trigger(
            view()
                .h(px(36.0))
                .px_3()
                .items_center()
                .justify_center()
                .rounded(move || theme().radius)
                .border_1()
                .border_color(move || theme().input)
                .child(
                    text(move || {
                        trigger_text()
                            .filter(|v| !v.is_empty())
                            .unwrap_or_else(|| "选择".into())
                    })
                    .text_sm(),
                ),
        )
}

/// `file://…` URI → path (percent-decoded).
pub fn file_uri_path(uri: &str) -> Option<PathBuf> {
    let rest = uri.strip_prefix("file://")?;
    let rest = rest.strip_prefix("/").map(|r| format!("/{r}")).unwrap_or(rest.to_owned());
    Some(PathBuf::from(percent_decode(&rest)))
}

pub fn percent_decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or("");
            if let Ok(v) = u8::from_str_radix(hex, 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}
