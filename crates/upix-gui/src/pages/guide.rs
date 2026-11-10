//! 指南 page — three steps and one model hint. Everything the Android help
//! screen says at length (families, degradations, weights) is what this page
//! deliberately leaves out: the model cards already carry scene badges, and a
//! wall of prose is what "最简" is meant to avoid.

use nsh_core::prelude::*;

use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::section_header;

pub fn guide_page(store: Store) -> View {
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
                            text("指南")
                                .text_size(px(18.0))
                                .font_weight(FontWeight::MEDIUM)
                                .text_color(palette::on_surface()),
                        )
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(10.0))
                                .child(step("1", "放入图片或文件夹（也可拖入，或 Ctrl+V 粘贴)"))
                                .child(step("2", "选模型 —— 动漫 Real-ESRGANv3-anime-x2，照片 NomosUni-Compact-x2，去块/去噪 De…-x1"))
                                .child(step("3", "点开始超分；结果在「导出位置」，默认存在原图旁边")),
                        )
                        .child(divider())
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(8.0))
                                .child(section_header("快捷键"))
                                .child(keys(&[
                                    ("Ctrl+1 / 2 / 3", "放大 / 模型 / 远程"),
                                    ("Ctrl+V", "粘贴图片"),
                                    ("Ctrl+.", "取消超分"),
                                ])),
                        )
                        .child(divider())
                        .child(
                            view()
                                .w_full()
                                .flex_col()
                                .gap(px(8.0))
                                .child(section_header("还没有模型？"))
                                .child(
                                    view()
                                        .w_full()
                                        .h(px(28.0))
                                        .rounded(px(6.0))
                                        .bg(palette::high())
                                        .border_1()
                                        .border_color(palette::border())
                                        .items_center()
                                        .justify_center()
                                        .cursor_pointer()
                                        .hover(|s| s.bg(palette::highest()))
                                        .on_press(move || store.nav.set(crate::nav::NavTab::Models))
                                        .child(
                                            text("打开模型页")
                                                .text_size(px(11.0))
                                                .font_weight(FontWeight::MEDIUM)
                                                .text_color(palette::text()),
                                        ),
                                ),
                        ),
                ),
        )
}

fn step(index: &'static str, body: &'static str) -> View {
    view()
        .w_full()
        .flex_row()
        .items_center()
        .gap(px(10.0))
        .child(
            view()
                .size(px(20.0))
                .rounded_full()
                .bg(palette::container())
                .border_1()
                .border_color(palette::chip_border())
                .items_center()
                .justify_center()
                .child(
                    text(index)
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .text_color(palette::primary()),
                ),
        )
        .child(
            text(body)
                .text_size(px(13.0))
                .text_color(palette::on_surface()),
        )
}

fn keys(rows: &[(&'static str, &'static str)]) -> View {
    let mut col = view().w_full().flex_col().gap(px(4.0));
    for (combo, what) in rows {
        col = col.child(
            view()
                .w_full()
                .flex_row()
                .items_center()
                .justify_between()
                .child(
                    text(*combo)
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .text_color(palette::on_surface()),
                )
                .child(
                    text(*what)
                        .text_size(px(11.0))
                        .text_color(palette::muted()),
                ),
        );
    }
    col
}

fn divider() -> View {
    view().w_full().h(px(1.0)).bg(palette::hairline())
}
