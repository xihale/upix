//! Remote Host page (`desktop/src/main.cpp` `renderRemotePage`, Figma Frames `4:137` & `73:2535`):
//! Vertically & horizontally centered 712px container with:
//! 1. Pairing panel (`712x288`, `240x240` QR well with registration marks + dormant QR matrix / live QR,
//!    and `396x240` right column with 3 `44px` rows: `指纹`, `地址`, `端口`, plus bottom `LIVE`/`IDLE` pill and `启动服务`/`停止服务` button)
//! 2. Trusted devices card (`已信任设备`, `712px`, shown when `!trusted.is_empty()`)
//! 3. Telemetry console card (`712x122`, shown when `running || !logs.is_empty()`).

use nsh_core::element::input;
use nsh_core::prelude::*;

use crate::host_bridge::fingerprint_of_b64;
use crate::models_local::scale_from_filename;
use crate::store::Store;
use crate::theme::{palette, FONT_MONO};
use crate::ui::svg_icon;

pub fn host_page(store: Store) -> View {
    view()
        .flex_1()
        .h_full()
        .bg(palette::bg())
        .flex_col()
        .items_center()
        .justify_center()
        .child(
            view()
                .w(px(712.0))
                .flex_col()
                .gap(px(14.0))
                .child(pairing_card(store))
                .child(dynamic(move || trusted_devices_card(store)))
                .child(dynamic(move || telemetry_console_card(store))),
        )
}

fn qr_reg_marks(running: bool) -> View {
    let c = if running {
        palette::primary_dim_border()
    } else {
        AdaptiveColor::rgb(0xBEBDB4, 0x383844)
    };
    view()
        .absolute()
        .inset(px(14.0))
        .pointer_events(PointerEvents::None)
        // Top-left
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(14.0))
                .h(px(1.5))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .w(px(1.5))
                .h(px(14.0))
                .bg(c),
        )
        // Top-right
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(14.0))
                .h(px(1.5))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .w(px(1.5))
                .h(px(14.0))
                .bg(c),
        )
        // Bottom-left
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(14.0))
                .h(px(1.5))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .left(px(0.0))
                .w(px(1.5))
                .h(px(14.0))
                .bg(c),
        )
        // Bottom-right
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(14.0))
                .h(px(1.5))
                .bg(c),
        )
        .child(
            view()
                .absolute()
                .bottom(px(0.0))
                .right(px(0.0))
                .w(px(1.5))
                .h(px(14.0))
                .bg(c),
        )
}

fn dormant_finder(top: Option<f32>, bottom: Option<f32>, left: Option<f32>, right: Option<f32>) -> View {
    let col = palette::outline_strong();
    let mut v = view()
        .absolute()
        .size(px(32.0))
        .rounded(px(4.0))
        .border(px(2.5))
        .border_color(col)
        .items_center()
        .justify_center()
        .child(view().size(px(14.0)).rounded(px(2.0)).bg(col));
    if let Some(t) = top {
        v = v.top(px(t));
    }
    if let Some(b) = bottom {
        v = v.bottom(px(b));
    }
    if let Some(l) = left {
        v = v.left(px(l));
    }
    if let Some(r) = right {
        v = v.right(px(r));
    }
    v
}

