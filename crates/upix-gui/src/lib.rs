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

use nsh_core::prelude::*;

use crate::store::Store;

#[cfg(target_os = "linux")]
pub fn evict_file_backed_font_pages() {
    const MADV_DONTNEED: i32 = 4;
    unsafe extern "C" {
        fn madvise(addr: *mut std::ffi::c_void, len: usize, advice: i32) -> i32;
        fn malloc_trim(pad: usize) -> i32;
    }

    let Ok(maps) = std::fs::read_to_string("/proc/self/maps") else {
        return;
    };
    for line in maps.lines() {
        let is_evictable_ro = line.ends_with(".ttc")
            || line.ends_with(".ttf")
            || line.ends_with(".otf")
            || line.contains("/fontconfig/")
            || line.ends_with("/locale-archive")
            || line.ends_with("/icon-theme.cache");
        if !is_evictable_ro {
            continue;
        }
        let mut parts = line.split_whitespace();
        let Some(range) = parts.next() else { continue };
        let Some(perms) = parts.next() else { continue };
        if !perms.starts_with("r--") {
            continue;
        }
        let Some((start_hex, end_hex)) = range.split_once('-') else {
            continue;
        };
        let (Ok(start), Ok(end)) = (
            usize::from_str_radix(start_hex, 16),
            usize::from_str_radix(end_hex, 16),
        ) else {
            continue;
        };
        if end > start {
            unsafe {
                let _ = madvise(start as *mut std::ffi::c_void, end - start, MADV_DONTNEED);
            }
        }
    }
    unsafe {
        let _ = malloc_trim(0);
    }
}

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
                log::debug!("UPIX_THEME={other:?} unrecognized: defaulting to dark");
                set_color_scheme(Appearance::Dark);
            }
            Err(_) => set_color_scheme(Appearance::Dark),
        }
        log::info!("upix-gui started");
    });

    view()
        .flex_1()
        .bg(crate::theme::palette::bg())
        .text_color(crate::theme::palette::text())
        .font_family(crate::theme::FONT_SANS)
        .child(shell::shell(store))
}
