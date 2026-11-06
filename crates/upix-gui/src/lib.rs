//! upix-gui — NSH (Native Shell) app, one Rust UI for Android + Linux/macOS/Windows.
//! Porting contract: docs/rewrite/CONTRACT.md §8. Pages and copy align the
//! Compose app (`apps/`) and the Qt desktop (`desktop/`); engine calls run on
//! worker threads and reach the UI through channels drained on the engine
//! thread, never blocking it.

pub mod engine_bridge;
pub mod host_bridge;
pub mod models_local;
mod nav;
pub mod pages;
pub mod qr;
mod shell;
pub mod store;
mod theme;
pub mod ui;

use nsh_base::theme;
use nsh_core::prelude::*;

use crate::store::Store;

/// The app's UI set (`nsh.json` `"rust"`): what a TSX entry may import from
/// `upix-gui/native`.
#[native_module("upix-gui/native")]
fn native(registry: &mut Registry) {
    registry.register::<App>();
}

/// The application root: one store, the theme applied from `UPIX_THEME`, and
/// the shell (rail on desktops, tab bar on phones).
#[component]
pub fn App() -> impl IntoElement {
    let store = Store::new();

    // `UPIX_THEME=light|dark` wins over the system appearance; anything else
    // (unset, other values) keeps following the system. A task, not the body:
    // the body runs once but must stay free of side effects.
    spawn(async move {
        match std::env::var("UPIX_THEME").as_deref() {
            Ok("light") => set_color_scheme(Appearance::Light),
            Ok("dark") => set_color_scheme(Appearance::Dark),
            Ok(other) => {
                log::debug!("UPIX_THEME={other:?} unrecognized: following the system");
            }
            Err(_) => log::debug!("UPIX_THEME unset: following the system"),
        }
        log::info!("upix-gui started");
    });

    view()
        .flex_1()
        .bg(move || theme().background)
        .text_color(move || theme().foreground)
        .child(shell::shell(store))
}