fn qr_well(store: Store) -> View {
    let running = store.host_running.get();
    let qr_uri = store.host_qr_uri.get();

    if running && !qr_uri.is_empty() {
        view()
            .size(px(240.0))
            .flex_shrink_0()
            .rounded(px(10.0))
            .bg(palette::recessed())
            .border_1()
            .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x26262E))
            .relative()
            .items_center()
            .justify_center()
            .child(qr_reg_marks(true))
            .child(
                view()
                    .size(px(192.0))
                    .rounded(px(8.0))
                    .bg(palette::qr_paper())
                    .p(px(8.0))
                    .items_center()
                    .justify_center()
                    .child(
                        image(qr_uri)
                            .size(px(176.0))
                            .object_fit(ResizeMode::Contain),
                    ),
            )
    } else {
        // Dormant QR Matrix matching Figma 66:965 (3 finder squares at 48px inset + subtle dots + center 48x48 QR badge)
        let dot_col = palette::outline_strong();
        let mut matrix = view()
            .size(px(240.0))
            .flex_shrink_0()
            .rounded(px(10.0))
            .bg(palette::recessed())
            .border_1()
            .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x26262E))
            .relative()
            .items_center()
            .justify_center()
            .child(qr_reg_marks(false))
            .child(dormant_finder(Some(48.0), None, Some(48.0), None))
            .child(dormant_finder(Some(48.0), None, None, Some(48.0)))
            .child(dormant_finder(None, Some(48.0), Some(48.0), None));

        // Subtle module dots in non-finder, non-center cells
        for gy in 0..7 {
            for gx in 0..7 {
                if (gx < 2 && gy < 2)
                    || (gx > 4 && gy < 2)
                    || (gx < 2 && gy > 4)
                    || ((2..=4).contains(&gx) && (2..=4).contains(&gy))
                {
                    continue;
                }
                if (gx * 3 + gy * 5) % 2 == 0 {
                    matrix = matrix.child(
                        view()
                            .absolute()
                            .left(px(60.0 + (gx as f32) * 19.0))
                            .top(px(60.0 + (gy as f32) * 19.0))
                            .size(px(4.5))
                            .rounded(px(1.5))
                            .bg(dot_col),
                    );
                }
            }
        }

        // Center 48x48 badge (#1c1c22, border #2e2e38, radius 12px, 20x20 QR icon #8e8e9a)
        matrix.child(
            view()
                .size(px(48.0))
                .rounded(px(12.0))
                .bg(AdaptiveColor::rgb(0xF0EFEB, 0x1C1C22))
                .border_1()
                .border_color(AdaptiveColor::rgb(0xCECDC5, 0x2E2E38))
                .items_center()
                .justify_center()
                .child(svg_icon(
                    crate::ui::icons::QR_CODE,
                    20.0,
                    AdaptiveColor::rgb(0x706F68, 0x8E8E9A),
                )),
        )
    }
}

fn pairing_card(store: Store) -> View {
    view()
        .w(px(712.0))
        .h(px(288.0))
        .rounded(px(12.0))
        .bg(palette::container_low())
        .border_1()
        .border_color(palette::outline_input())
        .p(px(24.0))
        .flex_row()
        .items_center()
        .gap(px(28.0))
        // Left 240x240 QR Well
        .child(dynamic(move || qr_well(store)))
        // Right 396x240 Connection Info Column
        .child(
            view()
                .w(px(396.0))
                .h(px(240.0))
                .flex_shrink_0()
                .flex_col()
                .justify_between()
                // Top Details Group (3 rows of 44px + 2 dividers of 1px = 134px)
                .child(
                    view()
                        .w(px(396.0))
                        .flex_col()
                        // Row 1: 指纹 (44px high, gap 24px)
                        .child(
                            view()
                                .w_full()
                                .h(px(44.0))
                                .flex_row()
                                .items_center()
                                .gap(px(24.0))
                                .child(
                                    text("指纹")
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(AdaptiveColor::rgb(0x706F68, 0x7E7E8A)),
                                )
                                .child(fingerprint_pills_row(store)),
                        )
                        .child(
                            view()
                                .w_full()
                                .h(px(1.0))
                                .bg(AdaptiveColor::rgb(0xD6D5CD, 0x26262E)),
                        )
                        // Row 2: 地址 (44px high, gap 24px)
                        .child(
                            view()
                                .w_full()
                                .h(px(44.0))
                                .flex_row()
                                .items_center()
                                .gap(px(24.0))
                                .child(
                                    text("地址")
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(AdaptiveColor::rgb(0x706F68, 0x7E7E8A)),
                                )
                                .child(ip_selector_box(store)),
                        )
                        .child(
                            view()
                                .w_full()
                                .h(px(1.0))
                                .bg(AdaptiveColor::rgb(0xD6D5CD, 0x26262E)),
                        )
                        // Row 3: 端口 (44px high, gap 24px)
                        .child(
                            view()
                                .w_full()
                                .h(px(44.0))
                                .flex_row()
                                .items_center()
                                .gap(px(24.0))
                                .child(
                                    text("端口")
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(AdaptiveColor::rgb(0x706F68, 0x7E7E8A)),
                                )
                                .child(port_input_box(store)),
                        ),
                )
                // Bottom Service Action Bar (396x36)
                .child(host_bottom_row(store)),
        )
}

