//! Runs the Rust application directly on the machine's native backend.

#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

#[cfg(target_os = "linux")]
use nsh_platform_linux as platform;
#[cfg(target_os = "macos")]
use nsh_platform_macos as platform;
#[cfg(target_os = "windows")]
use nsh_platform_windows as platform;

#[cfg(any(target_os = "linux", target_os = "macos", target_os = "windows"))]
fn main() -> impl std::process::Termination {
    // Internal warm-engine daemon entry (spawned by upix_host::serve::ensure_running).
    #[cfg(feature = "integration")]
    {
        let argv: Vec<String> = std::env::args().collect();
        if argv.get(1).map(|s| s.as_str()) == Some("--upix-internal-daemon") {
            let _ = env_logger::try_init();
            std::process::exit(
                upix_gui::engine_bridge::run_internal_daemon_from_argv(&argv) as i32,
            );
        }
    }

    let _ = env_logger::try_init();
    platform::run_app(
        platform::AppSource::Rust(platform::Root::of::<upix_gui::App>()),
        platform::AppOptions {
            title: "Upix".to_owned(),
            #[cfg(any(target_os = "linux", target_os = "windows"))]
            app_id: "ink.xeed.upix".to_owned(),
            size: (1280, 840),
            ..Default::default()
        },
    )
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
compile_error!(
    "Run this binary on macOS, Windows or Linux; use the nsh android target for devices."
);
