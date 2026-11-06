//! 进度: stage (读取图片 / 分块推理 / 编码输出), tile counts, %, ETA and the
//! 取消 button. All numbers arrive through the store's drain tick — this page
//! only reads signals and never touches the worker thread.

use nsh_base::theme;
use nsh_core::prelude::*;

use crate::store::{JobState, Store};
use crate::ui;

pub fn page(store: Store) -> impl IntoElement {
    scroll().flex_1().child(
        view()
            .p_4()
            .gap_4()
            .child(ui::page_title("进度"))
            .child(body(store)),
    )
}

fn body(store: Store) -> impl IntoElement {
    ui::card_with(move || match store.job.get() {
        JobState::Running { stage, done, total, started_ms } => {
            running_body(store, stage, done, total, started_ms).into_element()
        }
        JobState::Cancelled => view()
            .gap_3()
            .child(
                text("已取消")
                    .text_sm()
                    .text_color(move || theme().muted_foreground),
            )
            .child(back_row(store))
            .into_element(),
        JobState::Failed(message) => view()
            .gap_3()
            .child(
                text(move || format!("失败：{message}"))
                    .text_sm()
                    .text_color(move || theme().destructive),
            )
            .child(back_row(store))
            .into_element(),
        JobState::Idle => {
            // Done jobs jump straight to 对比; Idle here means the user
            // navigated back without a run.
            view()
                .gap_3()
                .child(
                    text(move || {
                        if store.result.get().is_some() {
                            "已完成，去「对比」查看结果"
                        } else {
                            "还没有任务。先在「首页」选图并开始超分。"
                        }
                    })
                    .text_sm()
                    .text_color(move || theme().muted_foreground),
                )
                .child(back_row(store))
                .into_element()
        }
    })
}

fn running_body(
    store: Store,
    stage: crate::engine_bridge::Stage,
    done: i32,
    total: i32,
    started_ms: f64,
) -> impl IntoElement {
    let pct = if total > 0 {
        (done as f32 / total as f32 * 100.0).clamp(0.0, 100.0)
    } else {
        0.0
    };
    view()
        .gap_3()
        .child(
            view()
                .flex_row()
                .items_center()
                .justify_between()
                .child(ui::chip(stage.label(), crate::theme::primary_ink()))
                .child(
                    text(move || format!("{pct:.0}%"))
                        .text_sm()
                        .tabular_nums()
                        .text_color(move || theme().foreground),
                ),
        )
        .child(ui::progress_bar(pct))
        .child(
            view()
                .flex_row()
                .items_center()
                .justify_between()
                .child(
                    text(move || {
                        if total > 0 {
                            format!("tile {done}/{total}")
                        } else {
                            "准备中…".to_owned()
                        }
                    })
                    .text_xs()
                    .tabular_nums()
                    .text_color(move || theme().muted_foreground),
                )
                .child(
                    text(move || {
                        let elapsed = (nsh_core::time::unix_ms() - started_ms) / 1000.0;
                        if done > 0 && elapsed > 0.0 {
                            let eta = elapsed * (total - done) as f64 / done as f64;
                            format!("剩余约 {}", eta_label(eta))
                        } else {
                            "估计剩余时间…".to_owned()
                        }
                    })
                    .text_xs()
                    .tabular_nums()
                    .text_color(move || theme().muted_foreground),
                ),
        )
        .child(
            view()
                .flex_row()
                .justify_end()
                .child(ui::ghost_button("取消", move || store.cancel_job())),
        )
}

fn back_row(store: Store) -> impl IntoElement {
    view().flex_row().justify_end().child(
        ui::ghost_button("返回首页", move || store.page.set(crate::nav::Page::Home)),
    )
}

/// "45 秒" / "3 分 12 秒".
fn eta_label(secs: f64) -> String {
    let secs = secs.round() as u64;
    if secs >= 60 {
        format!("{} 分 {} 秒", secs / 60, secs % 60)
    } else {
        format!("{secs} 秒")
    }
}