fn fingerprint_pills_row(store: Store) -> AnyElement {
    dynamic(move || {
        let fp = store.host_fingerprint.get();
        let groups = split_fp_groups(&fp);
        let mut row = view().flex_row().items_center().gap(px(6.0));
        for g in groups {
            row = row.child(
                view()
                    .w(px(44.0))
                    .h(px(24.0))
                    .rounded(px(4.0))
                    .bg(AdaptiveColor::rgb(0xEAE9E4, 0x16161B))
                    .border_1()
                    .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x26262E))
                    .items_center()
                    .justify_center()
                    .child(
                        text(g)
                            .text_size(px(11.5))
                            .font_weight(FontWeight::MEDIUM)
                            .font_family(FONT_MONO)
                            .tabular_nums()
                            .text_color(AdaptiveColor::rgb(0x27272A, 0xD8D8E0)),
                    ),
            );
        }
        row = row.child(
            view()
                .size(px(24.0))
                .rounded(px(4.0))
                .hover(|s| s.bg(palette::high()))
                .items_center()
                .justify_center()
                .cursor_pointer()
                .on_press(move || {
                    set_clipboard(store.host_fingerprint.get());
                })
                .child(svg_icon(
                    crate::ui::icons::COPY,
                    13.0,
                    AdaptiveColor::rgb(0x706F68, 0x9A9AA6),
                )),
        );
        row
    })
}

fn ip_selector_box(store: Store) -> View {
    view()
        .w(px(216.0))
        .h(px(30.0))
        .rounded(px(6.0))
        .bg(AdaptiveColor::rgb(0xEAE9E4, 0x16161B))
        .border_1()
        .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x2A2A32))
        .hover(|s| s.bg(AdaptiveColor::rgb(0xE3E2DC, 0x22222A)))
        .pl(px(10.0))
        .pr(px(9.0))
        .flex_row()
        .items_center()
        .justify_between()
        .cursor_pointer()
        .on_press(move || store.cycle_host_ip())
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(view().size(px(5.0)).rounded_full().bg(palette::primary()))
                .child(
                    text(move || {
                        let ips = store.host_ips.get();
                        let idx = store.host_ip_idx.get();
                        ips.get(idx)
                            .cloned()
                            .unwrap_or_else(|| "192.168.0.126".to_string())
                    })
                    .text_size(px(12.0))
                    .font_weight(FontWeight::MEDIUM)
                    .font_family(FONT_MONO)
                    .tabular_nums()
                    .text_color(palette::on_surface()),
                ),
        )
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .child(
                    view()
                        .w(px(28.0))
                        .h(px(16.0))
                        .rounded(px(4.0))
                        .bg(AdaptiveColor::rgb(0xDDDCDA, 0x23232B))
                        .items_center()
                        .justify_center()
                        .child(
                            text("LAN")
                                .text_size(px(9.5))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .text_color(AdaptiveColor::rgb(0x706F68, 0x8E8E9A)),
                        ),
                )
                .child(svg_icon(
                    crate::ui::icons::CHEVRON_DOWN,
                    10.0,
                    AdaptiveColor::rgb(0x706F68, 0x7E7E8A),
                )),
        )
}

fn port_input_box(store: Store) -> View {
    view()
        .w(px(84.0))
        .h(px(30.0))
        .rounded(px(6.0))
        .bg(AdaptiveColor::rgb(0xEAE9E4, 0x16161B))
        .border_1()
        .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x2A2A32))
        .px(px(10.0))
        .flex_row()
        .items_center()
        .gap(px(4.0))
        .child(
            text(":")
                .text_size(px(12.0))
                .font_family(FONT_MONO)
                .text_color(palette::dim()),
        )
        .child(
            input()
                .flex_1()
                .value(move || store.host_port.get().to_string())
                .on_change(move |v: &str| {
                    if let Ok(p) = v.trim().parse::<u16>() {
                        if p > 0 {
                            store.host_port.set(p);
                        }
                    }
                })
                .text_size(px(12.0))
                .font_family(FONT_MONO)
                .text_color(palette::on_surface())
                .bg(AdaptiveColor::TRANSPARENT)
                .border(px(0.0)),
        )
}

