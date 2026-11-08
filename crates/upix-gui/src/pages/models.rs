//! Models Hub page (`desktop/src/main.cpp` `renderModelsPage`, Figma Frame `4:62`):
//! Centered 760px container with search bar (`220x34`), 5-way scene filter (`210x34`: `全部`/`动漫`/`照片`/`综合`/`修复`),
//! 3 square `32x32` icon buttons (`folder`, `download`, `refresh`), and grouped `ModelGroup` family cards.

use nsh_core::element::input;
use nsh_core::file_dialog::{FileFilter, OpenDialog};
use nsh_core::prelude::*;

use crate::models_local::{human_bytes, ModelGroup, ModelVariant};
use crate::store::Store;
use crate::theme::{palette, FONT_MONO, FONT_SANS};
use crate::ui::{icon_button, segmented_sized, svg_icon, SegmentItem};

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
            Self::General => scene.contains("综合") || scene.contains("通用") || scene.is_empty(),
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
                        .gap(px(14.0))
                        // Top bar (34px): Search (220x34) + 12px gap + Scene Filter (210x34) + Right 3 square icon buttons (32x32)
                        .child(
                            view()
                                .w_full()
                                .h(px(34.0))
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    view()
                                        .flex_row()
                                        .items_center()
                                        .gap(px(12.0))
                                        // Search input (220x34, #151519, border #282830, radius 8px)
                                        .child(
                                            view()
                                                .w(px(220.0))
                                                .h(px(34.0))
                                                .rounded(px(8.0))
                                                .bg(palette::recessed())
                                                .border_1()
                                                .border_color(palette::outline())
                                                .px(px(10.0))
                                                .flex_row()
                                                .items_center()
                                                .gap(px(8.0))
                                                .child(svg_icon(
                                                    crate::ui::icons::SEARCH,
                                                    14.0,
                                                    palette::muted(),
                                                ))
                                                .child(
                                                    input()
                                                        .flex_1()
                                                        .placeholder("搜索…")
                                                        .placeholder_color(rgb(0xA2A2AC))
                                                        .caret_color(rgb(0x7FDAC9))
                                                        .value(move || search_q.get())
                                                        .on_change(move |v: &str| {
                                                            search_q.set(v.to_owned())
                                                        })
                                                        .text_size(px(12.0))
                                                        .font_family(FONT_SANS)
                                                        .text_color(palette::on_surface())
                                                        .bg(AdaptiveColor::TRANSPARENT)
                                                        .border(px(0.0)),
                                                ),
                                        )
                                        // 5-way Scene Filter (210x34) with spring follower pill
                                        .child(view().w(px(210.0)).child(segmented_sized(
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
                                            scene_filter,
                                            34.0,
                                            12.0,
                                            move |v| scene_filter.set(v),
                                        ))),
                                )
                                // Right 3 square icon buttons (32x32, gap 6px)
                                .child(
                                    view()
                                        .flex_row()
                                        .items_center()
                                        .gap(px(6.0))
                                        .child(icon_button(
                                            crate::ui::icons::FOLDER,
                                            32.0,
                                            14.0,
                                            7.0,
                                            move || {
                                                let dir = store.models_dir.with(|d| d.clone());
                                                let _ = std::fs::create_dir_all(&dir);
                                                spawn(async move {
                                                    let _ = open_path(dir).await;
                                                });
                                            },
                                        ))
                                        .child(icon_button(
                                            crate::ui::icons::DOWNLOAD,
                                            32.0,
                                            14.0,
                                            7.0,
                                            move || {
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
                                            },
                                        ))
                                        .child(icon_button(
                                            crate::ui::icons::REFRESH,
                                            32.0,
                                            14.0,
                                            7.0,
                                            move || {
                                                store.rescan_models();
                                                store.fetch_remote_catalog();
                                            },
                                        )),
                                ),
                        )
                        // Groups content
                        .child(dynamic(move || {
                            render_model_sections(store, &search_q.get(), scene_filter.get())
                        })),
                ),
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
            let summary = format!("{} 款", items.len());
            Some(ModelGroup {
                title: g.title.clone(),
                summary,
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

    let mut col = view().w_full().flex_col().gap(px(14.0));

    // ── Section 1: 已安装 (Figma 82:2938) ──
    col = col.child(section_divider("已安装", installed_count));
    for g in installed {
        col = col.child(family_card(store, g, true));
    }

    // ── Section 2: 可下载 (Figma 82:2988) ──
    col = col.child(section_divider("可下载", remote_count));
    for g in remote {
        col = col.child(family_card(store, g, false));
    }

    col
}

fn section_divider(title: &'static str, count: usize) -> View {
    view()
        .w_full()
        .h(px(18.0))
        .flex_row()
        .items_center()
        .gap(px(10.0))
        .child(
            text(title)
                .text_size(px(12.0))
                .font_weight(FontWeight::MEDIUM)
                .text_color(palette::muted()),
        )
        .child(
            text(count.to_string())
                .text_size(px(12.0))
                .font_family(FONT_MONO)
                .tabular_nums()
                .text_color(palette::dim()),
        )
        .child(
            view()
                .flex_1()
                .h(px(1.0))
                .bg(AdaptiveColor::rgb(0xD6D5CD, 0x25252C)),
        )
}

fn family_card(store: Store, group: ModelGroup, is_installed_section: bool) -> View {
    let mut card = view()
        .w_full()
        .rounded(px(10.0))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline())
        .overflow_hidden()
        .flex_col()
        // 34px group header (#202026, left title #c7c7d1, right "{N} 款" #6e6e7a)
        .child(
            view()
                .w_full()
                .h(px(34.0))
                .bg(palette::container())
                .px(px(16.0))
                .flex_row()
                .items_center()
                .justify_between()
                .child(
                    text(group.title)
                        .text_size(px(12.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(AdaptiveColor::rgb(0x3F3E3A, 0xC7C7D1)),
                )
                .child(
                    text(group.summary)
                        .text_size(px(11.0))
                        .text_color(palette::dim()),
                ),
        );

    for m in group.items {
        card = card
            .child(
                view()
                    .w_full()
                    .h(px(1.0))
                    .bg(AdaptiveColor::rgb(0xDDDCDA, 0x23232A)),
            )
            .child(model_row(store, m, is_installed_section));
    }

    card
}

fn scene_dot_color(scene: &str) -> AdaptiveColor {
    if scene.contains("动漫") {
        palette::primary()
    } else if scene.contains("照片") && !scene.contains("修复") {
        AdaptiveColor::rgb(0xD97706, 0xE5A952)
    } else if scene.contains("修复") {
        AdaptiveColor::rgb(0xDB2777, 0xE07A9A)
    } else {
        AdaptiveColor::rgb(0x6366F1, 0x9BA3E8)
    }
}

fn model_row(store: Store, m: ModelVariant, is_installed_section: bool) -> View {
    let dot_col = scene_dot_color(&m.scene);
    let scene_label = if m.scene.is_empty() || m.scene == "通用" {
        "综合".to_string()
    } else {
        m.scene.clone()
    };

    let is_downloading = store.downloading_models.get().contains(&m.name);
    let is_selected = is_installed_section && store.selected_model.get() == m.name;
    let daemon_up = store.daemon_running.get();
    let daemon_loading = store.daemon_loading.get();
    let m_name = m.name.clone();
    let m_name_select = m.name.clone();

    let size_str = if m.size_bytes > 0 {
        human_bytes(m.size_bytes)
    } else {
        "—".to_string()
    };

    let mut row = view()
        .w_full()
        .h(px(38.0))
        .pl(px(16.0))
        .pr(px(14.0))
        .flex_row()
        .items_center()
        .justify_between()
        .hover(|s| s.bg(palette::container()))
        .transition_colors()
        .duration(140.0)
        .ease_out();

    if is_installed_section {
        row = row.cursor_pointer().on_press(move || {
            store.selected_model.set(m_name_select.clone());
        });
    }

    row
        // Left: 244px fixed Name Column (short_name 13px mono + optional "★" + optional zap) + Scene badge + Scale badge
        .child(
            view()
                .flex_row()
                .items_center()
                .child(
                    view()
                        .w(px(244.0))
                        .h(px(20.0))
                        .flex_row()
                        .items_center()
                        .gap(px(6.0))
                        .child(
                            text(m.short_name)
                                .text_size(px(13.0))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .text_color(palette::on_surface()),
                        )
                        .child(if m.recommended {
                            text("★")
                                .text_size(px(11.0))
                                .text_color(palette::muted())
                                .into_element()
                        } else {
                            view().into_element()
                        })
                        .child(if is_selected && daemon_loading {
                            svg_icon(
                                crate::ui::icons::LOADER_FRAMES[0],
                                12.0,
                                palette::primary(),
                            )
                            .animate_spin()
                            .into_element()
                        } else if is_selected && daemon_up {
                            svg_icon(crate::ui::icons::ZAP, 11.5, palette::primary()).into_element()
                        } else {
                            view().into_element()
                        }),
                )
                .child(
                    view()
                        .flex_row()
                        .items_center()
                        .gap(px(8.0))
                        // Scene badge (height 20px, #23232a, border #32323c, radius 4px, 5x5 dot + 11px text)
                        .child(
                            view()
                                .h(px(20.0))
                                .px(px(7.0))
                                .rounded(px(4.0))
                                .bg(AdaptiveColor::rgb(0xE8E7E1, 0x23232A))
                                .border_1()
                                .border_color(palette::outline_strong())
                                .flex_row()
                                .items_center()
                                .gap(px(5.0))
                                .child(view().size(px(5.0)).rounded_full().bg(dot_col))
                                .child(
                                    text(scene_label)
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(AdaptiveColor::rgb(0x3F3E3A, 0xC7C7D1)),
                                ),
                        )
                        // Scale badge (height 20px, #1c1c22, border #2e2e37, radius 4px, 11px mono #a2a2ac)
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
                                    text(format!("{}×", m.scale))
                                        .text_size(px(11.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::muted()),
                                ),
                        ),
                ),
        )
        // Right: File size (12px mono #8f8f9e) + 26x26 action icon button (trash / download / progress)
        .child(if is_installed_section {
            view()
                .flex_row()
                .items_center()
                .gap(px(12.0))
                .child(
                    text(size_str)
                        .text_size(px(12.0))
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(AdaptiveColor::rgb(0x706F68, 0x8F8F9E)),
                )
                .child(
                    view()
                        .size(px(26.0))
                        .rounded(px(6.0))
                        .hover(|s| s.bg(palette::error_dim_bg()))
                        .items_center()
                        .justify_center()
                        .cursor_pointer()
                        .on_press(move || store.delete_model(&m_name))
                        .child(svg_icon(
                            crate::ui::icons::TRASH,
                            13.5,
                            AdaptiveColor::rgb(0x8A8982, 0x5E5E6A),
                        )),
                )
        } else if is_downloading {
            view()
                .flex_row()
                .items_center()
                .gap(px(10.0))
                .child(
                    text(size_str)
                        .text_size(px(12.0))
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(AdaptiveColor::rgb(0x706F68, 0x8F8F9E)),
                )
                .child(
                    view()
                        .w(px(72.0))
                        .h(px(3.0))
                        .rounded(px(1.5))
                        .bg(AdaptiveColor::rgb(0xD6D5CD, 0x2A2A33))
                        .overflow_hidden()
                        .child(
                            view()
                                .h_full()
                                .w(percentage(0.6))
                                .rounded(px(1.5))
                                .bg(palette::primary()),
                        ),
                )
                .child(
                    text("60%")
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(palette::primary()),
                )
        } else {
            view()
                .flex_row()
                .items_center()
                .gap(px(12.0))
                .child(
                    text(size_str)
                        .text_size(px(12.0))
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(AdaptiveColor::rgb(0x706F68, 0x8F8F9E)),
                )
                .child(icon_button(
                    crate::ui::icons::DOWNLOAD,
                    26.0,
                    13.0,
                    6.0,
                    move || store.download_model(m_name.clone()),
                ))
        })
}
