//! Remote Host page (`desktop/src/main.cpp` `render_host_tab`):
//! Centered 712px container with:
//! 1. Pairing card (240x240 QR well with registration marks + 4-group fingerprint pills,
//!    LAN IP selector, port input, auto-approve toggle, and LIVE/IDLE status + start/stop button)
//! 2. Trusted devices card (`~/.upix/trusted_clients`)
//! 3. Telemetry console card (live log stream).

use nsh_core::element::input;
use nsh_core::prelude::*;

use crate::host_bridge::fingerprint_of_b64;
use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::{svg_icon, toggle_row};

pub fn host_page(store: Store) -> View {
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
                        .w(px(712.0))
                        .flex_col()
                        .gap(px(16.0))
                        .child(pairing_card(store))
                        .child(dynamic(move || trusted_devices_card(store)))
                        .child(dynamic(move || telemetry_console_card(store))),
                ),
        )
}

fn qr_reg_marks() -> View {
    let c = palette::reg_mark();
    view()
        .absolute()
        .inset(px(10.0))
        .pointer_events(PointerEvents::None)
        // Top-left
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(10.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(1.0))
                .h(px(10.0))
                .bg(c),
        )
        // Top-right
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(10.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(1.0))
                .h(px(10.0))
                .bg(c),
        )
        // Bottom-left
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(10.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(1.0))
                .h(px(10.0))
                .bg(c),
        )
        // Bottom-right
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(10.0))
                .h(px(1.0))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(1.0))
                .h(px(10.0))
                .bg(c),
        )
}

fn pairing_card(store: Store) -> View {
    view()
        .w_full()
        .h(px(304.0))
        .rounded(px(12.0))
        .bg(palette::surface())
        .border_1()
        .border_color(palette::outline())
        .p(px(24.0))
        .flex_row()
        .items_center()
        .gap(px(28.0))
        // Left 240x240 QR Well
        .child(dynamic(move || qr_well(store)))
        // Right Instrument Column
        .child(
            view()
                .flex_1()
                .h_full()
                .flex_col()
                .justify_between()
                // Top instrument rows
                .child(
                    view()
                        .w_full()
                        .flex_col()
                        .gap(px(12.0))
                        // Row 1: 指纹
                        .child(
                            view()
                                .w_full()
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    text("指纹")
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::muted()),
                                )
                                .child(fingerprint_pills_row(store)),
                        )
                        .child(view().w_full().h(px(1.0)).bg(palette::outline()))
                        // Row 2: 地址
                        .child(
                            view()
                                .w_full()
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    text("地址")
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::muted()),
                                )
                                .child(ip_selector_box(store)),
                        )
                        // Row 3: 端口
                        .child(
                            view()
                                .w_full()
                                .flex_row()
                                .items_center()
                                .justify_between()
                                .child(
                                    text("端口")
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::muted()),
                                )
                                .child(port_input_box(store)),
                        )
                        // Row 4: 自动批准新设备
                        .child(dynamic(move || {
                            toggle_row(
                                "免确认自动配对",
                                "",
                                store.host_auto_approve.get(),
                                move |v| store.host_auto_approve.set(v),
                            )
                        })),
                )
                // Bottom status + start/stop button row
                .child(host_bottom_row(store)),
        )
}

fn qr_well(store: Store) -> View {
    let running = store.host_running.get();
    let qr_uri = store.host_qr_uri.get();

    if running && !qr_uri.is_empty() {
        view()
            .size(px(240.0))
            .rounded(px(10.0))
            .bg(palette::recessed())
            .border_1()
            .border_color(palette::outline())
            .relative()
            .items_center()
            .justify_center()
            .child(qr_reg_marks())
            .child(
                view()
                    .size(px(188.0))
                    .rounded(px(6.0))
                    .bg(palette::qr_paper())
                    .p(px(6.0))
                    .items_center()
                    .justify_center()
                    .child(
                        image(qr_uri)
                            .size(px(176.0))
                            .object_fit(ResizeMode::Contain),
                    ),
            )
    } else {
        // Offline QR well with finder pattern silhouettes + center badge
        view()
            .size(px(240.0))
            .rounded(px(10.0))
            .bg(palette::recessed())
            .border_1()
            .border_color(palette::outline())
            .relative()
            .items_center()
            .justify_center()
            .child(qr_reg_marks())
            // 3 muted finder-pattern squares
            .child(
                view()
                    .absolute()
                    .top(px(36.0))
                    .left(px(36.0))
                    .size(px(28.0))
                    .rounded(px(4.0))
                    .border(px(2.0))
                    .border_color(palette::outline()),
            )
            .child(
                view()
                    .absolute()
                    .top(px(36.0))
                    .right(px(36.0))
                    .size(px(28.0))
                    .rounded(px(4.0))
                    .border(px(2.0))
                    .border_color(palette::outline()),
            )
            .child(
                view()
                    .absolute()
                    .bottom(px(36.0))
                    .left(px(36.0))
                    .size(px(28.0))
                    .rounded(px(4.0))
                    .border(px(2.0))
                    .border_color(palette::outline()),
            )
            .child(
                view()
                    .flex_col()
                    .items_center()
                    .gap(px(10.0))
                    .child(
                        view()
                            .size(px(36.0))
                            .rounded(px(8.0))
                            .bg(palette::container_low())
                            .border_1()
                            .border_color(palette::outline())
                            .items_center()
                            .justify_center()
                            .child(svg_icon(crate::ui::icons::HOST, 16.0, palette::dim())),
                    )
                    .child(
                        text("启动后生成配对码")
                            .text_size(px(11.0))
                            .text_color(palette::dim()),
                    ),
            )
    }
}

