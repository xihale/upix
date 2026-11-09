//! Shared CLI helpers — Rust port of `cli/src/common.{hpp,cpp}`.

use std::path::Path;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

use upix_core::engine::ProcessingProgress;
use upix_core::{ProcessingConfig, sanitize_model_for_filename};
use upix_img::EncoderOptions;

use crate::args::ProcessArgs;
use crate::logging;

// ── Global run-state ────────────────────────────────────────────────────────
// Set false by the SIGINT/SIGTERM handler; long-running loops (process, host,
// daemon) poll these for graceful shutdown.
pub static G_RUNNING: AtomicBool = AtomicBool::new(true);
pub static G_CANCEL_REQUESTED: AtomicBool = AtomicBool::new(false);


// ── Defaults ────────────────────────────────────────────────────────────────
// Models directory resolution: the --models-dir flag (bound directly to each
// subcommand's field) > $UPIX_MODELS_DIR > platform default > ./models.
// Unix default: $XDG_DATA_HOME/upix/models (~/.local/share/upix/models).
// Windows default: %LOCALAPPDATA%\upix\models. A fixed user-level store,
// never cwd-relative.
pub fn default_models_dir() -> String {
    if let Some(env) = env_nonempty("UPIX_MODELS_DIR") {
        return env;
    }
    #[cfg(windows)]
    {
        let base = env_nonempty("LOCALAPPDATA").unwrap_or_else(|| {
            env_nonempty("APPDATA").unwrap_or_default()
        });
        if !base.is_empty() {
            return format!("{base}\\upix\\models");
        }
    }
    if let Some(xdg) = env_nonempty("XDG_DATA_HOME") {
        return format!("{xdg}/upix/models");
    }
    if let Some(home) = user_home() {
        return format!("{home}/.local/share/upix/models");
    }
    "./models".to_string() // no HOME/XDG at all (degenerate): keep something usable
}

fn env_nonempty(name: &str) -> Option<String> {
    match std::env::var_os(name) {
        Some(v) if !v.is_empty() => Some(v.to_string_lossy().into_owned()),
        _ => None,
    }
}

/// `upix::user_home()` (identity.cpp): $HOME, falling back to %USERPROFILE%.
pub fn user_home() -> Option<String> {
    if let Some(h) = env_nonempty("HOME") {
        return Some(h);
    }
    env_nonempty("USERPROFILE")
}

/// Is `pid` a live process that is actually our serve daemon? kill(pid, 0)
/// alone also succeeds for zombies and for an unrelated process that recycled
/// the pid. The exe symlink is the reliable check: it fails for zombies and
/// names the real binary otherwise; the command line must also carry the
/// daemon subcommand, not an unrelated upix process.
pub fn pid_is_upix_daemon(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    // Windows: no /proc and no kill(0) in libc-windows; tasklist is the
    // closest std-only equivalent — the image-name field ("upix-cli.exe")
    // is the zombie/recycled-pid guard (a dead or recycled pid simply does
    // not match). The command-line check is not available without heavier
    // Win32 bindings; the endpoint probe in the callers is the real daemon
    // contract, so a live non-daemon upix process passing here only costs a
    // bounded wait, never a wrong kill (stop() revalidates the same way).
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        match std::process::Command::new("tasklist")
            .args(["/FI", &format!("PID eq {pid}"), "/NH", "/FO", "CSV"])
            .creation_flags(CREATE_NO_WINDOW)
            .output()
        {
            Ok(o) => String::from_utf8_lossy(&o.stdout)
                .to_ascii_lowercase()
                .contains("\"upix"),
            Err(_) => false,
        }
    }
    #[cfg(unix)]
    {
        // SAFETY: kill with signal 0 is a permission probe with no side effects.
        if unsafe { libc::kill(pid, 0) } != 0 {
            return false;
        }
        let exe = match std::fs::read_link(format!("/proc/{pid}/exe")) {
            Ok(e) => e.to_string_lossy().into_owned(),
            Err(_) => return false, // zombie or vanished
        };
        if !exe.contains("upix") {
            return false;
        }
        match std::fs::read(format!("/proc/{pid}/cmdline")) {
            Ok(bytes) => String::from_utf8_lossy(&bytes).contains("daemon"),
            Err(_) => false,
        }
    }
}

