//! Top-level application shell: 76px left navigation rail (`desktop/src/main.cpp`),
//! active page viewport, and global pairing approval modal (`83:3762`).

use std::path::PathBuf;
use std::time::Duration;

use nsh_core::element::ImageSrc;
use nsh_core::prelude::*;
use nsh_core::{LayoutCurve, LayoutTransition};

use crate::host_bridge::ApprovalReply;
use crate::nav::NavTab;
use crate::pages::host::{host_page, split_fp_groups};
use crate::pages::models::models_page;
use crate::pages::workbench::workbench_page;
use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::svg_icon;

pub fn shell(store: Store) -> View {
    // Adaptive background channel loop: only ticks at 80ms when a background job,
    // daemon warmup, model download, or remote host server is active; sleeps when idle
    // so the UI process uses 0% CPU and does not churn memory while waiting for user input.
    spawn(async move {
        loop {
            store.drain();
            let delay = if store.has_background_activity() {
                Duration::from_millis(80)
            } else {
                Duration::from_millis(800)
            };
            timer(delay).await;
        }
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
        } else if store.show_ip_picker.get() {
            store.show_ip_picker.set(false);
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
                .child(dynamic(move || {
                    let page = match store.nav.get() {
                        NavTab::Workbench => workbench_page(store).into_element(),
                        NavTab::Models => models_page(store).into_element(),
                        NavTab::Host => host_page(store).into_element(),
                    };
                    #[cfg(target_os = "linux")]
                    std::thread::spawn(|| {
                        std::thread::sleep(std::time::Duration::from_millis(250));
                        crate::evict_file_backed_font_pages();
                    });
                    page
                })),
        )
        .child(when(
            move || store.host_pending_approval.get().is_some(),
            move || pairing_modal_overlay(store),
        ))
}

fn nav_rail(store: Store) -> View {
    const NAV_SPRING: LayoutTransition =
        LayoutTransition::new(LayoutCurve::Spring { damping: 0.85 }, 270.0);

    let refs: [(NavTab, ElementRef); 3] = NavTab::ALL.map(|tab| (tab, element_ref()));

    // Spring-animated background pill that follows the selected nav tab natively
    let active_pill = view()
        .absolute()
        .left(px(8.0))
        .right(px(8.0))
        .rounded(px(10.0))
        .bg(palette::high())
        .pointer_events(PointerEvents::None)
        .follow(move || {
            let cur = store.nav.get();
            refs.iter()
                .find(|(t, _)| *t == cur)
                .map(|(_, r)| *r)
        })
        .follow_transition(move || (!reduce_motion()).then_some(NAV_SPRING));

    let mut items_col = view()
        .w_full()
        .relative()
        .flex_col()
        .items_center()
        .gap(px(2.0))
        .child(active_pill);

    for (tab, el_ref) in refs {
        items_col = items_col.child(
            view()
                .element_ref(&el_ref)
                .size(px(60.0))
                .rounded(px(10.0))
                .flex_col()
                .items_center()
                .justify_center()
                .gap(px(3.0))
                .cursor_pointer()
                .transition()
                .duration(140.0)
                .ease_out()
                .hover(move |s| {
                    s.bg(AdaptiveColor::new(rgba(0x0000000A), rgba(0xFFFFFF08)))
                })
                .active(|s| if reduce_motion() { s } else { s.scale(0.95) })
                .on_press(move || store.nav.set(tab))
                .child(
                    svg(ImageSrc::svg_markup(tab.icon_svg()))
                        .size(px(20.0))
                        .transition_colors()
                        .duration(150.0)
                        .ease_out()
                        .tint_color(move || {
                            if store.nav.get() == tab {
                                palette::primary()
                            } else {
                                palette::muted()
                            }
                        }),
                )
                .child(
                    text(tab.label())
                        .text_size(px(11.0))
                        .font_weight(FontWeight::MEDIUM)
                        .transition_colors()
                        .duration(150.0)
                        .ease_out()
                        .text_color(move || {
                            if store.nav.get() == tab {
                                palette::on_surface()
                            } else {
                                palette::muted()
                            }
                        }),
                ),
        );
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
    let Some(info) = store.host_pending_approval.get() else {
        return view();
    };
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
        .bg(palette::overlay_scrim().opacity(0.72))
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
                                .transition()
                                .duration(140.0)
                                .ease_out()
                                .hover(|s| s.bg(palette::high()))
                                .active(|s| if reduce_motion() { s } else { s.scale(0.97) })
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
                                .transition()
                                .duration(140.0)
                                .ease_out()
                                .hover(|s| s.bg(palette::primary_hover()))
                                .active(|s| {
                                    let s = s.bg(palette::accent_active());
                                    if reduce_motion() { s } else { s.scale(0.97) }
                                })
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