fn fingerprint_pills_row(store: Store) -> AnyElement {
    dynamic(move || {
        let fp = store.host_fingerprint.get();
        let groups = split_fp_groups(&fp);
        let mut row = view().flex_row().items_center().gap(px(6.0));
        for g in groups {
            row = row.child(
                view()
                    .w(px(50.0))
                    .h(px(28.0))
                    .rounded(px(6.0))
                    .bg(palette::container_low())
                    .border_1()
                    .border_color(palette::outline())
                    .items_center()
                    .justify_center()
                    .child(
                        text(g)
                            .text_size(px(12.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .font_family(FONT_MONO)
                            .tabular_nums()
                            .text_color(palette::on_surface()),
                    ),
            );
        }
        row = row.child(
            view()
                .h(px(28.0))
                .px(px(8.0))
                .rounded(px(6.0))
                .bg(palette::container_low())
                .border_1()
                .border_color(palette::outline())
                .hover(|s| s.bg(palette::high()))
                .items_center()
                .justify_center()
                .cursor_pointer()
                .on_press(move || {
                    set_clipboard(store.host_fingerprint.get());
                })
                .child(
                    text("复制")
                        .text_size(px(10.0))
                        .text_color(palette::muted()),
                ),
        );
        row
    })
}

fn ip_selector_box(store: Store) -> View {
    view()
        .w(px(252.0))
        .h(px(32.0))
        .rounded(px(6.0))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline_input())
        .hover(|s| s.bg(palette::high()))
        .px(px(10.0))
        .flex_row()
        .items_center()
        .justify_between()
        .cursor_pointer()
        .on_press(move || store.cycle_host_ip())
        .child(
            text(move || {
                let ips = store.host_ips.get();
                let idx = store.host_ip_idx.get();
                ips.get(idx)
                    .cloned()
                    .unwrap_or_else(|| "127.0.0.1".to_string())
            })
            .text_size(px(12.0))
            .font_family(FONT_MONO)
            .tabular_nums()
            .text_color(palette::on_surface()),
        )
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .child(
                    view()
                        .h(px(18.0))
                        .px(px(6.0))
                        .rounded(px(4.0))
                        .bg(palette::recessed())
                        .border_1()
                        .border_color(palette::outline())
                        .items_center()
                        .justify_center()
                        .child(
                            text("LAN")
                                .text_size(px(9.0))
                                .font_family(FONT_MONO)
                                .text_color(palette::dim()),
                        ),
                )
                .child(text("▾").text_size(px(11.0)).text_color(palette::dim())),
        )
}

fn port_input_box(store: Store) -> View {
    view()
        .w(px(252.0))
        .h(px(32.0))
        .rounded(px(6.0))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline_input())
        .px(px(10.0))
        .flex_row()
        .items_center()
        .child(
            input()
                .w_full()
                .value(move || store.host_port.get().to_string())
                .on_change(move |v: &str| {
                    if let Ok(p) = v.trim().parse::<u16>() {
                        if p > 0 {
                            store.host_port.set(p);
                        }
                    }
                })
                .text_size(px(12.0))
                .text_color(palette::on_surface())
                .bg(AdaptiveColor::TRANSPARENT)
                .border(px(0.0)),
        )
}