// ── Image / format helpers ──────────────────────────────────────────────────
/// Infer output format ("png"/"jpg"/"webp"/"avif") from a file extension;
/// "" if unknown.
pub fn infer_format_from_extension(path: &str) -> String {
    let dot = match path.rfind('.') {
        Some(d) => d,
        None => return String::new(),
    };
    let ext = path[dot + 1..].to_ascii_lowercase();
    match ext.as_str() {
        "png" => "png".to_string(),
        "jpg" | "jpeg" => "jpg".to_string(),
        "webp" => "webp".to_string(),
        "avif" => "avif".to_string(),
        _ => String::new(),
    }
}

/// Default output location when none was given: `img.png` → `img_<model>.png`
/// (sibling, input extension kept when it is a known output format, else
/// .png); directory input `shots/` → `shots_<model>/` next to it.
pub fn derive_default_output(input_path: &str, model_name: &str, input_is_dir: bool) -> String {
    let inp = Path::new(input_path);
    let tag = format!("_{}", sanitize_model_for_filename(model_name));

    if input_is_dir {
        // filename() of "shots/" is "shots"; of "/" is "" (rejected upstream).
        let parent = inp.parent().unwrap_or(Path::new(""));
        return format!("{}/", parent.join(format!("{}{}", file_name_str(inp), tag)).display());
    }

    let mut ext = inp
        .extension()
        .map(|e| format!(".{}", e.to_string_lossy()))
        .unwrap_or_default();
    if ext.is_empty()
        || infer_format_from_extension(&file_name_str(inp)).is_empty()
    {
        ext = ".png".to_string();
    }
    let stem = inp
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    let parent = inp.parent().unwrap_or(Path::new(""));
    parent.join(format!("{stem}{tag}{ext}")).display().to_string()
}

fn file_name_str(p: &Path) -> String {
    p.file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default()
}

// ── Progress reporting ──────────────────────────────────────────────────────
/// Shared ETA estimate: 0 until a full second of throughput is observable and
/// once the last tile is done (matches what the human line always showed).
pub fn progress_eta_seconds(prog: &ProcessingProgress, start_time: Instant) -> i64 {
    if prog.completed_tiles <= 0 || prog.completed_tiles >= prog.total_tiles {
        return 0;
    }
    let elapsed = start_time.elapsed().as_secs_f64();
    if elapsed <= 0.1 {
        return 0;
    }
    let tiles_per_sec = prog.completed_tiles as f64 / elapsed;
    ((prog.total_tiles - prog.completed_tiles) as f64 / tiles_per_sec).round() as i64
}

/// Tile-progress update: machine JSON event when enabled, else the human
/// `\r` line on stderr. Producers: inline single image, daemon delegate,
/// directory batch — one call site each.
pub fn report_progress(prog: &ProcessingProgress, start_time: Instant) {
    if prog.total_tiles <= 0 {
        return;
    }
    if logging::machine_output_enabled() {
        let pct = prog.percentage();
        emit_json_line(format!(
            r#"{{"t":"progress","pct":{pct:.0},"done":{},"total":{},"eta":{}}}"#,
            prog.completed_tiles,
            prog.total_tiles,
            progress_eta_seconds(prog, start_time)
        ));
        return;
    }
    print_progress_stderr(prog, start_time);
}

