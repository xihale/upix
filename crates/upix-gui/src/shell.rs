//! The shell: a left rail on wide windows (desktop gallery layout) and a tab
//! bar on narrow ones, the 100 ms worker-drain tick, and the approval
//! dialog that overlays everything.

use std::time::Duration;

use nsh_base::{alert_dialog, theme};
use nsh_core::prelude::*;

use crate::host_bridge::ApprovalReply;
use crate::nav::Page;
use crate::pages;
use crate::store::Store;

pub fn shell(store: Store) -> impl IntoElement {
    // The worker drain: events move to signals here, on the engine thread.
    set_interval(Duration::from_millis(100), move || store.drain());

    // The pairing approval: one native dialog per request; the answer goes
    // back through the host handle. [y/n] stdin of the C++ coordinator.
    effect(move || {
        let Some(request) = store.host_pending.get() else {
            return;
        };
        spawn(async move {
            let answer = alert_dialog()
                .title("配对请求")
                .description(format!(
                    "设备 {}\n指纹 {}\n与手机上显示的指纹核对一致后，再点允许。",
                    request.device, request.fingerprint
                ))
                .cancel("deny", "拒绝 (Esc)")
                .action("allow", "允许")
                .show()
                .await;
            let reply = match answer {
                Ok(result) if result.action.as_deref() == Some("allow") => ApprovalReply::Allow,
                _ => ApprovalReply::Deny,
            };
            store.answer_approval(reply);
        });
    });

    view()
        .flex_1()
        .bg(move || theme().background)
        // Remade when the window crosses the threshold (as the gallery's
        // shell does): the store carries everything across.
        .child(
            when(
                move || window().width.0 >= 700.0,
                move || rail_layout(store).into_element(),
            )
            .otherwise(move || tab_layout(store).into_element()),
        )
}

fn rail_layout(store: Store) -> impl IntoElement {
    view()
        .flex_1()
        .flex_row()
        .child(
            view()
                .w(px(160.0))
                .p_3()
                .gap_1()
                .border_r_1()
                .border_color(move || theme().border)
                .bg(move || theme().sidebar)
                .child(
                    text("Upix")
                        .text_base()
                        .font_weight(FontWeight::SEMIBOLD)
                        .p_2()
                        .text_color(move || theme().sidebar_foreground),
                )
                .children(Page::ALL.map(|page| rail_item(store, page))),
        )
        .child(view().flex_1().child(current_page(store)))
}

fn tab_layout(store: Store) -> impl IntoElement {
    view()
        .flex_1()
        .p_safe_area()
        .gap_0()
        .child(
            view()
                .flex_row()
                .px_2()
                .h(px(44.0))
                .items_center()
                .gap_1()
                .border_b_1()
                .border_color(move || theme().border)
                .children(Page::ALL.map(|page| tab_item(store, page))),
        )
        .child(view().flex_1().child(current_page(store)))
}

fn rail_item(store: Store, page: Page) -> impl IntoElement {
    let selected = move || store.page.get() == page;
    view()
        .px_3()
        .py_2()
        .rounded(move || theme().radius)
        .hover(|s| s.bg(move || theme().accent))
        .style_when(selected, |s| s.bg(move || theme().primary))
        .cursor_pointer()
        .on_press(move || store.page.set(page))
        .role(AccessibilityRole::Button)
        .label(page.title())
        .child(text(page.title()).text_sm().text_color(move || {
            if selected() {
                theme().primary_foreground
            } else {
                theme().sidebar_foreground
            }
        }))
}

fn tab_item(store: Store, page: Page) -> impl IntoElement {
    let selected = move || store.page.get() == page;
    view()
        .flex_1()
        .h_full()
        .items_center()
        .justify_center()
        .cursor_pointer()
        .on_press(move || store.page.set(page))
        .role(AccessibilityRole::Button)
        .label(page.title())
        .child(text(page.title()).text_sm().text_color(move || {
            if selected() {
                theme().primary
            } else {
                theme().muted_foreground
            }
        }))
        // The selected tab's underline.
        .style_when(selected, |s| s.border_b_1().border_color(move || theme().primary))
}

fn current_page(store: Store) -> impl IntoElement {
    dynamic(move || match store.page.get() {
        Page::Home => pages::home::page(store).into_element(),
        Page::Progress => pages::progress::page(store).into_element(),
        Page::Compare => pages::compare::page(store).into_element(),
        Page::Host => pages::host::page(store).into_element(),
        Page::Models => pages::models::page(store).into_element(),
    })
}