fn host_bottom_row(store: Store) -> AnyElement {
    dynamic(move || {
        let running = store.host_running.get();

        view()
            .w_full()
            .pt(px(12.0))
            .border_t_1()
            .border_color(palette::outline())
            .flex_row()
            .items_center()
            .justify_between()
            // Left: Status pill + label
            .child(
                view()
                    .flex_row()
                    .items_center()
                    .gap(px(10.0))
                    .child(
                        view()
                            .h(px(22.0))
                            .px(px(8.0))
                            .rounded(px(5.0))
                            .bg(if running {
                                palette::primary_dim_bg()
                            } else {
                                palette::container_low()
                            })
                            .border_1()
                            .border_color(if running {
                                palette::primary_dim_border()
                            } else {
                                palette::outline()
                            })
                            .flex_row()
                            .items_center()
                            .gap(px(6.0))
                            .child(
                                view()
                                    .size(px(6.0))
                                    .rounded_full()
                                    .bg(if running {
                                        palette::primary()
                                    } else {
                                        palette::dim()
                                    }),
                            )
                            .child(
                                text(if running { "LIVE" } else { "IDLE" })
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::BOLD)
                                    .font_family(FONT_MONO)
                                    .text_color(if running {
                                        palette::primary()
                                    } else {
                                        palette::dim()
                                    }),
                            ),
                    )
                    .child(
                        text(if running {
                            "X25519 + ChaCha20-Poly1305 · 正在监听"
                        } else {
                            "未广播 · 端口未监听"
                        })
                        .text_size(px(11.0))
                        .text_color(palette::muted()),
                    ),
            )
            // Right: Start / Stop button (124x34)
            .child(if running {
                view()
                    .w(px(124.0))
                    .h(px(34.0))
                    .rounded(px(7.0))
                    .bg(palette::container_low())
                    .border_1()
                    .border_color(palette::outline_input())
                    .hover(|s| s.bg(palette::high()))
                    .items_center()
                    .justify_center()
                    .cursor_pointer()
                    .on_press(move || store.stop_host())
                    .child(
                        text("停止服务")
                            .text_size(px(12.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(palette::on_surface()),
                    )
            } else {
                view()
                    .w(px(124.0))
                    .h(px(34.0))
                    .rounded(px(7.0))
                    .bg(palette::primary())
                    .hover(|s| s.bg(palette::primary_hover()))
                    .items_center()
                    .justify_center()
                    .cursor_pointer()
                    .on_press(move || store.start_host())
                    .child(
                        text("启动服务")
                            .text_size(px(12.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(palette::on_primary()),
                    )
            })
    })
}

fn trusted_devices_card(store: Store) -> View {
    let peers = store.host_trusted.get();
    if peers.is_empty() && !store.host_running.get() {
        return view().h(px(0.0));
    }

    let mut card = view()
        .w_full()
        .rounded(px(12.0))
        .bg(palette::surface())
        .border_1()
        .border_color(palette::outline())
        .overflow_hidden()
        .flex_col()
        // 34px header
        .child(
            view()
                .w_full()
                .h(px(34.0))
                .bg(palette::container_low())
                .px(px(16.0))
                .flex_row()
                .items_center()
                .justify_between()
                .child(
                    text("已信任设备")
                        .text_size(px(11.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(palette::muted()),
                )
                .child(
                    view()
                        .flex_row()
                        .items_center()
                        .gap(px(10.0))
                        .child(
                            text(peers.len().to_string())
                                .text_size(px(11.0))
                                .font_family(FONT_MONO)
                                .tabular_nums()
                                .text_color(palette::dim()),
                        )
                        .child(if !peers.is_empty() {
                            view()
                                .px(px(6.0))
                                .py(px(2.0))
                                .rounded(px(4.0))
                                .cursor_pointer()
                                .hover(|s| s.bg(palette::error_dim_bg()))
                                .on_press(move || store.clear_trusted())
                                .child(
                                    text("清空")
                                        .text_size(px(10.0))
                                        .text_color(palette::error()),
                                )
                        } else {
                            view()
                        }),
                ),
        );

    if peers.is_empty() {
        card = card.child(
            view()
                .w_full()
                .h(px(44.0))
                .px(px(16.0))
                .items_center()
                .child(
                    text("暂无已配对手机")
                        .text_size(px(11.0))
                        .text_color(palette::dim()),
                ),
        );
    } else {
        for (idx, pubkey_b64) in peers.into_iter().enumerate() {
            let fp = fingerprint_of_b64(&pubkey_b64);
            card = card
                .child(view().w_full().h(px(1.0)).bg(palette::outline()))
                .child(
                    view()
                        .w_full()
                        .h(px(38.0))
                        .px(px(16.0))
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(10.0))
                                .child(
                                    view()
                                        .size(px(6.0))
                                        .rounded_full()
                                        .bg(palette::primary()),
                                )
                                .child(
                                    text(format!("Android 客户端 #{}", idx + 1))
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(palette::on_surface()),
                                )
                                .child(
                                    view()
                                        .h(px(18.0))
                                        .px(px(6.0))
                                        .rounded(px(4.0))
                                        .bg(palette::container_low())
                                        .border_1()
                                        .border_color(palette::outline())
                                        .items_center()
                                        .justify_center()
                                        .child(
                                            text("已信任")
                                                .text_size(px(10.0))
                                                .text_color(palette::dim()),
                                        ),
                                ),
                        )
                        .child(
                            view()
                                .flex_row()
                                .items_center()
                                .gap(px(10.0))
                                .child(
                                    text(fp)
                                        .text_size(px(11.0))
                                        .font_family(FONT_MONO)
                                        .tabular_nums()
                                        .text_color(palette::dim()),
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
                                        .on_press(move || store.remove_trusted(idx))
                                        .child(svg_icon(
                                            crate::ui::icons::CLOSE,
                                            11.0,
                                            palette::muted(),
                                        )),
                                ),
                        ),
                );
        }
    }

    card
}

fn telemetry_console_card(store: Store) -> View {
    let logs = store.host_logs.get();
    let running = store.host_running.get();
    if !running && logs.is_empty() {
        return view().h(px(0.0));
    }

    let active_model = store.selected_model.get();

    let mut log_col = view().w_full().p(px(12.0)).flex_col().gap(px(6.0));
    if logs.is_empty() {
        log_col = log_col.child(
            text("等待手机端连接请求…")
                .text_size(px(11.0))
                .text_color(palette::dim()),
        );
    } else {
        for line in logs.iter().rev().take(8) {
            log_col = log_col.child(
                view()
                    .w_full()
                    .flex_row()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        view()
                            .h(px(16.0))
                            .px(px(5.0))
                            .rounded(px(3.0))
                            .bg(palette::primary_dim_bg())
                            .border_1()
                            .border_color(palette::primary_dim_border())
                            .items_center()
                            .justify_center()
                            .child(
                                text("HOST")
                                    .text_size(px(9.0))
                                    .font_family(FONT_MONO)
                                    .text_color(palette::primary()),
                            ),
                    )
                    .child(
                        text(line.clone())
                            .text_size(px(11.0))
                            .font_family(FONT_MONO)
                            .text_color(palette::on_surface()),
                    ),
            );
        }
    }

    view()
        .w_full()
        .rounded(px(12.0))
        .bg(palette::surface())
        .border_1()
        .border_color(palette::outline())
        .overflow_hidden()
        .flex_col()
        // 38px header
        .child(
            view()
                .w_full()
                .h(px(38.0))
                .bg(palette::container_low())
                .px(px(16.0))
                .flex_row()
                .items_center()
                .justify_between()
                .child(
                    view()
                        .flex_row()
                        .items_center()
                        .gap(px(10.0))
                        .child(view().size(px(6.0)).rounded_full().bg(palette::primary()))
                        .child(
                            text(if running { "服务运行中" } else { "历史日志" })
                                .text_size(px(12.0))
                                .font_weight(FontWeight::MEDIUM)
                                .text_color(palette::on_surface()),
                        )
                        .child(
                            view()
                                .h(px(18.0))
                                .px(px(6.0))
                                .rounded(px(4.0))
                                .bg(palette::recessed())
                                .border_1()
                                .border_color(palette::outline())
                                .items_center()
                                .justify_center()
                                .child(
                                    text(active_model)
                                        .text_size(px(10.0))
                                        .font_family(FONT_MONO)
                                        .text_color(palette::muted()),
                                ),
                        ),
                )
                .child(
                    text("实时日志")
                        .text_size(px(11.0))
                        .text_color(palette::dim()),
                ),
        )
        .child(view().w_full().h(px(1.0)).bg(palette::outline()))
        .child(log_col)
}

pub fn split_fp_groups(fp: &str) -> Vec<String> {
    let raw: String = fp.chars().filter(|c| *c != '-').collect();
    if raw.len() >= 16 {
        vec![
            raw[0..4].to_string(),
            raw[4..8].to_string(),
            raw[8..12].to_string(),
            raw[12..16].to_string(),
        ]
    } else {
        fp.split('-').map(|s| s.to_string()).collect()
    }
}
