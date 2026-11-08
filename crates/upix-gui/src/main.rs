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

    // Keep GTK4 / GSK / Vulkan / glibc memory footprint lean on Linux desktops:
    // - Use GSK's Cairo renderer by default so GTK4 does not pull in ~105 MB of
    //   NVIDIA/Mesa/LLVM GPU shader compiler libraries for a 2D UI window.
    // - Disable 3rd-party Vulkan layers and prefer iGPU ICD on hybrid laptops.
    // - Lower glibc's malloc trim threshold so temporary image/font buffers return to OS.
    #[cfg(target_os = "linux")]
    {
        if std::env::var_os("GSK_RENDERER").is_none() {
            // SAFETY: called at the very start of `main`, before any threads are spawned.
            unsafe { std::env::set_var("GSK_RENDERER", "cairo") };
        }
        if std::env::var_os("VK_LOADER_LAYERS_DISABLE").is_none() {
            unsafe { std::env::set_var("VK_LOADER_LAYERS_DISABLE", "~all~") };
        }
        if std::env::var_os("VK_LOADER_DRIVERS_SELECT").is_none()
            && (std::path::Path::new("/usr/share/vulkan/icd.d/intel_icd.x86_64.json").exists()
                || std::path::Path::new("/usr/share/vulkan/icd.d/intel_icd.json").exists())
        {
            unsafe { std::env::set_var("VK_LOADER_DRIVERS_SELECT", "intel*") };
        }
        if std::env::var_os("MALLOC_TRIM_THRESHOLD_").is_none() {
            unsafe { std::env::set_var("MALLOC_TRIM_THRESHOLD_", "65536") };
        }
        std::thread::spawn(|| {
            std::thread::sleep(std::time::Duration::from_millis(900));
            upix_gui::evict_file_backed_font_pages();
        });
    }

    let _ = env_logger::try_init();
    platform::run_app(
        platform::AppSource::Rust(platform::Root::of::<upix_gui::App>()),
        platform::AppOptions {
            title: "Upix".to_owned(),
            #[cfg(any(target_os = "linux", target_os = "windows"))]
            app_id: "ink.xeed.upix".to_owned(),
            size: (1280, 840),
            appearance: nsh_core::prelude::Appearance::Dark,
            ..Default::default()
        },
    )
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
compile_error!(
    "Run this binary on macOS, Windows or Linux; use the nsh android target for devices."
);