/// Render a progress line to stderr:  45% (12/27, ETA 3s)
pub fn print_progress_stderr(prog: &ProcessingProgress, start_time: Instant) {
    use std::io::Write;
    if prog.total_tiles <= 0 {
        return;
    }
    let mut line = format!(
        "{:.0}% ({}/{}",
        prog.percentage(),
        prog.completed_tiles,
        prog.total_tiles
    );
    let eta_sec = progress_eta_seconds(prog, start_time);
    if eta_sec > 0 {
        line.push_str(&format!(", ETA {eta_sec}s"));
    }
    line.push(')');
    let mut err = std::io::stderr();
    let _ = write!(err, "\r{line}");
    let _ = err.flush();
    if prog.completed_tiles >= prog.total_tiles {
        let _ = writeln!(err);
    }
}

// ── Machine-readable output ─────────────────────────────────────────────────
/// Escape a UTF-8 string for embedding in a JSON string literal (quotes,
/// backslashes, control chars; bytes >= 0x20 pass through untouched).
pub fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// Write one machine event line to stdout and flush (GUI consumers read
/// incrementally; pairing prompts block on stdin right after an event).
pub fn emit_json_line(json: String) {
    println!("{json}");
    use std::io::Write;
    let _ = std::io::stdout().flush();
}

// ── Interruptible stdin ─────────────────────────────────────────────────────
/// Bound a silent/hostile writer (C++ `kMaxPromptLineBytes`).
pub const MAX_PROMPT_LINE_BYTES: usize = 4096;

/// Read one line from stdin without ever blocking past a poll tick, so a
/// signal can end the wait: the signal handler must not run while a blocked
/// std::getline() would simply restart (SA_RESTART) — pressing Ctrl+C at a
/// prompt would otherwise look like a hang until Enter is pressed.
/// `still_waiting` is re-checked every tick (the caller passes its shutdown
/// condition); when it returns false, or stdin is at EOF with nothing
/// buffered, the wait is abandoned and the function returns false (callers
/// treat that as "no approval").
/// Returns true when a complete line was read; the trailing '\n' is stripped.
pub fn read_line_interruptible(
    line: &mut String,
    still_waiting: &dyn Fn() -> bool,
) -> bool {
    line.clear();
    // Windows: no poll(2) on fds in libc; use blocking std reads. Degraded
    // but correct: still_waiting is re-checked between reads, and the CRT
    // delivers Ctrl+C to the handler after the current read returns.
    #[cfg(windows)]
    {
        use std::io::Read;
        let mut buf = [0u8; 256];
        match std::io::stdin().read(&mut buf) {
            Ok(0) => return !line.is_empty(),
            Ok(n) => {
                line.push_str(&String::from_utf8_lossy(&buf[..n]));
                if let Some(nl) = line.find('\n') {
                    line.truncate(nl);
                    return true;
                }
                // Same hostile-writer bound as the unix loop.
                if line.len() > MAX_PROMPT_LINE_BYTES {
                    line.truncate(MAX_PROMPT_LINE_BYTES);
                    return true;
                }
                !still_waiting()
            }
            Err(_) => false,
        }
    }
    #[cfg(unix)]
    loop {
        if !still_waiting() {
            return false; // shutting down: the caller answers the request
        }
        let mut pfd = libc::pollfd {
            fd: libc::STDIN_FILENO,
            events: libc::POLLIN,
            revents: 0,
        };
        // SAFETY: pfd is a valid single-element array for the duration of poll.
        let pr = unsafe { libc::poll(&mut pfd, 1, 200) };
        if pr < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue; // signal: re-check still_waiting()
            }
            return false;
        }
        if pr == 0 {
            continue; // tick: re-check still_waiting()
        }
        if pfd.revents & (libc::POLLIN | libc::POLLHUP | libc::POLLERR) == 0 {
            return false;
        }
        // Bytes are ready, so a single read() cannot block. A partial line
        // from a GUI writer (no '\n' yet) just loops — with a poll tick in
        // between, so a signal still gets through.
        let mut buf = [0u8; 256];
        // SAFETY: reading into a valid stack buffer from an owned fd.
        let n = unsafe { libc::read(libc::STDIN_FILENO, buf.as_mut_ptr().cast(), buf.len()) };
        if n < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            return false;
        }
        if n == 0 {
            // EOF: bytes already read count as the answer (std::getline
            // behaved the same); an empty buffer means stdin is unusable.
            return !line.is_empty();
        }
        line.push_str(&String::from_utf8_lossy(&buf[..n as usize]));
        if let Some(nl) = line.find('\n') {
            line.truncate(nl);
            return true;
        }
        if line.len() > MAX_PROMPT_LINE_BYTES {
            line.truncate(MAX_PROMPT_LINE_BYTES);
            return true;
        }
    }
}

