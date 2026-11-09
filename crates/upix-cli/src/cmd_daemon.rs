//! `upix daemon {start, stop, status}` — manage the local warm-engine daemon.
//!
//! start:  fork into background (unless --fg), open the Unix socket, load a
//!         warm ncnn engine, and serve single-image upscale requests so that
//!         `upix process` skips the ~490ms init+load fixed cost. Exits by
//!         itself after --idle-timeout seconds without a request (0 = never).
//! stop:   read the daemon's pid file and SIGTERM it.
//! status: read-only report — pid file, liveness, socket reachability.

use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use upix_host::{ServeConfig, ServeDevice};

use crate::args::DaemonArgs;
use crate::common::{emit_json_line, json_escape, pid_is_upix_daemon};
use crate::cmd_host::CliBackend;
use upix_host::net_adapters::NetFramer;

pub fn run(a: &DaemonArgs) -> i32 {
    let socket_path = if a.socket_path.is_empty() {
        upix_host::default_serve_socket_path()
    } else {
        a.socket_path.clone()
    };
    if socket_path.is_empty() {
        eprintln!(
            "daemon: no default socket path available (set XDG_CACHE_HOME or HOME)"
        );
        return 1;
    }

    let pid_path = format!("{socket_path}.pid");

    if a.subcommand == "stop" {
        return match upix_host::stop(&socket_path) {
            Ok(()) => {
                // Wait for the daemon to actually exit: its accept() loop only
                // wakes on the next poll tick (~500 ms), so returning
                // immediately would leave `daemon status` reporting "running"
                // for a moment after stop.
                let pid: i32 = std::fs::read_to_string(&pid_path)
                    .ok()
                    .and_then(|s| s.trim().parse().ok())
                    .unwrap_or(0);
                for _ in 0..60 {
                    // up to 3 s
                    if pid <= 0 || !pid_is_upix_daemon(pid) {
                        println!("stopped (pid {pid})");
                        return 0;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(50));
                }
                println!("stop signal sent; pid {pid} still exiting (processing a request?)");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        };
    }

    if a.subcommand == "status" {
        return cmd_daemon_status(a, &socket_path, &pid_path);
    }

    // ── start ────────────────────────────────────────────────────────────────
    // Guard: if a pid file exists and names a live upix daemon, refuse
    // (already running). kill(0) alone would also bless zombies and recycled
    // pids and wedge every future start.
    {
        if let Ok(text) = std::fs::read_to_string(&pid_path) {
            if let Ok(existing) = text.trim().parse::<i32>() {
                if pid_is_upix_daemon(existing) {
                    eprintln!("daemon already running (pid {existing})");
                    return 1;
                }
            }
            // pid file present but process dead → stale, overwritten below.
        }
    }

    // (The ensure_running() spawner's `--upix-internal-daemon` argv is
    // dispatched in main() before the CLI parser; run_internal_daemon()
    // enters daemon_main() directly with foreground=true, so it never
    // reaches this fork.)

    // Background via single fork + setsid (detaches controlling terminal).
    if !a.foreground {
        // SAFETY: fork before any thread creation; the child either execs the
        // daemon body via run_daemon_fg or exits.
        let pid = unsafe { libc::fork() };
        if pid < 0 {
            log::error!(target: "Daemon", "fork failed");
            return 1;
        }
        if pid > 0 {
            return 0; // parent exits
        }
        // SAFETY: setsid with no preconditions in a freshly forked child.
        unsafe {
            libc::setsid();
        }
        // Close stdio: the daemon has no console output after this point
        // (logs go through the logger, which would otherwise write to the
        // inherited terminal fds).
        unsafe {
            libc::close(libc::STDIN_FILENO);
            libc::close(libc::STDOUT_FILENO);
            libc::close(libc::STDERR_FILENO);
        }
    }

    daemon_main(a, &socket_path, &pid_path)
}

fn cmd_daemon_status(a: &DaemonArgs, socket_path: &str, pid_path: &str) -> i32 {
    // Plain stdout — a status line is user-facing output, not a log.
    // --json: same facts as the prose line, as one document. Exit code
    // semantics are unchanged (1 = not running); consumers parse the
    // document regardless of the exit code. `stale` covers every
    // pid-file-without-live-daemon case (dead pid, corrupt file) — for
    // UI purposes they are all "the file lies".
    if a.json {
        let text = match std::fs::read_to_string(pid_path) {
            Ok(t) => t,
            Err(_) => {
                emit_json_line(r#"{"running":false,"stale":false}"#.to_string());
                return 1;
            }
        };
        let pid: i32 = text.trim().parse().unwrap_or(0);
        if pid <= 0 || !pid_is_upix_daemon(pid) {
            emit_json_line(r#"{"running":false,"stale":true}"#.to_string());
            return 1;
        }
        let sock_up = std::path::Path::new(socket_path).exists();
        emit_json_line(format!(
            r#"{{"running":true,"pid":{},"socket":"{}","socketMissing":{}}}"#,
            pid,
            json_escape(socket_path),
            if sock_up { "false" } else { "true" }
        ));
        return 0;
    }
    let text = match std::fs::read_to_string(pid_path) {
        Ok(t) => t,
        Err(_) => {
            println!("not running");
            return 1;
        }
    };
    let pid: i32 = match text.trim().parse() {
        Ok(p) => p,
        Err(_) => {
            eprintln!("status: corrupt pid file at {pid_path}");
            return 1;
        }
    };
    if !pid_is_upix_daemon(pid) {
        println!("not running (stale pid file)");
        return 1;
    }
    // Liveness alone can race with shutdown; the socket is the real contract.
    let sock_up = std::path::Path::new(socket_path).exists();
    if sock_up {
        println!("running (pid {pid}, socket {socket_path})");
    } else {
        println!("running (pid {pid}, socket {socket_path} — socket file missing!)");
    }
    0
}

/// Daemon body (foreground in the calling process). Writes the pid file on
/// success and unlinks it on shutdown.
fn daemon_main(a: &DaemonArgs, socket_path: &str, pid_path: &str) -> i32 {
    let device = if a.force_cpu { ServeDevice::Cpu } else { ServeDevice::Gpu };
    let idle = if a.idle_timeout_s <= 0 {
        0
    } else {
        a.idle_timeout_s
    };
    let cfg = ServeConfig {
        socket_path: socket_path.to_string(),
        models_dir: PathBuf::from(&a.models_dir),
        model_name: a.model_name.clone(),
        device,
        idle_timeout_s: idle,
        backend: Arc::new(CliBackend {
            models_dir: PathBuf::from(&a.models_dir),
        }),
        framer: Arc::new(NetFramer),
        uds: Box::new(upix_host::transport_std::StdUdsFactory),
    };
    // Preflight = the bind that C++'s ServeDaemon ctor does before the pid
    // file is written: a throwaway listener at the same path. Binding twice
    // is safe — the second (real) bind in run_daemon_fg unlinks the stale
    // file first, exactly like the C++ UnixServer.
    let uds: Box<dyn upix_host::seams::UdsFactory> =
        Box::new(upix_host::transport_std::StdUdsFactory);
    if let Err(e) = uds.listen(std::path::Path::new(socket_path)) {
        // We never wrote the pid file on a failed start, so whatever sits
        // there belongs to someone else. Remove it only when it describes
        // no live upix daemon: a lost start race (bind refused because
        // another daemon holds the socket) must not unlink the winner's
        // pid file.
        if let Ok(text) = std::fs::read_to_string(pid_path) {
            let owner: i32 = text.trim().parse().unwrap_or(0);
            if !pid_is_upix_daemon(owner) {
                let _ = std::fs::remove_file(pid_path);
            }
        }
        log::error!(target: "Daemon", "daemon start failed: {e}");
        return 1;
    }

    // Write pid file after successful start (so stop can find us).
    let _ = upix_host::write_pid_file(socket_path);

    // Wire signals to the daemon's stop(). `signal_handler` only sets the
    // generic g_running flag, which the daemon's run() loop does NOT observe —
    // so a SIGTERM from `daemon stop` would otherwise leave the daemon stuck
    // in accept().
    // C++ g_daemon indirection: the handler flips a process-global flag —
    // never upix_host::stop(), which SIGTERMs the pid in the pid file
    // (ourselves → recursive signal storm).
    crate::install_daemon_signal_handlers(Box::new(|| {
        STOP_FLAG.store(true, Ordering::SeqCst);
    }));

    // C++ parity: daemon.run() blocks on the calling thread and returns when
    // stop is requested (signal) or the idle timeout fired. run_daemon runs
    // the loop on a worker; this thread watches both exits:
    //   - SIGTERM/SIGINT → STOP_FLAG → handle.stop()
    //   - idle timeout → the loop unlinks the socket file → we observe it.
    let handle = match upix_host::run_daemon(cfg) {
        Ok(h) => h,
        Err(e) => {
            log::error!(target: "Daemon", "daemon loop failed: {e}");
            let _ = std::fs::remove_file(pid_path);
            return 1;
        }
    };
    while !STOP_FLAG.load(Ordering::SeqCst)
        && std::path::Path::new(socket_path).exists()
    {
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    handle.stop();

    // Cleanup on shutdown: daemon unlinks the socket; we unlink the pid file.
    let _ = std::fs::remove_file(pid_path);
    log::info!(target: "Daemon", "daemon shutting down");
    0
}

/// `--upix-internal-daemon <socket> <models_dir> <model_or_empty> <gpu|cpu>
/// <idle_timeout>` — the ensure_running() spawner's argv contract. Runs the
/// daemon in the foreground of this (already detached) process.
pub const INTERNAL_DAEMON_FLAG: &str = "--upix-internal-daemon";

/// Set by the daemon's SIGTERM/SIGINT handler; watched by daemon_main.
static STOP_FLAG: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Process-wide slot so the signal handler can reach the running daemon's
/// stop() (the daemon run loop does not observe g_running).
static DAEMON_STOP: std::sync::Mutex<Option<Box<dyn Fn() + Send>>> =
    std::sync::Mutex::new(None);

pub fn install_daemon_stop(stop: Box<dyn Fn() + Send>) {
    *DAEMON_STOP.lock().unwrap() = Some(stop);
}

pub fn request_daemon_stop() {
    if let Some(stop) = DAEMON_STOP.lock().unwrap().as_ref() {
        stop();
    }
}

#[derive(Debug, Clone)]
pub struct InternalDaemonArgs {
    pub socket_path: String,
    pub models_dir: String,
    pub model_name: String,
    pub device: ServeDevice,
    pub idle_timeout_s: i64,
}

pub fn get_internal_daemon_args(argv: &[String]) -> Option<InternalDaemonArgs> {
    if argv.len() < 6 || argv[1] != INTERNAL_DAEMON_FLAG {
        return None;
    }
    let device = match argv[5].as_str() {
        "gpu" => ServeDevice::Gpu,
        "cpu" => ServeDevice::Cpu,
        _ => return None,
    };
    Some(InternalDaemonArgs {
        socket_path: argv[2].clone(),
        models_dir: argv[3].clone(),
        model_name: argv[4].clone(),
        device,
        idle_timeout_s: argv[6].parse().unwrap_or(600),
    })
}

pub fn run_internal_daemon(args: &InternalDaemonArgs) -> i32 {
    let a = DaemonArgs {
        subcommand: "start".into(),
        model_name: args.model_name.clone(),
        models_dir: args.models_dir.clone(),
        socket_path: args.socket_path.clone(),
        force_cpu: args.device == ServeDevice::Cpu,
        foreground: true,
        idle_timeout_s: args.idle_timeout_s,
        json: false,
    };
    let socket_path = a.socket_path.clone();
    let pid_path = format!("{socket_path}.pid");
    daemon_main(&a, &socket_path, &pid_path)
}