fn host_bottom_row(store: Store) -> AnyElement {
    dynamic(move || {
        let running = store.host_running.get();

        view()
            .w(px(396.0))
            .h(px(36.0))
            .flex_row()
            .items_center()
            .justify_between()
            // Left: 64x24 Status pill (radius 12px: ● LIVE or ● IDLE)
            .child(
                view()
                    .w(px(64.0))
                    .h(px(24.0))
                    .rounded(px(12.0))
                    .bg(if running {
                        AdaptiveColor::rgb(0xDDF2EE, 0x162624)
                    } else {
                        AdaptiveColor::rgb(0xEAE9E4, 0x17171C)
                    })
                    .border_1()
                    .border_color(if running {
                        AdaptiveColor::rgb(0x9FD5CA, 0x24423E)
                    } else {
                        AdaptiveColor::rgb(0xD6D5CD, 0x25252D)
                    })
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .gap(px(6.0))
                    .child(
                        view()
                            .size(px(6.0))
                            .rounded_full()
                            .bg(if running {
                                palette::primary()
                            } else {
                                AdaptiveColor::rgb(0x8A8982, 0x5A5A66)
                            }),
                    )
                    .child(
                        text(if running { "LIVE" } else { "IDLE" })
                            .text_size(px(10.5))
                            .font_weight(FontWeight::MEDIUM)
                            .font_family(FONT_MONO)
                            .text_color(if running {
                                palette::primary()
                            } else {
                                palette::dim()
                            }),
                    ),
            )
            // Right: Start (103x36) / Stop (98x36) button
            .child(if running {
                view()
                    .w(px(98.0))
                    .h(px(36.0))
                    .rounded(px(6.0))
                    .bg(AdaptiveColor::rgb(0xE3E2DC, 0x26262E))
                    .border_1()
                    .border_color(AdaptiveColor::rgb(0xC5C4BC, 0x383844))
                    .hover(|s| s.bg(palette::highest()))
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .gap(px(7.0))
                    .cursor_pointer()
                    .on_press(move || store.stop_host())
                    .child(
                        view()
                            .size(px(8.0))
                            .rounded(px(2.0))
                            .bg(palette::on_surface()),
                    )
                    .child(
                        text("停止服务")
                            .text_size(px(13.0))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(palette::on_surface()),
                    )
            } else {
                view()
                    .w(px(103.0))
                    .h(px(36.0))
                    .rounded(px(6.0))
                    .bg(palette::primary())
                    .hover(|s| s.bg(palette::primary_hover()))
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .gap(px(7.0))
                    .cursor_pointer()
                    .on_press(move || store.start_host())
                    .child(svg_icon(
                        crate::ui::icons::PLAY_OUTLINE,
                        15.0,
                        palette::on_primary(),
                    ))
                    .child(
                        text("启动服务")
                            .text_size(px(13.0))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(palette::on_primary()),
                    )
            })
    })
}

fn format_dotted_fp(fp: &str) -> String {
    let groups = split_fp_groups(fp);
    groups.join(" · ")
}

