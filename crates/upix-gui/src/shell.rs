//! Top-level application shell: 76px left navigation rail (`desktop/src/main.cpp`),
//! active page viewport, and global pairing approval modal (`83:3762`).

use std::path::PathBuf;
use std::time::Duration;

use nsh_core::prelude::*;

use crate::host_bridge::{ApprovalReply, ApprovalRequest};
use crate::nav::NavTab;
use crate::pages::host::{host_page, split_fp_groups};
use crate::pages::models::models_page;
use crate::pages::workbench::workbench_page;
use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::svg_icon;

pub fn shell(store: Store) -> View {
    // 50ms interval to drain background job, catalog, and host channels
    set_interval(Duration::from_millis(50), move || {
        store.drain();
    });

    // Global keyboard shortcuts matching `desktop/src/main.cpp`
    on_key("mod+1", move |_| store.nav.set(NavTab::Workbench));
    on_key("mod+2", move |_| store.nav.set(NavTab::Models));
    on_key("mod+3", move |_| store.nav.set(NavTab::Host));
    on_key("mod+v", move |_| {
        if store.nav.get() == NavTab::Workbench {
            spawn(async move {
                let text = clipboard_string().await;
                let trimmed = text.trim();
                if !trimmed.is_empty() {
                    let p = PathBuf::from(trimmed);
                    if p.is_file() {
                        store.add_paths(&[p]);
                    }
                }
            });
        }
    });
    on_key("mod+.", move |_| store.cancel_job());
    on_key("escape", move |_| {
        if store.host_pending_approval.get().is_some() {
            store.answer_approval(ApprovalReply::Deny);
        } else if store.show_model_picker.get() {
            store.show_model_picker.set(false);
        }
    });

    view()
        .size_full()
        .flex_row()
        .bg(palette::bg())
        .relative()
        .child(nav_rail(store))
        .child(
            view()
                .flex_1()
                .h_full()
                .overflow_hidden()
                .child(dynamic(move || match store.nav.get() {
                    NavTab::Workbench => workbench_page(store).into_element(),
                    NavTab::Models => models_page(store).into_element(),
                    NavTab::Host => host_page(store).into_element(),
                })),
        )
        .child(when(
            move || store.host_pending_approval.get().is_some(),
            move || pairing_modal_overlay(store),
        ))
}

