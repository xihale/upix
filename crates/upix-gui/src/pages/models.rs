//! 模型: installed list (scene chips, 推荐, ★), the downloadable catalog and
//! local import — the desktop ModelsPage.qml grammar. Download runs on a
//! worker through `upix_host::modelctl` (default `integration` build).

use nsh_core::file_dialog::OpenDialog;
use nsh_base::theme;
use nsh_core::prelude::*;

use crate::models_local::{ModelRow, CATALOG};
use crate::store::Store;
use crate::ui;

pub fn page(store: Store) -> impl IntoElement {
    scroll().flex_1().child(
        view()
            .p_4()
            .gap_4()
            .child(ui::page_title("模型"))
            .child(toolbar(store))
            .child(ui::section_label("已安装"))
            .child(installed_list(store))
            .child(ui::section_label("可下载"))
            .child(catalog_list(store))
            .child(import_log(store)),
    )
}

fn toolbar(store: Store) -> impl IntoElement {
    view()
        .flex_row()
        .gap_2()
        .child(ui::ghost_button("打开目录", move || {
            let dir = store.models_dir.get().clone();
            spawn(async move {
                let _ = open_path(dir).await;
            });
        }))
        .child(ui::ghost_button("刷新", move || store.refresh_models()))
        .child(
            ui::ghost_button("导入", move || {
                let picked = pick_paths(
                    OpenDialog::files()
                        .multiple(true)
                        .filter(nsh_core::file_dialog::FileFilter::new(
                            "模型",
                            ["param", "bin"],
                        ))
                        .button_label("导入模型"),
                );
                let store = store;
                spawn(async move {
                    if let Ok(Some(paths)) = picked.await {
                        store.import_models(&paths);
                    }
                });
            }),
        )
        .child(
            text(move || store.models_dir.get().display().to_string())
                .text_xs()
                .font_family(crate::ui::mono_font())
                .text_color(move || theme().muted_foreground)
                .flex_1()
                .number_of_lines(1),
        )
}

/// The installed rows: an `each` keyed by model name, so a refresh keeps
/// rows and bindings alive instead of rebuilding the card.
fn installed_list(store: Store) -> impl IntoElement {
    ui::card_with(move || {
        view()
            .gap_1()
            .child(each(
                store.installed,
                |row: &ModelRow| row.name.clone(),
                move |row: ReadSignal<ModelRow>, _index: ReadSignal<usize>| {
                    model_row(store, row)
                },
            ))
            .child(when(
                move || store.installed.get().is_empty(),
                move || {
                    text("未发现模型")
                        .text_sm()
                        .text_color(move || theme().muted_foreground)
                        .into_element()
                },
            ))
    })
}

fn model_row(store: Store, row: ReadSignal<ModelRow>) -> impl IntoElement {
    let starred = move || {
        let name = row.with(|r| r.name.clone());
        store.favorites.get().iter().any(|f| *f == name)
    };
    view()
        .flex_row()
        .items_center()
        .gap_2()
        .w_full()
        .p_2()
        .rounded(move || theme().radius)
        .hover(|s| s.bg(move || theme().accent))
        .child(
            text(move || {
                if starred() {
                    "★"
                } else {
                    "☆"
                }
            })
            .text_sm()
            .text_color(move || {
                if starred() {
                    theme().primary
                } else {
                    theme().muted_foreground
                }
            })
            .on_press(move || {
                let name = row.get_untracked().name;
                store.toggle_favorite(&name);
            }),
        )
        .child(
            text(move || row.with(|r| r.name.clone()))
                .text_sm()
                .flex_1()
                .number_of_lines(1),
        )
        .child(ui::chip(
            move || format!("x{}", row.with(|r| r.scale)),
            crate::theme::muted_ink(),
        ))
        .child(when(
            move || !row.with(|r| r.scene.is_empty()),
            move || {
                ui::chip(
                    move || row.with(|r| r.scene.clone()),
                    crate::theme::primary_ink(),
                )
                .into_element()
            },
        ))
        .child(when(
            move || row.with(|r| r.recommended) || starred(),
            move || ui::chip("推荐", crate::theme::primary_ink()).into_element(),
        ))
        .child(when(
            move || !row.with(|r| r.has_bin),
            move || {
                ui::chip("缺 .bin", move || theme().destructive).into_element()
            },
        ))
}

fn catalog_list(store: Store) -> impl IntoElement {
    ui::card_with(move || {
        view().gap_1().children(
            CATALOG
                .iter()
                .map(|entry| {
                    // `entry` is &'static; its closures stay reactive on the
                    // installed signal.
                    let have = move || {
                        store
                            .installed
                            .get()
                            .iter()
                            .any(|row| row.name.eq_ignore_ascii_case(entry.name))
                    };
                    view()
                        .flex_row()
                        .items_center()
                        .gap_2()
                        .p_2()
                        .w_full()
                        .child(
                            text(entry.name)
                                .text_sm()
                                .flex_1()
                                .number_of_lines(1),
                        )
                        .child(ui::chip(
                            format!("x{}", entry.scale),
                            crate::theme::muted_ink(),
                        ))
                        .child(when(
                            move || !entry.scene.is_empty(),
                            move || {
                                ui::chip(entry.scene, crate::theme::primary_ink()).into_element()
                            },
                        ))
                        .child(when(
                            || entry.recommended,
                            move || ui::chip("推荐", crate::theme::primary_ink()).into_element(),
                        ))
                        .child(when(
                            have,
                            move || {
                                ui::chip("已安装", move || theme().muted_foreground)
                                    .into_element()
                            },
                        ))
                        .child(when(
                            move || !have(),
                            move || {
                                let busy = move || {
                                    store.downloading.get().as_deref() == Some(entry.name)
                                };
                                ui::ghost_button(
                                    move || {
                                        if busy() {
                                            "下载中".to_owned()
                                        } else {
                                            "下载".to_owned()
                                        }
                                    },
                                    move || store.download_model(entry.name),
                                )
                                .disabled(move || store.downloading.get().is_some())
                                .into_element()
                            },
                        ))
                })
                .collect::<Vec<_>>(),
        )
    })
}

fn import_log(store: Store) -> impl IntoElement {
    when(
        move || !store.import_log.get().is_empty(),
        move || {
            ui::card_with(move || {
                view().gap_1().children(
                    store
                        .import_log
                        .get()
                        .iter()
                        .map(|line| {
                            text(line.clone())
                                .text_xs()
                                .font_family(crate::ui::mono_font())
                                .text_color(move || theme().muted_foreground)
                        })
                        .collect::<Vec<_>>(),
                )
            })
            .into_element()
        },
    )
}