// ── Config resolution ───────────────────────────────────────────────────────
/// Defaults -> config file -> explicitly supplied CLI flags. The
/// recommendation is deferred until an engine exists, and only applies
/// without a loaded config or an explicit tile size. A loaded file owns its
/// defaulted fields too.
pub struct EffectiveProcessingConfig {
    pub config: ProcessingConfig,
    pub recommend_tile_size: bool,
}

pub fn resolve_processing_config(a: &ProcessArgs) -> EffectiveProcessingConfig {
    let mut config = ProcessingConfig::default();
    let mut loaded = false;
    if !a.config_file.is_empty() {
        match ProcessingConfig::load(Path::new(&a.config_file)) {
            Ok(c) => {
                config = c;
                loaded = true;
                log::info!(target: "Config", "Loaded config from {}", a.config_file);
            }
            Err(e) => {
                log::warn!(target: "Config", "Failed to load config: {e}");
            }
        }
    }
    if a.tile_size_explicit {
        config.tile_size = a.tile_size;
        // A fixed CLI size overrides file-enabled auto sizing; supplying both
        // CLI options retains the existing --auto-tile behavior.
        config.auto_tile_size = false;
    }
    if a.tile_overlap_explicit {
        config.tile_overlap = a.tile_overlap;
    }
    if a.batch_size_explicit {
        config.batch_size = a.batch_size;
    }
    if a.auto_tile_explicit {
        config.auto_tile_size = a.auto_tile;
    }
    if a.min_tile_explicit {
        config.min_tile_size = a.min_tile;
    }
    if a.max_tile_explicit {
        config.max_tile_size = a.max_tile;
    }
    if a.output_quality >= 0 {
        config.output_quality = a.output_quality;
    }
    if !a.output_format.is_empty() {
        config.output_format = upix_core::config::normalize_output_format(&a.output_format);
    }
    if a.png_level_explicit {
        config.encoder.png_level = a.png_level;
    }
    if a.jpeg_subsampling_explicit {
        config.encoder.jpeg_subsampling = a.jpeg_subsampling;
    }
    if a.webp_method_explicit {
        config.encoder.webp_method = a.webp_method;
    }
    if a.webp_lossless_explicit {
        config.encoder.webp_lossless = a.webp_lossless;
    }
    if a.avif_speed_explicit {
        config.encoder.avif_speed = a.avif_speed;
    }
    if a.avif_subsampling_explicit {
        config.encoder.avif_subsampling = a.avif_subsampling;
    }
    if a.avif_threads_explicit {
        config.encoder.avif_threads = a.avif_threads;
    }
    let recommend_tile_size = !loaded && !a.tile_size_explicit && !config.auto_tile_size;
    EffectiveProcessingConfig { config, recommend_tile_size }
}

/// Per-format encoder knobs (wire mirror → the canonical EncoderOptions).
pub fn encoder_options_from_wire(w: &upix_core::config::EncoderOptionsWire) -> EncoderOptions {
    EncoderOptions {
        png_level: w.png_level,
        jpeg_subsampling: w.jpeg_subsampling,
        webp_method: w.webp_method,
        webp_lossless: w.webp_lossless,
        avif_speed: w.avif_speed,
        avif_subsampling: w.avif_subsampling,
        avif_threads: w.avif_threads,
    }
}