fn nav_rail(store: Store) -> View {
    let mut items_col = view().w_full().flex_col().items_center().gap(px(2.0));

    for tab in NavTab::ALL {
        items_col = items_col.child(dynamic(move || {
            let selected = store.nav.get() == tab;
            view()
                .size(px(60.0))
                .rounded(px(10.0))
                .flex_col()
                .items_center()
                .justify_center()
                .gap(px(3.0))
                .cursor_pointer()
                .bg(if selected {
                    palette::high()
                } else {
                    AdaptiveColor::TRANSPARENT
                })
                .hover(move |s| {
                    s.bg(if selected {
                        palette::high()
                    } else {
                        palette::container_low()
                    })
                })
                .on_press(move || store.nav.set(tab))
                .child(svg_icon(
                    tab.icon_svg(),
                    20.0,
                    if selected {
                        palette::primary()
                    } else {
                        palette::muted()
                    },
                ))
                .child(
                    text(tab.label())
                        .text_size(px(11.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(if selected {
                            palette::on_surface()
                        } else {
                            palette::muted()
                        }),
                )
        }));
    }

    view()
        .w(px(76.0))
        .h_full()
        .pt(px(12.0))
        .pb(px(16.0))
        .flex_col()
        .items_center()
        .justify_between()
        .bg(palette::bg())
        .child(items_col)
        .child(
            text("0.2.0")
                .text_size(px(10.0))
                .font_weight(FontWeight::MEDIUM)
                .tabular_nums()
                .text_color(palette::muted()),
        )
}

fn pairing_modal_overlay(store: Store) -> View {
    let info = store
        .host_pending_approval
        .get()
        .unwrap_or(ApprovalRequest {
            device: "Pixel 9 Pro".to_string(),
            fingerprint: "EC61F2B66D4C25C4".to_string(),
        });
    let groups = split_fp_groups(&info.fingerprint);

    let mut pills_row = view()
        .flex_row()
        .items_center()
        .justify_center()
        .gap(px(8.0));
    for g in groups {
        pills_row = pills_row.child(
            view()
                .w(px(60.0))
                .h(px(32.0))
                .rounded(px(6.0))
                .bg(palette::surface())
                .border_1()
                .border_color(palette::outline_strong())
                .items_center()
                .justify_center()
                .child(
                    text(g)
                        .text_size(px(13.0))
                        .font_weight(FontWeight::MEDIUM)
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(palette::on_surface()),
                ),
        );
    }

    view()
        .absolute()
        .inset(px(0.0))
        .bg(AdaptiveColor::rgb(0x08080B, 0x08080B).opacity(0.72))
        .items_center()
        .justify_center()
        .child(
            view()
                .w(px(380.0))
                .h(px(240.0))
                .rounded(px(14.0))
                .bg(palette::surface())
                .border_1()
                .border_color(palette::outline_strong())
                .shadow_xl()
                .pt(px(24.0))
                .pb(px(18.0))
                .px(px(22.0))
                .flex_col()
                .justify_between()
                // Header row (centered: 28x28 smartphone badge + device name 15px mono + PAIR pill)
                .child(
                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .justify_center()
                        .gap(px(10.0))
                        .child(
                            view()
                                .size(px(28.0))
                                .rounded(px(7.0))
                                .bg(palette::primary_dim_bg())
                                .border_1()
                                .border_color(palette::primary_dim_border())
                                .items_center()
                                .justify_center()
                                .child(svg_icon(
                                    crate::ui::icons::SMARTPHONE,
                                    15.0,
                                    palette::primary(),
                                )),
                        )
                        .child(
                            text(info.device)
                                .text_size(px(15.0))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .text_color(palette::on_surface()),
                        )
                        .child(
                            view()
                                .w(px(38.0))
                                .h(px(20.0))
                                .rounded(px(4.0))
                                .bg(palette::high())
                                .border_1()
                                .border_color(palette::outline_strong())
                                .items_center()
                                .justify_center()
                                .child(
                                    text("PAIR")
                                        .text_size(px(10.5))
                                        .font_weight(FontWeight::MEDIUM)
                                        .font_family(FONT_MONO)
                                        .text_color(palette::muted()),
                                ),
                        ),
                )
                // Fingerprint verification well (336x84, #151519, border #282830, radius 10px)
                .child(
                    view()
                        .w_full()
                        .h(px(84.0))
                        .rounded(px(10.0))
                        .bg(palette::recessed())
                        .border_1()
                        .border_color(palette::outline())
                        .flex_col()
                        .items_center()
                        .justify_center()
                        .gap(px(8.0))
                        .child(
                            text("核对设备指纹一致")
                                .text_size(px(11.5))
                                .text_color(palette::muted()),
                        )
                        .child(pills_row),
                )
                // Action buttons row (116x38 "拒绝 Esc" + 10px gap + 210x38 "允许连接")
                .child(
                    view()
                        .w_full()
                        .flex_row()
                        .items_center()
                        .gap(px(10.0))
                        .child(
                            view()
                                .w(px(116.0))
                                .h(px(38.0))
                                .rounded(px(8.0))
                                .bg(palette::container())
                                .border_1()
                                .border_color(palette::outline_strong())
                                .hover(|s| s.bg(palette::high()))
                                .flex_row()
                                .items_center()
                                .justify_center()
                                .gap(px(6.0))
                                .cursor_pointer()
                                .on_press(move || store.answer_approval(ApprovalReply::Deny))
                                .child(
                                    text("拒绝")
                                        .text_size(px(13.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_surface()),
                                )
                                .child(
                                    text("Esc")
                                        .text_size(px(11.5))
                                        .font_family(FONT_MONO)
                                        .text_color(palette::dim()),
                                ),
                        )
                        .child(
                            view()
                                .w(px(210.0))
                                .h(px(38.0))
                                .rounded(px(8.0))
                                .bg(palette::primary())
                                .hover(|s| s.bg(palette::primary_hover()))
                                .items_center()
                                .justify_center()
                                .cursor_pointer()
                                .on_press(move || store.answer_approval(ApprovalReply::Allow))
                                .child(
                                    text("允许连接")
                                        .text_size(px(13.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_primary()),
                                ),
                        ),
                ),
        )
}
