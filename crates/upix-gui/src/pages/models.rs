//! Models Hub page (`desktop/src/main.cpp` `render_models_tab`):
//! Centered 760px container with search bar, 5-way scene filter (`全部`/`动漫`/`照片`/`综合`/`修复`),
//! folder/import/refresh actions, and grouped `ModelGroup` family cards (`upix_core::model_families`).

use nsh_core::element::input;
use nsh_core::file_dialog::{FileFilter, OpenDialog};
use nsh_core::prelude::*;

use crate::models_local::{human_bytes, ModelGroup, ModelVariant};
use crate::nav::NavTab;
use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::{segmented, svg_icon, SegmentItem};

#[derive(Clone, Copy, PartialEq, Eq)]
enum SceneFilter {
    All,
    Anime,
    Photo,
    General,
    Restore,
}

impl SceneFilter {
    fn matches(self, scene: &str) -> bool {
        match self {
            Self::All => true,
            Self::Anime => scene.contains("动漫"),
            Self::Photo => scene.contains("照片") && !scene.contains("修复"),
            Self::General => scene.contains("综合") || scene.is_empty(),
            Self::Restore => scene.contains("修复"),
        }
    }
}

pub fn models_page(store: Store) -> View {
    let search_q = signal(String::new());
    let scene_filter = signal(SceneFilter::All);

    view()
        .flex_1()
        .h_full()
        .bg(palette::bg())
        .items_center()
        .child(
            scroll()
                .w_full()
                .h_full()
                .items_center()
                .py(px(28.0))
                .child(
                    view()
                        .w(px(760.0))
                        .flex_col()
                        .gap(px(20.0))
                        // Top bar: Search + Scene Filter + 3 action buttons
                        .child(
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
                                        // Search input (220x32)
                                        .child(
                                            view()
                                                .w(px(220.0))
                                                .h(px(32.0))
                                                .rounded(px(6.0))
                                                .bg(palette::container_low())
                                                .border_1()
                                                .border_color(palette::outline())
                                                .px(px(10.0))
                                                .flex_row()
                                                .items_center()
                                                .gap(px(8.0))
                                                .child(
                                                    input()
                                                        .flex_1()
                                                        .placeholder("搜索模型...")
                                                        .value(move || search_q.get())
                                                        .on_change(move |v: &str| search_q.set(v.to_owned()))
                                                        .text_size(px(12.0))
                                                        .text_color(palette::on_surface())
                                                        .bg(AdaptiveColor::TRANSPARENT)
                                                        .border(px(0.0)),
                                                ),
                                        )
                                        // 5-way Scene Filter (250px)
                                        .child(view().w(px(250.0)).child(dynamic(move || {
                                            segmented(
                                                &[
                                                    SegmentItem {
                                                        value: SceneFilter::All,
                                                        label: "全部",
                                                    },
                                                    SegmentItem {
                                                        value: SceneFilter::Anime,
                                                        label: "动漫",
                                                    },
                                                    SegmentItem {
                                                        value: SceneFilter::Photo,
                                                        label: "照片",
                                                    },
                                                    SegmentItem {
                                                        value: SceneFilter::General,
                                                        label: "综合",
                                                    },
                                                    SegmentItem {
                                                        value: SceneFilter::Restore,
                                                        label: "修复",
                                                    },
                                                ],
                                                scene_filter.get(),
                                                move |v| scene_filter.set(v),
                                            )
                                        }))),
                                )
                                // Right action buttons
                                .child(
                                    view()
                                        .flex_row()
                                        .items_center()
                                        .gap(px(6.0))
                                        .child(action_btn("打开目录", move || {
                                            let dir = store.models_dir.with(|d| d.clone());
                                            let _ = std::fs::create_dir_all(&dir);
                                            spawn(async move {
                                                let _ = open_path(dir).await;
                                            });
                                        }))
                                        .child(action_btn("导入模型", move || {
                                            let picked = pick_paths(
                                                OpenDialog::files()
                                                    .multiple(true)
                                                    .filter(FileFilter::new(
                                                        "NCNN 模型",
                                                        ["param", "bin"],
                                                    ))
                                                    .button_label("导入模型"),
                                            );
                                            spawn(async move {
                                                if let Ok(Some(files)) = picked.await {
                                                    store.import_model_files(&files);
                                                }
                                            });
                                        }))
                                        .child(action_btn("刷新目录", move || {
                                            store.rescan_models();
                                            store.fetch_remote_catalog();
                                        })),
                                ),
                        )
                        // Status line if non-empty
                        .child(dynamic(move || {
                            let st = store.models_status.get();
                            if st.is_empty() {
                                view().h(px(0.0))
                            } else {
                                view()
                                    .w_full()
                                    .px(px(12.0))
                                    .py(px(8.0))
                                    .rounded(px(6.0))
                                    .bg(palette::container_low())
                                    .border_1()
                                    .border_color(palette::outline())
                                    .child(
                                        text(st)
                                            .text_size(px(11.0))
                                            .text_color(palette::muted()),
                                    )
                            }
                        }))
                        // Groups content
                        .child(dynamic(move || {
                            render_model_sections(store, &search_q.get(), scene_filter.get())
                        })),
                ),
        )
}

