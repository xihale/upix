//! CLI logging — format-compatible port of the C++ `upix::Logger`
//! (`crates/upix-engine-ffi/csrc/upix/utils/logger.hpp`).
//!
//! Line format, exactly:
//! `[YYYY-MM-DD HH:MM:SS.mmm] [Tag] [LEVEL] message`
//! with the level spelled DEBUG / INFO / WARN / ERROR, written to **stdout**
//! by default and to **stderr** in machine-output mode (the `--json` /
//! `UPIX_PROGRESS=json` event stream owns stdout).
//!
//! Env knobs mirror the C++:
//!   * `UPIX_LOG_LEVEL=debug` enables DEBUG (default: INFO);
//!   * `NO_COLOR` (non-empty) disables color, `FORCE_COLOR` (non-empty) and
//!     `CLICOLOR_FORCE` (non-empty and not "0") force it; otherwise color is
//!     on only when stdout is a TTY.

use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};

/// stdout carries machine events; logs must not corrupt the stream.
static TO_STDERR: AtomicBool = AtomicBool::new(false);

pub fn machine_output_enabled() -> bool {
    TO_STDERR.load(Ordering::Relaxed)
}

/// Switch the log sink to stderr (idempotent). Call before the first log line
/// so stdout stays a clean event stream.
pub fn enable_machine_output() {
    TO_STDERR.store(true, Ordering::Relaxed);
}

fn env_nonempty(name: &str) -> Option<String> {
    match std::env::var_os(name) {
        Some(v) if !v.is_empty() => Some(v.to_string_lossy().into_owned()),
        _ => None,
    }
}

/// Should ANSI color codes be emitted? Same precedence as
/// `console_color_enabled()` in logger.hpp.
pub fn console_color_enabled() -> bool {
    if env_nonempty("NO_COLOR").is_some() {
        return false;
    }
    if env_nonempty("FORCE_COLOR").is_some() {
        return true;
    }
    if let Some(v) = env_nonempty("CLICOLOR_FORCE") {
        if v != "0" {
            return true;
        }
    }
    // SAFETY: isatty on a valid fd number has no preconditions.
    unsafe { libc::isatty(libc::STDOUT_FILENO) == 1 }
}

fn level_tag(level: log::Level) -> &'static str {
    match level {
        log::Level::Error => "ERROR",
        log::Level::Warn => "WARN",
        log::Level::Info => "INFO",
        log::Level::Debug => "DEBUG",
        log::Level::Trace => "TRACE",
    }
}

fn level_color(level: log::Level) -> &'static str {
    match level {
        log::Level::Error => "\x1b[31m",
        log::Level::Warn => "\x1b[33m",
        log::Level::Info => "\x1b[32m",
        log::Level::Debug => "\x1b[34m",
        log::Level::Trace => "\x1b[34m",
    }
}

/// `[YYYY-MM-DD HH:MM:SS.mmm]` in local time (C++ `localtime_r`).
fn timestamp() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs() as libc::time_t;
    let ms = now.subsec_millis();
    // SAFETY: localtime_r writes into the provided tm; both pointers are valid
    // for the duration of the call.
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&secs, &mut tm) };
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        ms
    )
}

struct CliLogger {
    min: log::LevelFilter,
    color: bool,
}

impl log::Log for CliLogger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        metadata.level() <= self.min
    }

    fn log(&self, record: &log::Record<'_>) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let ts = timestamp();
        let tag = record.target();
        let level = level_tag(record.level());
        let line = if self.color {
            format!(
                "[{}] [\x1b[36m{}\x1b[0m] [{}{}\x1b[0m] {}\n",
                ts,
                tag,
                level_color(record.level()),
                level,
                record.args()
            )
        } else {
            format!("[{}] [{}] [{}] {}\n", ts, tag, level, record.args())
        };
        // One write per line keeps interleaved threads from splitting a record.
        if TO_STDERR.load(Ordering::Relaxed) {
            let _ = std::io::stderr().write_all(line.as_bytes());
        } else {
            let _ = std::io::stdout().write_all(line.as_bytes());
        }
    }

    fn flush(&self) {
        let _ = std::io::stdout().flush();
        let _ = std::io::stderr().flush();
    }
}

/// Install the process-wide logger (first call wins; later calls are no-ops).
pub fn init() {
    let min = match env_nonempty("UPIX_LOG_LEVEL").as_deref() {
        Some("debug") => log::LevelFilter::Debug,
        _ => log::LevelFilter::Info,
    };
    let logger = CliLogger { min, color: console_color_enabled() };
    let _ = log::set_boxed_logger(Box::new(logger));
    log::set_max_level(min);
}