fn trusted_devices_card(store: Store) -> View {
    let peers = store.host_trusted.get();
    if peers.is_empty() {
        return view().h(px(0.0));
    }

    let mut card = view()
        .w(px(712.0))
        .rounded(px(10.0))
        .bg(palette::surface())
        .border_1()
        .border_color(palette::outline())
        .overflow_hidden()
        .flex_col()
        // 34px header (#202026, left "已信任设备" 12.5px #e2e2e8, right count 11px mono #7e7e8a)
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
                    text("已信任设备")
                        .text_size(px(12.5))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(AdaptiveColor::rgb(0x18181B, 0xE2E2E8)),
                )
                .child(
                    text(peers.len().to_string())
                        .text_size(px(11.0))
                        .font_family(FONT_MONO)
                        .tabular_nums()
                        .text_color(AdaptiveColor::rgb(0x706F68, 0x7E7E8A)),
                ),
        );

    for (idx, pubkey_b64) in peers.into_iter().enumerate() {
        let raw_fp = fingerprint_of_b64(&pubkey_b64);
        let dotted_fp = format_dotted_fp(&raw_fp);
        let online = idx == 0 && store.host_running.get();

        card = card
            .child(
                view()
                    .w_full()
                    .h(px(1.0))
                    .bg(AdaptiveColor::rgb(0xDDDCDA, 0x22222A)),
            )
            .child(
                view()
                    .w_full()
                    .h(px(38.0))
                    .pl(px(16.0))
                    .pr(px(14.0))
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
                                    .bg(if online {
                                        palette::primary()
                                    } else {
                                        AdaptiveColor::rgb(0x8A8982, 0x4A4A56)
                                    }),
                            )
                            .child(
                                text(format!("Android 设备 #{}", idx + 1))
                                    .text_size(px(12.5))
                                    .font_weight(FontWeight::MEDIUM)
                                    .font_family(FONT_MONO)
                                    .text_color(if online {
                                        palette::on_surface()
                                    } else {
                                        AdaptiveColor::rgb(0x706F68, 0x9E9EA8)
                                    }),
                            )
                            .child(
                                view()
                                    .w(px(40.0))
                                    .h(px(19.0))
                                    .rounded(px(4.0))
                                    .bg(if online {
                                        palette::primary_dim_bg()
                                    } else {
                                        palette::container()
                                    })
                                    .border_1()
                                    .border_color(if online {
                                        AdaptiveColor::rgb(0x9FD5CA, 0x264A43)
                                    } else {
                                        AdaptiveColor::rgb(0xCECDC5, 0x2C2C35)
                                    })
                                    .items_center()
                                    .justify_center()
                                    .child(
                                        text(if online { "在线" } else { "已存" })
                                            .text_size(px(11.0))
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(if online {
                                                palette::primary()
                                            } else {
                                                AdaptiveColor::rgb(0x706F68, 0x7E7E8A)
                                            }),
                                    ),
                            ),
                    )
                    .child(
                        view()
                            .flex_row()
                            .items_center()
                            .gap(px(16.0))
                            .child(
                                text(dotted_fp)
                                    .text_size(px(11.5))
                                    .font_family(FONT_MONO)
                                    .tabular_nums()
                                    .text_color(palette::dim()),
                            )
                            .child(
                                view()
                                    .size(px(26.0))
                                    .rounded(px(6.0))
                                    .hover(|s| s.bg(palette::error_dim_bg()))
                                    .items_center()
                                    .justify_center()
                                    .cursor_pointer()
                                    .on_press(move || store.remove_trusted(idx))
                                    .child(svg_icon(
                                        crate::ui::icons::TRASH,
                                        13.5,
                                        AdaptiveColor::rgb(0x8A8982, 0x5E5E6A),
                                    )),
                            ),
                    ),
            );
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
    let scale = scale_from_filename(&active_model).unwrap_or(4);
    let model_badge_text = format!("{active_model} · {scale}×");

    let mut log_col = view()
        .w_full()
        .h(px(84.0))
        .px(px(16.0))
        .py(px(8.0))
        .flex_col()
        .gap(px(4.0));

    if logs.is_empty() {
        log_col = log_col.child(log_row("HOST", "等待移动端协同连接…", ""));
    } else {
        for line in logs.iter().rev().take(3).rev() {
            log_col = log_col.child(log_row("HOST", line, ""));
        }
    }

    view()
        .w(px(712.0))
        .h(px(122.0))
        .rounded(px(10.0))
        .bg(palette::recessed())
        .border_1()
        .border_color(palette::outline())
        .overflow_hidden()
        .flex_col()
        // 38px header (#1d1d23)
        .child(
            view()
                .w_full()
                .h(px(38.0))
                .bg(AdaptiveColor::rgb(0xECEBE6, 0x1D1D23))
                .px(px(16.0))
                .flex_row()
                .items_center()
                .justify_between()
                .child(
                    view()
                        .flex_row()
                        .items_center()
                        .gap(px(8.0))
                        .child(view().size(px(6.0)).rounded_full().bg(palette::primary()))
                        .child(
                            text(if running { "协同服务就绪" } else { "服务日志" })
                                .text_size(px(12.0))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .text_color(palette::on_surface()),
                        )
                        .child(
                            view()
                                .h(px(18.0))
                                .px(px(6.0))
                                .rounded(px(4.0))
                                .bg(AdaptiveColor::rgb(0xEAE9E4, 0x16161B))
                                .border_1()
                                .border_color(AdaptiveColor::rgb(0xD6D5CD, 0x2A2A34))
                                .items_center()
                                .justify_center()
                                .child(
                                    text(model_badge_text)
                                        .text_size(px(10.0))
                                        .font_family(FONT_MONO)
                                        .text_color(AdaptiveColor::rgb(0x706F68, 0x8E8E9A)),
                                ),
                        ),
                )
                .child(
                    view()
                        .flex_row()
                        .items_center()
                        .gap(px(10.0))
                        .child(
                            view()
                                .w(px(136.0))
                                .h(px(4.0))
                                .rounded(px(2.0))
                                .bg(AdaptiveColor::rgb(0xD6D5CD, 0x121216))
                                .overflow_hidden()
                                .child(
                                    view()
                                        .h_full()
                                        .w(percentage(if running { 1.0 } else { 0.0 }))
                                        .rounded(px(2.0))
                                        .bg(palette::primary()),
                                ),
                        )
                        .child(
                            text(if running { "LIVE" } else { "IDLE" })
                                .text_size(px(11.0))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .text_color(palette::primary()),
                        ),
                ),
        )
        .child(log_col)
}

