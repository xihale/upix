//! 主机: start/stop coordinator, the pairing QR, the approval dialog data,
//! fingerprint and trust list — the desktop HostPage.qml layout (header with
//! status dot + 停止主机, pairing panel with a white QR field, quiet log
//! strip). The dialog itself opens in the shell so it overlays everything.

use nsh_base::theme;
use nsh_core::prelude::*;

use crate::store::Store;
use crate::ui;

pub fn page(store: Store) -> impl IntoElement {
    scroll().flex_1().child(
        view()
            .p_4()
            .gap_4()
            .child(header(store))
            .child(pairing_panel(store))
            .child(log_strip(store)),
    )
}

fn header(store: Store) -> impl IntoElement {
    let live = move || store.host_running.get() && store.host_info.get().is_some();
    view()
        .flex_row()
        .items_center()
        .justify_between()
        .child(ui::page_title("主机"))
        .child(
            view()
                .flex_row()
                .items_center()
                .gap_2()
                .child(ui::status_dot(live))
                .child(
                    text(move || {
                        if let Some(info) = store.host_info.get() {
                            if store.host_running.get() {
                                format!("运行中 · {}:{}", info.ip, info.port)
                            } else {
                                "启动中…".to_owned()
                            }
                        } else if store.host_running.get() {
                            "启动中…".to_owned()
                        } else {
                            "未运行".to_owned()
                        }
                    })
                    .text_xs()
                    .font_family(crate::ui::mono_font())
                    .text_color(move || theme().muted_foreground),
                )
                .child(
                    ui::ghost_button("停止主机", move || store.stop_host())
                        .disabled(move || !store.host_running.get()),
                ),
        )
}

/// QR field | instrument column (port + auto-approve + fingerprint + trust).
fn pairing_panel(store: Store) -> impl IntoElement {
    ui::card_with(move || {
        view()
            .flex_row()
            .gap_4()
            .child(qr_field(store))
            .child(
                view()
                    .flex_1()
                    .gap_3()
                    .child(
                        view()
                            .flex_row()
                            .items_center()
                            .gap_2()
                            .child(text("端口").text_sm())
                            .child(
                                ui::field("9177", store.host_port_draft, move |_| {})
                                .w(px(120.0)),
                            )
                            .child(
                                text("自动批准")
                                    .text_sm()
                                    .text_color(move || theme().muted_foreground),
                            )
                            .child(
                                nsh_base::switch()
                                    .checked(store.host_auto_approve)
                                    .on_change(move |v: &bool| store.host_auto_approve.set(*v))
                                    .track_style(|s| {
                                        s.w(px(40.0))
                                            .h(px(22.0))
                                            .rounded_full()
                                            .bg(move || {
                                                if store.host_auto_approve.get() {
                                                    theme().primary
                                                } else {
                                                    theme().secondary
                                                }
                                            })
                                    })
                                    .thumb_style(|s| {
                                        s.size(px(18.0))
                                            .rounded_full()
                                            .bg(move || theme().background)
                                    }),
                            ),
                    )
                    .child(
                        ui::primary_button(
                            move || {
                                if store.host_running.get() {
                                    SharedString::from("启动中…")
                                } else {
                                    "启动主机".into()
                                }
                            },
                            move || store.start_host(),
                        )
                        .disabled(move || store.host_running.get()),
                    )
                    .child(fingerprint_block(store))
                    .child(trust_block(store)),
            )
    })
}

/// The QR field: white ground + the pairing QR when live; idle otherwise
/// (scanners need a light ground once live — HostPage.qml).
fn qr_field(store: Store) -> impl IntoElement {
    view()
        .size(px(280.0))
        .items_center()
        .justify_center()
        .rounded(px(8.0))
        .border_1()
        .border_color(move || theme().border)
        .bg(move || {
            if store.host_info.get().is_some() {
                // 扫码底色必须为白（QR 极性，CLI 打印二维码时同样的理由）。
                AdaptiveColor::rgb(0xffffff, 0xffffff)
            } else {
                theme().muted
            }
        })
        .child(
            view()
                .size(px(248.0))
                .items_center()
                .justify_center()
                .gap_3()
                .child(when(
                    move || store.host_info.get().is_some(),
                    move || {
                        image(move || {
                            store
                                .host_info
                                .get()
                                .map(|info| info.qr_data_uri)
                                .unwrap_or_default()
                        })
                        .size(px(248.0))
                        .object_fit(ResizeMode::Contain)
                        .into_element()
                    },
                )
                .otherwise(move || {
                    view()
                        .items_center()
                        .justify_center()
                        .gap_2()
                        .child(
                            text(move || {
                                if store.host_running.get() {
                                    "正在启动主机…"
                                } else {
                                    "在手机上扫码配对"
                                }
                            })
                            .text_sm()
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(move || theme().foreground),
                        )
                        .into_element()
                })),
        )
}

fn fingerprint_block(store: Store) -> impl IntoElement {
    view()
        .gap_1()
        .child(ui::section_label("设备指纹"))
        .child(
            text(move || {
                store
                    .host_info
                    .get()
                    .map(|info| info.fingerprint.clone())
                    .unwrap_or_else(|| "—".into())
            })
            .text_sm()
            .font_family(crate::ui::mono_font())
            .tabular_nums(),
        )
        .child(
            text("与手机上显示的指纹核对一致后，再点允许。")
                .text_xs()
                .text_color(move || theme().muted_foreground),
        )
}

fn trust_block(store: Store) -> impl IntoElement {
    view()
        .gap_1()
        .child(ui::section_label("信任列表"))
        .child(when(
            move || store.trusted.get().is_empty(),
            move || {
                text("信任列表为空（~/.upix/trusted_clients）")
                    .text_xs()
                    .text_color(move || theme().muted_foreground)
                    .into_element()
            },
        )
        .otherwise(move || {
            view().gap_1().children(
                store
                    .trusted
                    .get()
                    .into_iter()
                    .map(|key| {
                        text(key).text_xs().font_family(crate::ui::mono_font())
                    })
                    .collect::<Vec<_>>(),
            )
        }))
}

fn log_strip(store: Store) -> impl IntoElement {
    ui::card_with(move || {
        view()
            .gap_1()
            .min_h(px(64.0))
            .children(
                store
                    .host_log
                    .get()
                    .iter()
                    .rev()
                    .take(8)
                    .rev()
                    .map(|line| {
                        text(line.clone())
                            .text_xs()
                            .font_family(crate::ui::mono_font())
                            .text_color(move || theme().muted_foreground)
                    })
                    .collect::<Vec<_>>(),
            )
    })
}
