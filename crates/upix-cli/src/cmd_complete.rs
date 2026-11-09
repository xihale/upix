//! `upix __complete WORDS...` — the dynamic shell-completion engine.
//!
//! Cobra/clap-style protocol: the bash/zsh/fish scripts emitted by
//! cmd_completions.rs are thin adapters that replay the command line being
//! completed into this hidden command and print what it returns (one
//! "candidate\tdescription" line per candidate). All context logic lives here,
//! so the per-shell scripts stay trivial and can never drift:
//!
//!   - sub-commands and flags come straight from the CLI definition tree —
//!     an option added there completes everywhere automatically;
//!   - model names come from ModelManager against the models dir in effect
//!     (--models/--models-dir seen on the line wins over $UPIX_MODELS_DIR);
//!   - input-file candidates are filtered to the decoders this binary actually
//!     ships: png/jpg/jpeg/webp always, avif only when system libavif loads
//!     (avif_decode_available()).
//!
//! main() dispatches argv[1] == "__complete" here before any clap parsing.
//! The last WORD is the partial word under the cursor (may be empty). Output
//! is stdout lines only; diagnostics must never reach it, so nothing here
//! logs and every filesystem/library probe fails silent.

use std::collections::BTreeSet;
use std::path::PathBuf;

use crate::common::default_models_dir;

struct Cand {
    value: String,
    desc: String,
}

struct Out {
    items: Vec<Cand>,
}

impl Out {
    fn emit(&mut self, value: impl Into<String>, desc: impl Into<String>) {
        self.items.push(Cand { value: value.into(), desc: desc.into() });
    }
}

// ── decoder/format knowledge ────────────────────────────────────────────────

/// Input extensions this binary can decode. avif joins only when the system
/// libavif loads right now — the "还得看情况" case, resolved at TAB time.
fn image_exts() -> BTreeSet<String> {
    let mut s: BTreeSet<String> = ["png", "jpg", "jpeg", "webp"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    if upix_img::avif_decode_available() {
        s.insert("avif".to_string());
    }
    s
}

fn complete_output_formats(out: &mut Out) {
    out.emit("png", "lossless");
    out.emit("jpg", "quality 1-100");
    out.emit("jpeg", "quality 1-100");
    out.emit("webp", "quality 1-100");
    if upix_img::avif_encode_available() {
        out.emit("avif", "needs system libavif");
    }
}

// ── file enumeration ────────────────────────────────────────────────────────

/// Directory entries matching the typed prefix; directories always offered
/// with a trailing '/' so paths stay navigable, files filtered by `exts`
/// (None = any file). A leading "~" round-trips so home-relative typing keeps
/// working.
fn complete_files(out: &mut Out, typed_prefix: &str, exts: Option<&BTreeSet<String>>, dirs_only: bool) {
    let mut real = typed_prefix.to_string();
    let tilde = real.starts_with('~');
    if tilde {
        let home = match std::env::var("HOME") {
            Ok(h) if !h.is_empty() => h,
            _ => return,
        };
        real = format!("{home}{}", &real[1..]); // "~/ab" → "$HOME/ab", "~" → "$HOME"
    }

    let (dir_real, stem) = match real.rfind('/') {
        None => (String::new(), real.clone()),
        Some(slash) => (real[..slash + 1].to_string(), real[slash + 1..].to_string()),
    };

    // What to echo back in front of each name: exactly what the user typed,
    // with $HOME mapped back to ~ when applicable.
    let mut shown_dir = dir_real.clone();
    if tilde && !shown_dir.is_empty() {
        if let Ok(home) = std::env::var("HOME") {
            if shown_dir.starts_with(&home) {
                shown_dir = format!("~{}", &shown_dir[home.len()..]);
            }
        }
    }

    let dir = if dir_real.is_empty() { PathBuf::from(".") } else { PathBuf::from(&dir_real) };
    let Ok(entries) = std::fs::read_dir(&dir) else { return };

    let mut hits: Vec<Cand> = Vec::new();
    for entry in entries.flatten() {
        let p = entry.path();
        let name = p
            .file_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        if !name.starts_with(stem.as_str()) {
            continue;
        }
        if stem.is_empty() && !name.is_empty() && name.starts_with('.') {
            continue; // hide dotfiles
        }
        let is_dir = p.is_dir();
        if !is_dir && !dirs_only {
            if let Some(exts) = exts {
                let ext = p
                    .extension()
                    .map(|e| e.to_string_lossy().to_ascii_lowercase())
                    .unwrap_or_default();
                if ext.is_empty() || !exts.contains(&ext) {
                    continue;
                }
            }
        }
        hits.push(Cand {
            value: format!("{shown_dir}{name}{}", if is_dir { "/" } else { "" }),
            desc: String::new(),
        });
    }

    hits.sort_by(|a, b| a.value.cmp(&b.value));
    out.items.extend(hits);
}

// ── model names ─────────────────────────────────────────────────────────────

/// Installed models for -m/--model. Pre-checking the directory keeps
/// ModelManager's stdout warning out of the candidate stream.
fn complete_models(out: &mut Out, models_dir_override: &str) {
    let dir = if models_dir_override.is_empty() {
        PathBuf::from(default_models_dir())
    } else {
        PathBuf::from(models_dir_override)
    };
    if !dir.is_dir() {
        return;
    }
    let mut manager = upix_core::ModelManager::new(dir);
    let Ok(models) = manager.scan_models() else { return };
    for m in models {
        let desc = if m.scale_factor > 0 { format!("{}x", m.scale_factor) } else { String::new() };
        out.emit(m.name, desc);
    }
}

// ── tree walking ────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Node {
    Root,
    Model,
    ModelList,
    ModelListRemote,
    ModelDownload,
    ModelImport,
    Host,
    HostStart,
    HostInfo,
    Daemon,
    DaemonStart,
    DaemonStop,
    DaemonStatus,
    Completions,
}

