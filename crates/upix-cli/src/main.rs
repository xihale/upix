//! Upix CLI — thin entry point.
//!
//! The command tree (subcommands, options, validators, help text) is built in
//! cli_def.rs via clap. main() just parses argv and dispatches to the cmd_*
//! entry point that clap selected.

mod args;
mod cli_def;
mod cmd_complete;
mod cmd_completions;
mod cmd_daemon;
mod cmd_host;
mod cmd_model;
mod cmd_process;
mod common;
mod logging;
mod process_batch;

use std::sync::atomic::Ordering;

use common::{G_CANCEL_REQUESTED, G_RUNNING};

/// Injected at build time via `env!("CARGO_PKG_VERSION")`; workspace version
/// is the single source of truth. Optional codecs append their capabilities so
/// `upix --version` says whether AVIF is linked in or dlopen'd at runtime
/// (xmake.lua's on_load built the same string for the C++ CLI).
/// &'static str form for the clap derive `version` attribute (the string is
/// cached once; the capability probes run on first call).
pub fn version_str() -> &'static str {
    static CACHE: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    CACHE.get_or_init(version_string)
}

pub fn version_string() -> String {
    let mut caps = String::new();
    // The Rust build has no static libavif; the codec dlopens the system
    // library at runtime (upix-img). Encoder/decoder presence is a runtime
    // fact — reported the same way the C++ reported the dlopen-only build.
    let _ = upix_img::avif_available();
    caps.push_str(" avif:system-dlopen");
    format!("v{}{}", env!("CARGO_PKG_VERSION"), caps)
}

/// SIGINT/SIGTERM handler: set g_running=false + g_cancel_requested=true so
/// long-running loops (process, host, daemon) unwind gracefully.
extern "C" fn upix_signal_handler(_sig: libc::c_int) {
    G_RUNNING.store(false, Ordering::SeqCst);
    G_CANCEL_REQUESTED.store(true, Ordering::SeqCst);
}

extern "C" fn daemon_signal_handler(sig: libc::c_int) {
    upix_signal_handler(sig);
    // The daemon's run loop does not observe g_running; reach in and request
    // its own shutdown (cmd_daemon::install_daemon_stop slot).
    cmd_daemon::request_daemon_stop();
}

/// Install SIGINT/SIGTERM handlers for the current subcommand.
pub fn install_signal_handlers() {
    // SAFETY: the handlers only touch atomics.
    unsafe {
        libc::signal(libc::SIGINT, upix_signal_handler as *const () as usize);
        libc::signal(libc::SIGTERM, upix_signal_handler as *const () as usize);
    }
}

/// Install daemon-flavored handlers (SIGTERM from `daemon stop` must also
/// stop the accept loop, not just flip the flag).
pub fn install_daemon_signal_handlers(stop: Box<dyn Fn() + Send>) {
    cmd_daemon::install_daemon_stop(stop);
    unsafe {
        libc::signal(libc::SIGINT, daemon_signal_handler as *const () as usize);
        libc::signal(libc::SIGTERM, daemon_signal_handler as *const () as usize);
    }
}

fn main() {
    logging::init();
    G_RUNNING.store(true, Ordering::SeqCst);
    G_CANCEL_REQUESTED.store(false, Ordering::SeqCst);

    let argv: Vec<String> = std::env::args().collect();

    // Hidden dynamic-completion entry (cobra-style): the emitted shell scripts
    // replay the line being completed into this and print its candidates.
    // Handled before clap so arbitrary words are never parsed as flags.
    if argv.len() >= 2 && argv[1] == "__complete" {
        let words: Vec<String> = argv[2..].to_vec();
        std::process::exit(cmd_complete::run(&words));
    }

    // The ensure_running() spawner re-invokes this binary with an internal
    // daemon mode that bypasses the CLI parser entirely.
    if let Some(internal) = cmd_daemon::get_internal_daemon_args(&argv) {
        logging::init();
        let code = cmd_daemon::run_internal_daemon(&internal);
        std::process::exit(code);
    }

    // A subcommand (or the root upscale callback) returns the exit code.
    let code = cli_def::dispatch(argv[1..].to_vec());
    std::process::exit(code);
}