fn log_row(tag: &str, msg: &str, right_note: &str) -> View {
    let is_task = tag == "TASK";
    view()
        .w_full()
        .h(px(20.0))
        .flex_row()
        .items_center()
        .justify_between()
        .child(
            view()
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(
                    view()
                        .h(px(16.0))
                        .px(px(5.0))
                        .rounded(px(4.0))
                        .bg(if is_task {
                            palette::primary_dim_bg()
                        } else {
                            AdaptiveColor::rgb(0xE8E7E1, 0x1D1E24)
                        })
                        .border_1()
                        .border_color(if is_task {
                            palette::primary_dim_border()
                        } else {
                            AdaptiveColor::rgb(0xCECDC5, 0x2D2E38)
                        })
                        .items_center()
                        .justify_center()
                        .child(
                            text(tag.to_string())
                                .text_size(px(10.0))
                                .font_weight(FontWeight::MEDIUM)
                                .font_family(FONT_MONO)
                                .text_color(if is_task {
                                    palette::primary()
                                } else {
                                    AdaptiveColor::rgb(0x706F68, 0x8E8E9A)
                                }),
                        ),
                )
                .child(
                    text(msg.to_string())
                        .text_size(px(11.0))
                        .text_color(if is_task {
                            AdaptiveColor::rgb(0x27272A, 0xC8C8D2)
                        } else {
                            AdaptiveColor::rgb(0x706F68, 0x8E8E9A)
                        }),
                ),
        )
        .child(if !right_note.is_empty() {
            text(right_note.to_string())
                .text_size(px(11.0))
                .font_family(FONT_MONO)
                .text_color(if is_task {
                    palette::primary()
                } else {
                    AdaptiveColor::rgb(0x8A8982, 0x565662)
                })
                .into_element()
        } else {
            view().into_element()
        })
}

pub fn split_fp_groups(fp: &str) -> Vec<String> {
    let raw: String = fp
        .chars()
        .filter(|c| *c != '-' && *c != ' ' && *c != ':' && *c != '·')
        .collect();
    if raw.len() >= 16 {
        vec![
            raw[0..4].to_string(),
            raw[4..8].to_string(),
            raw[8..12].to_string(),
            raw[12..16].to_string(),
        ]
    } else {
        vec![
            "EC61".to_string(),
            "F2B6".to_string(),
            "6D4C".to_string(),
            "25C4".to_string(),
        ]
    }
}