fn subcommands_of(node: Node) -> &'static [(&'static str, &'static str)] {
    match node {
        Node::Root => &[
            ("model", "Manage models (list, download, import)"),
            ("host", "Start or query the coordinator for phones"),
            ("daemon", "Manage the warm-engine daemon"),
        ],
        Node::Model => &[
            ("list", "List installed models"),
            ("list-remote", "List models in the GitHub release"),
            ("download", "Download model(s) from the release"),
            ("import", "Import local .param/.bin files"),
        ],
        Node::Host => &[
            ("start", "Start the coordinator (accepts one phone, upscales submitted images)"),
            ("info", "Print the connection card (IP/QR/key)"),
        ],
        Node::Daemon => &[
            ("start", "Start the daemon (default)"),
            ("stop", "Stop the daemon"),
            ("status", "Report whether the daemon is running"),
        ],
        _ => &[],
    }
}

/// (long, short, takes_value, description) for each node's options. Order
/// mirrors the CLI definition.
fn options_of(node: Node) -> Vec<(&'static str, &'static str, bool, &'static str)> {
    let mut v: Vec<(&'static str, &'static str, bool, &'static str)> = Vec::new();
    match node {
        Node::Root => {
            v.extend([
                ("--input", "-i", true, "Input image or directory"),
                ("--output", "-o", true, "Output path (default: derived from input + model)"),
                ("--model", "-m", true, "Model name (default: first available)"),
                ("--models", "", true, "Models directory"),
                ("--tile-size", "", true, "Tile size"),
                ("--tile-overlap", "", true, "Tile overlap"),
                ("--batch-size", "", true, "Batch size"),
                ("--auto-tile", "", false, "Auto-select tile size from GPU VRAM"),
                ("--min-tile", "", true, "Minimum auto-tile size"),
                ("--max-tile", "", true, "Maximum auto-tile size"),
                ("--jobs", "-j", true, "Parallel workers for directory mode"),
                ("--gpu", "", false, "Force GPU (directory mode: GPU submit-queue pipeline; single image: prefer GPU)"),
                ("--cpu", "", false, "Force CPU only (never GPU, and never the GPU warm-engine daemon)"),
                ("--no-daemon", "", false, "Never use the warm-engine daemon (process inline)"),
                ("--force", "-f", false, "Re-process inputs that already carry an SR marker (such inputs are skipped by default)"),
                ("--output-quality", "", true, "Output quality 0-100, shared by the lossy encoders (default: 90)"),
                ("--output-format", "", true, "Output format (default: auto from extension)"),
                ("--png-level", "", true, "PNG zlib compression level 0-9 (default: auto, derived from --output-quality)"),
                ("--jpeg-subsampling", "", true, "JPEG chroma subsampling"),
                ("--webp-method", "", true, "WebP encode effort 0-6, higher = smaller but slower (default: auto, 4)"),
                ("--webp-lossless", "", false, "Force lossless WebP (default: lossy at --output-quality)"),
                ("--avif-speed", "", true, "AVIF encoder speed 0-10, higher = much faster, larger files"),
                ("--avif-subsampling", "", true, "AVIF chroma subsampling"),
                ("--avif-threads", "", true, "AVIF encoder tile threads (default: auto, hardware concurrency)"),
                ("--config", "", true, "Config file path"),
            ]);
        }
        Node::ModelList => {
            v.extend([
                ("--models-dir", "", true, "Models directory"),
                ("--names-only", "", false, "Print model names only, one per line (for scripts/completions)"),
                ("--json", "", false, "Emit one JSON document: {modelsDir, installed:[…]}"),
            ]);
        }
        Node::ModelListRemote => {
            v.extend([
                ("--repo", "", true, "GitHub repo owner/name (default: upstream release)"),
                ("--json", "", false, "Emit one JSON document: {manifestUrl, remote:[…]}"),
            ]);
        }
        Node::ModelDownload => {
            v.extend([
                ("--models-dir", "", true, "Models directory"),
                ("--repo", "", true, "GitHub repo owner/name (default: upstream release)"),
                ("--json", "", false, "Emit JSON download events: {\"t\":\"download\",name,pct}"),
            ]);
        }
        Node::ModelImport => {
            v.extend([
                ("--models-dir", "", true, "Models directory"),
                ("--json", "", false, "Emit one JSON document: {ok, imported:[…] | error}"),
            ]);
        }
        Node::Host | Node::HostStart => {
            v.extend([
                ("--port", "-p", true, "Port"),
                ("--models-dir", "", true, "Models directory"),
                ("--auto-approve", "", false, "Auto-approve all pairing requests without prompting (for automated testing/CI)"),
                ("--json", "", false, "Emit JSON events (card/pairing/task) on stdout; logs to stderr"),
            ]);
        }
        Node::HostInfo => {
            v.extend([
                ("--port", "-p", true, "Port"),
                ("--json", "", false, "Emit one JSON document: {ips, port, fingerprint, qr}"),
            ]);
        }
        Node::Daemon | Node::DaemonStart => {
            v.extend([
                ("--model", "", true, "Model name"),
                ("--models-dir", "", true, "Models directory"),
                ("--socket", "", true, "Unix socket path"),
                ("--cpu", "", false, "Force CPU"),
                ("--fg", "", false, "Run in foreground"),
                ("--idle-timeout", "", true, "Exit after N idle seconds (0 = never)"),
            ]);
        }
        Node::DaemonStop => {
            v.extend([("--socket", "", true, "Unix socket path")]);
        }
        Node::DaemonStatus => {
            v.extend([
                ("--socket", "", true, "Daemon endpoint path (unix socket; Windows: pid-file base for the loopback port)"),
                ("--json", "", false, "Emit one JSON document: {running, pid, endpoint, endpointMissing} | {running:false, stale}"),
            ]);
        }
        Node::Completions => {
            v.extend([("--install", "-i", false, "Write the script into this shell's user completion directory instead of printing it")]);
        }
        _ => {}
    }
    v
}

fn find_option(node: Node, tok: &str) -> Option<(&'static str, &'static str, bool, &'static str)> {
    let eq = tok.find('=').unwrap_or(tok.len());
    let t = &tok[..eq];
    for o in options_of(node) {
        if !o.1.is_empty() && t == o.1 {
            return Some(o);
        }
        if t == o.0 {
            return Some(o);
        }
        // attached short value (-ifile)
        if t.len() > 2 && o.1.len() == 2 && tok.starts_with(o.1) && !o.0.starts_with("--") {
            return Some(o);
        }
    }
    None
}

fn find_subcommand(node: Node, name: &str) -> Option<Node> {
    let next = match node {
        Node::Root => {
            if name == "model" {
                Node::Model
            } else if name == "host" {
                Node::Host
            } else if name == "daemon" {
                Node::Daemon
            } else if name == "completions" {
                Node::Completions
            } else {
                return None;
            }
        }
        Node::Model => match name {
            "list" => Node::ModelList,
            "list-remote" => Node::ModelListRemote,
            "download" => Node::ModelDownload,
            "import" => Node::ModelImport,
            _ => return None,
        },
        Node::Host => match name {
            "start" => Node::HostStart,
            "info" => Node::HostInfo,
            _ => return None,
        },
        Node::Daemon => match name {
            "start" => Node::DaemonStart,
            "stop" => Node::DaemonStop,
            "status" => Node::DaemonStatus,
            _ => return None,
        },
        _ => return None,
    };
    Some(next)
}

fn complete_subcommands(out: &mut Out, node: Node, cur: &str) {
    for (name, desc) in subcommands_of(node) {
        if cur.is_empty() || name.starts_with(cur) {
            out.emit(*name, *desc);
        }
    }
}

fn complete_option_names(out: &mut Out, node: Node, cur: &str) {
    for (long, short, _takes, desc) in options_of(node) {
        if !short.is_empty() && short.starts_with(cur) {
            out.emit(short, desc);
        }
        if long.starts_with(cur) {
            out.emit(long, desc);
        }
    }
}

/// Value completion for the option whose value is being typed.
fn complete_option_value(
    out: &mut Out,
    _node: Node,
    option: &(&'static str, &'static str, bool, &'static str),
    cur: &str,
    models_dir_override: &str,
) {
    let name = option.0;
    if name == "--model" {
        complete_models(out, models_dir_override);
    } else if name == "--input" {
        let exts = image_exts();
        complete_files(out, cur, Some(&exts), false);
    } else if name == "--models" || name == "--models-dir" {
        complete_files(out, cur, None, true);
    } else if name == "--output-format" {
        complete_output_formats(out);
    } else if name == "--jpeg-subsampling" {
        out.emit("420", "default");
        out.emit("422", "");
        out.emit("444", "best chroma");
    } else if name == "--avif-subsampling" {
        out.emit("420", "default");
        out.emit("444", "best chroma");
    } else if name == "--output" || name == "--config" || name == "--socket" {
        complete_files(out, cur, None, false); // plain paths: let every entry through
    }
    // Free-form values (ports, tile sizes, durations, repo, …): no candidates.
}

pub fn run(words: &[String]) -> i32 {
    let mut out = Out { items: Vec::new() };
    let cur = words.last().cloned().unwrap_or_default();
    let words: Vec<String> = if words.is_empty() {
        Vec::new()
    } else {
        words[..words.len() - 1].to_vec()
    };

    let mut node = Node::Root; // deepest subcommand reached
    let mut parent = Node::Root; // one level up (for model import/download)
    let mut pending: Option<(&'static str, &'static str, bool, &'static str)> = None;
    let mut positionals = 0usize; // positional words at the current node
    let mut only_positional = false; // seen "--"
    let mut models_dir_override = String::new();

    for tok in &words {
        if let Some(opt) = pending {
            if (opt.0 == "--models" || opt.0 == "--models-dir") && !tok.is_empty() {
                models_dir_override = tok.clone();
            }
            pending = None;
            continue;
        }
        if !only_positional && tok == "--" {
            only_positional = true;
            continue;
        }
        if !only_positional && tok.len() >= 2 && tok.starts_with('-') {
            if let Some(o) = find_option(node, tok) {
                if o.2 {
                    let eq = tok.find('=');
                    if eq.is_none() {
                        pending = Some(o); // bare "--x": value is the next word
                    } else if o.0 == "--models" || o.0 == "--models-dir" {
                        // inline "--x=v"
                        models_dir_override = tok[eq.unwrap() + 1..].to_string();
                    }
                }
            }
            continue;
        }
        if let Some(sub) = find_subcommand(node, tok) {
            parent = node;
            node = sub;
            positionals = 0;
        } else {
            positionals += 1;
        }
    }

    if let Some(opt) = pending {
        complete_option_value(&mut out, node, &opt, &cur, &models_dir_override);
    } else if !only_positional && cur.starts_with('-') {
        complete_option_names(&mut out, node, &cur);
    } else if node == Node::Root {
        if positionals == 0 {
            // Root, first word: sub-command or input image (`upix img.png`
            // upscales directly).
            complete_subcommands(&mut out, node, &cur);
            let exts = image_exts();
            complete_files(&mut out, &cur, Some(&exts), false);
        } else {
            complete_files(&mut out, &cur, None, false); // later root positionals: output path
        }
    } else if node == Node::Completions {
        if positionals == 0 {
            out.emit("bash", "");
            out.emit("zsh", "");
            out.emit("fish", "");
        }
    } else if node == Node::ModelImport && parent == Node::Model {
        let ncnn_exts: BTreeSet<String> = ["param", "bin"].iter().map(|s| s.to_string()).collect();
        complete_files(&mut out, &cur, Some(&ncnn_exts), false);
    } else if positionals == 0 {
        // Nested verb slots: model/host/daemon sub-subcommands.
        complete_subcommands(&mut out, node, &cur);
    } else {
        let _ = parent;
    }
    // Everything else (daemon/host verbs, `model download`'s remote
    // catalog): no candidates rather than misleading ones.

    for c in out.items {
        println!("{}\t{}", c.value, c.desc);
    }
    0
}