fn action_btn(label: &'static str, on_click: impl Fn() + 'static) -> View {
    view()
        .h(px(30.0))
        .px(px(10.0))
        .rounded(px(6.0))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline())
        .hover(|s| s.bg(palette::high()))
        .items_center()
        .justify_center()
        .cursor_pointer()
        .on_press(move || on_click())
        .child(
            text(label)
                .text_size(px(11.0))
                .font_weight(FontWeight::MEDIUM)
                .text_color(palette::on_surface()),
        )
}

fn render_model_sections(store: Store, query: &str, scene_f: SceneFilter) -> View {
    let q = query.trim().to_ascii_lowercase();

    let filter_group = |g: &ModelGroup, only_uninstalled: bool| -> Option<ModelGroup> {
        let items: Vec<ModelVariant> = g
            .items
            .iter()
            .filter(|m| {
                if only_uninstalled && m.installed {
                    return false;
                }
                let scene_ok = scene_f.matches(&m.scene);
                let q_ok = q.is_empty()
                    || m.name.to_ascii_lowercase().contains(&q)
                    || m.short_name.to_ascii_lowercase().contains(&q)
                    || g.title.to_ascii_lowercase().contains(&q)
                    || m.scene.to_ascii_lowercase().contains(&q);
                scene_ok && q_ok
            })
            .cloned()
            .collect();
        if items.is_empty() {
            None
        } else {
            Some(ModelGroup {
                title: g.title.clone(),
                summary: g.summary.clone(),
                scene: g.scene.clone(),
                recommended: g.recommended,
                items,
            })
        }
    };

    let installed: Vec<ModelGroup> = store
        .installed_groups
        .get()
        .iter()
        .filter_map(|g| filter_group(g, false))
        .collect();
    let remote: Vec<ModelGroup> = store
        .remote_groups
        .get()
        .iter()
        .filter_map(|g| filter_group(g, true))
        .collect();

    let installed_count: usize = installed.iter().map(|g| g.items.len()).sum();
    let remote_count: usize = remote.iter().map(|g| g.items.len()).sum();

    let mut col = view().w_full().flex_col().gap(px(12.0));

    // ── Section 1: 已安装 ──
    col = col.child(section_divider("已安装", installed_count));
    if installed.is_empty() {
        col = col.child(
            view()
                .w_full()
                .h(px(64.0))
                .rounded(px(10.0))
                .bg(palette::surface())
                .border_1()
                .border_color(palette::outline())
                .items_center()
                .justify_center()
                .child(
                    text("暂无匹配的已安装模型")
                        .text_size(px(12.0))
                        .text_color(palette::dim()),
                ),
        );
    } else {
        for g in installed {
            col = col.child(family_card(store, g, true));
        }
    }

    // ── Section 2: 可下载 ──
    col = col.child(view().h(px(4.0)));
    col = col.child(section_divider("可下载", remote_count));
    if store.remote_loading.get() && remote.is_empty() {
        col = col.child(
            view()
                .w_full()
                .h(px(64.0))
                .rounded(px(10.0))
                .bg(palette::surface())
                .border_1()
                .border_color(palette::outline())
                .items_center()
                .justify_center()
                .child(
                    text("正在获取云端模型目录...")
                        .text_size(px(12.0))
                        .text_color(palette::dim()),
                ),
        );
    } else if remote.is_empty() {
        col = col.child(
            view()
                .w_full()
                .h(px(64.0))
                .rounded(px(10.0))
                .bg(palette::surface())
                .border_1()
                .border_color(palette::outline())
                .items_center()
                .justify_center()
                .child(
                    text("所有匹配的模型均已安装，或点击右上角刷新目录同步")
                        .text_size(px(12.0))
                        .text_color(palette::dim()),
                ),
        );
    } else {
        for g in remote {
            col = col.child(family_card(store, g, false));
        }
    }

    col
}

