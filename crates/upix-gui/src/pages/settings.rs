//! 设置 page — the engine, encoder and directory controls live here so the
//! workbench inspector stays the three things a run needs (model, output,
//! export location). Deliberately terse: labels and controls, no prose.

use nsh_core::prelude::*;

use crate::engine_bridge::{ChromaSubsampling, OutputFormat};
use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::{quality_slider, section_header, segmented_sized, toggle_row, SegmentItem};

pub fn settings_page(store: Store) -> View {
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
                        .w(px(560.0))
                        .flex_col()
                        .gap(px(22.0))
                        .child(
                            text("设置")
                                .text_size(px(18.0))
                                .font_weight(FontWeight::MEDIUM)
                                .text_color(palette::on_surface()),
                        )
                        .child(engine_section(store))
                        .child(encoder_section(store))
                        .child(directory_section(store))
                        .child(about_section()),
                ),
        )
}

fn engine_section(store: Store) -> View {
    view()
        .w_full()
        .flex_col()
        .gap(px(12.0))
        .child(section_header("引擎"))
        .child(row(
            "计算设备",
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
        ))
        .child(row(
            "分块尺寸",
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
        ))
        .child(toggle_row("常驻热引擎", "", move || store.daemon_running.get() || store.daemon_loading.get(), move |v| store.toggle_daemon(v)))
        .child(toggle_row("覆盖已有结果", "", move || store.force.get(), move |v| store.force.set(v)))
}

fn encoder_section(store: Store) -> View {
    view()
        .w_full()
        .flex_col()
        .gap(px(12.0))
        .child(section_header("编码"))
        .child(
            view()
                .w_full()
                .flex_col()
                .gap(px(8.0))
                .child(row(
                    "质量",
                    view().child(
                        text(move || store.quality.get().to_string())
                            .text_size(px(13.0))
                            .font_weight(FontWeight::MEDIUM)
                            .font_family(FONT_MONO)
                            .tabular_nums()
                            .text_color(palette::on_surface()),
                    ),
                ))
                .child(quality_slider(store.quality, 1, 100)),
        )
        .child(when(
            move || store.format.get().supports_chroma(),
            move || {
                row(
                    "色度抽样",
                    view().w(px(164.0)).child(segmented_sized(
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
                        move |v| store.chroma.set(v),
                    )),
                )
            },
        ))
        .child(when(
            move || store.format.get() == OutputFormat::Avif,
            move || {
                row(
                    "编码速度",
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
}

fn directory_section(store: Store) -> View {
    let models_dir = store.models_dir.with(|d| d.display().to_string());
    view()
        .w_full()
        .flex_col()
        .gap(px(10.0))
        .child(section_header("目录"))
        .child(
            view()
                .w_full()
                .flex_col()
                .gap(px(4.0))
                .child(
                    text("模型目录")
                        .text_size(px(13.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(palette::on_surface()),
                )
                .child(
                    text(models_dir)
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .text_color(palette::muted()),
                ),
        )
}

fn about_section() -> View {
    view()
        .w_full()
        .flex_col()
        .gap(px(6.0))
        .child(section_header("关于"))
        .child(
            text(format!("Upix {} · 引擎 ncnn", env!("CARGO_PKG_VERSION")))
                .text_size(px(11.0))
                .font_family(FONT_MONO)
                .text_color(palette::muted()),
        )
}

/// 14px label on the left, control on the right.
fn row(label: impl Into<String>, control: View) -> View {
    view()
        .w_full()
        .h(px(28.0))
        .flex_row()
        .items_center()
        .justify_between()
        .child(
            text(label.into())
                .text_size(px(14.0))
                .font_weight(FontWeight::MEDIUM)
                .text_color(palette::on_surface()),
        )
        .child(control)
}