// ── QR / connection card ────────────────────────────────────────────────────
/// Render text as a 2-row-per-line block of Unicode half-blocks (same module
/// geometry as the C++ `generate_qr_ascii`: Ecc::MEDIUM, quiet zone 4).
pub fn generate_qr_ascii(text: &str) -> String {
    let qr = qrcodegen::QrCode::encode_text(text, qrcodegen::QrCodeEcc::Medium)
        .expect("qr payload fits the qrcodegen limits");

    // 半块字符渲染：两模块竖叠一字符，配合 1:2 终端字体格得到 ~1:1 方形模块。
    // quiet zone 取标准 4 模块——此前用 2 模块在暗色终端/滚动边框干扰下
    // 识别率明显下降。
    let mut result = String::new();
    let border: i32 = 4;
    let size: i32 = qr.size() as i32;

    let module = |x: i32, y: i32| -> bool {
        x >= 0 && y >= 0 && x < size && y < size && qr.get_module(x, y)
    };

    let mut y = -border;
    while y < size + border {
        let mut x = -border;
        while x < size + border {
            let top = module(x, y);
            let bottom = y + 1 < size + border && module(x, y + 1);
            if top && bottom {
                result.push('█');
            } else if top && !bottom {
                result.push('▀');
            } else if !top && bottom {
                result.push('▄');
            } else {
                result.push(' ');
            }
            x += 1;
        }
        result.push('\n');
        y += 2;
    }
    result
}

/// Connection card data shared by the human printer and the `--json`
/// emitters (`upix_host::pairing::ConnectionCard` shape).
pub use upix_host::ConnectionCard;

/// Resolve local addresses + identity into a card. Pure data — no I/O beyond
/// the interface enumeration, safe to call before any output mode is chosen.
pub fn build_connection_card(
    port: u16,
    identity_pubkey: &[u8; 32],
    fingerprint: &str,
) -> ConnectionCard {
    let addresses: Vec<String> = upix_net::local_ips()
        .into_iter()
        .map(|ip| ip.to_string())
        .collect();
    upix_host::build_connection_card(port, identity_pubkey, fingerprint, addresses)
}

/// Print the connection card shown to the user: all local IPv4 addresses
/// (first non-loopback one is used as the QR default), the identity
/// fingerprint, and a QR code carrying the JSON the Android app's
/// JoinInviteParser.parseJsonInvite() understands (v2: fingerprint + base64
/// pubkey). Shared by `host start` and `host info` to stay in sync.
pub fn print_connection_card(port: u16, identity_pubkey: &[u8; 32], fingerprint: &str) {
    let card = build_connection_card(port, identity_pubkey, fingerprint);

    println!("\nConnection Information:");
    for (i, addr) in card.addresses.iter().enumerate() {
        if i == 0 {
            println!("  IP: {addr}  (default for QR)");
        } else {
            println!("  IP: {addr}");
        }
    }
    println!("  Port: {}", card.port);
    println!("  Fingerprint: {}\n", card.fingerprint);
    println!("QR Code (scan with Android app):\n");

    // 极性：QR 规定亮底暗码。半块字符靠前景色渲染码块、空格露出背景，
    // 深色终端下会呈现反色的黑底白码，扫码器（含 ZXing）无法识别。
    // 在彩色终端上强制 SGR 白底黑前景（47/30），与终端主题无关。
    // 无色环境（管道/NO_COLOR）保持纯文本，由查看环境决定极性。
    let qr_text = generate_qr_ascii(&card.qr_payload);
    if logging::console_color_enabled() {
        for line in qr_text.split('\n') {
            if line.is_empty() {
                continue;
            }
            println!("\x1b[47m\x1b[30m{line}\x1b[0m");
        }
    } else {
        // C++ uses std::println("{}", qr_text); the payload already ends
        // with a newline, so println appends one more blank line.
        print!("{qr_text}\n");
    }
    use std::io::Write;
    let _ = std::io::stdout().flush();
}

#[path = "daemon_delegate.rs"]
mod daemon_delegate;
pub use daemon_delegate::*;