fn section_divider(title: &'static str, count: usize) -> View {
    view()
        .w_full()
        .flex_row()
        .items_center()
        .gap(px(10.0))
        .child(
            text(title)
                .text_size(px(11.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(palette::muted()),
        )
        .child(
            text(count.to_string())
                .text_size(px(11.0))
                .font_family(FONT_MONO)
                .tabular_nums()
                .text_color(palette::dim()),
        )
        .child(view().flex_1().h(px(1.0)).bg(palette::outline()))
}

fn family_card(store: Store, group: ModelGroup, is_installed_section: bool) -> View {
    let mut card = view()
        .w_full()
        .rounded(px(10.0))
        .bg(palette::surface())
        .border_1()
        .border_color(palette::outline())
        .overflow_hidden()
        .flex_col()
        // 34px group header
        .child(
            view()
                .w_full()
                .h(px(34.0))
                .bg(palette::container_low())
                .px(px(16.0))
                .flex_row()
                .items_center()
                .gap(px(10.0))
                .child(
                    text(group.title)
                        .text_size(px(12.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(palette::on_surface()),
                )
                .child(
                    text(group.summary)
                        .text_size(px(11.0))
                        .text_color(palette::dim()),
                ),
        );

    for m in group.items {
        card = card
            .child(view().w_full().h(px(1.0)).bg(palette::outline()))
            .child(model_row(store, m, is_installed_section));
    }

    card
}

fn model_row(store: Store, m: ModelVariant, is_installed_section: bool) -> View {
    let scene_col = if m.scene.contains("动漫") {
        palette::scene_anime()
    } else if m.scene.contains("修复") {
        palette::scene_restore()
    } else if m.scene.contains("照片") {
        palette::scene_photo()
    } else {
        palette::scene_general()
    };

    let is_downloading = store.downloading_models.get().contains(&m.name);
    let is_selected = store.selected_model.get() == m.name;
    let m_name = m.name.clone();
    let m_name_select = m.name.clone();

    view()
        .w_full()
        .h(px(40.0))
        .px(px(16.0))
        .flex_row()
        .items_center()
        .justify_between()
        .hover(|s| s.bg(palette::container_low()))
        // Left: short_name + star + scene badge + full stem
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(
                    text(m.short_name)
                        .text_size(px(12.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .font_family(FONT_MONO)
                        .text_color(palette::on_surface()),
                )
                .child(if m.recommended {
                    text("★ 推荐")
                        .text_size(px(10.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(palette::star_gold())
                        .into_element()
                } else {
                    view().into_element()
                })
                // Scene badge with colored dot
                .child(if !m.scene.is_empty() {
                    view()
                        .h(px(18.0))
                        .px(px(7.0))
                        .rounded(px(4.0))
                        .bg(palette::container_low())
                        .border_1()
                        .border_color(palette::outline())
                        .flex_row()
                        .items_center()
                        .gap(px(5.0))
                        .child(view().size(px(5.0)).rounded_full().bg(scene_col))
                        .child(
                            text(m.scene)
                                .text_size(px(10.0))
                                .text_color(palette::muted()),
                        )
                        .into_element()
                } else {
                    view().into_element()
                })
                .child(
                    text(m.name)
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .text_color(palette::dim()),
                ),
        )
        // Right: Scale badge + Size + Select/Delete/Download buttons
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(10.0))
                // Scale badge
                .child(
                    view()
                        .h(px(20.0))
                        .px(px(6.0))
                        .rounded(px(4.0))
                        .bg(palette::container_low())
                        .border_1()
                        .border_color(palette::outline())
                        .items_center()
                        .justify_center()
                        .child(
                            text(format!("{}×", m.scale))
                                .text_size(px(10.0))
                                .font_family(FONT_MONO)
                                .tabular_nums()
                                .text_color(palette::muted()),
                        ),
                )
                // File size (when installed)
                .child(if is_installed_section && m.size_bytes > 0 {
                    view()
                        .w(px(64.0))
                        .items_end()
                        .child(
                            text(human_bytes(m.size_bytes))
                                .text_size(px(11.0))
                                .font_family(FONT_MONO)
                                .tabular_nums()
                                .text_color(palette::dim()),
                        )
                } else {
                    view()
                })
                // Action buttons
                .child(if is_installed_section {
                    view()
                        .flex_row()
                        .items_center()
                        .gap(px(6.0))
                        .child(
                            view()
                                .h(px(26.0))
                                .px(px(10.0))
                                .rounded(px(6.0))
                                .bg(if is_selected {
                                    palette::primary_dim_bg()
                                } else {
                                    palette::container_low()
                                })
                                .border_1()
                                .border_color(if is_selected {
                                    palette::primary_dim_border()
                                } else {
                                    palette::outline()
                                })
                                .hover(|s| s.bg(palette::high()))
                                .items_center()
                                .justify_center()
                                .cursor_pointer()
                                .on_press(move || {
                                    store.selected_model.set(m_name_select.clone());
                                    store.nav.set(NavTab::Workbench);
                                })
                                .child(
                                    text(if is_selected { "当前模型" } else { "选用" })
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(if is_selected {
                                            palette::primary()
                                        } else {
                                            palette::on_surface()
                                        }),
                                ),
                        )
                        .child(
                            view()
                                .size(px(26.0))
                                .rounded(px(6.0))
                                .bg(palette::container_low())
                                .border_1()
                                .border_color(palette::outline())
                                .hover(|s| s.bg(palette::error_dim_bg()))
                                .items_center()
                                .justify_center()
                                .cursor_pointer()
                                .on_press(move || store.delete_model(&m_name))
                                .child(svg_icon(
                                    crate::ui::icons::CLOSE,
                                    11.0,
                                    palette::muted(),
                                )),
                        )
                } else if is_downloading {
                    view()
                        .h(px(26.0))
                        .px(px(10.0))
                        .rounded(px(6.0))
                        .bg(palette::primary_dim_bg())
                        .border_1()
                        .border_color(palette::primary_dim_border())
                        .items_center()
                        .justify_center()
                        .child(
                            text("下载中…")
                                .text_size(px(10.0))
                                .font_family(FONT_MONO)
                                .text_color(palette::primary()),
                        )
                } else {
                    view()
                        .h(px(26.0))
                        .px(px(10.0))
                        .rounded(px(6.0))
                        .bg(palette::primary_dim_bg())
                        .border_1()
                        .border_color(palette::primary_dim_border())
                        .hover(|s| s.bg(palette::toggle_on_bg()))
                        .items_center()
                        .justify_center()
                        .cursor_pointer()
                        .on_press(move || store.download_model(m_name.clone()))
                        .child(
                            text("下载")
                                .text_size(px(11.0))
                                .font_weight(FontWeight::SEMIBOLD)
                                .text_color(palette::primary()),
                        )
                }),
        )
}
