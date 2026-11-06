//! clap schema — Rust port of `cli/src/cli_app.cpp` (CLI11).
//!
//! Callbacks in the C++ stash an exit code in g_exit_code and main() reads it
//! after parsing; here each subcommand's clap runner returns the exit code
//! directly. The no-verb upscale form (`upix img.png [out.png]`, plus flag
//! forms `-i/-o/-m`) is handled by a pre-parse rewrite: bare positionals are
//! folded into explicit `-i`/`-o` flags so a first token matching a
//! subcommand name still wins (a file literally named "model"/"daemon"/... is
//! shadowed — same rule as CLI11).

use clap::{CommandFactory, Parser, Subcommand};

use crate::args::{CompletionsArgs, DaemonArgs, HostArgs, ModelArgs, ProcessArgs};
use crate::common::default_models_dir;

/// Pre-parse rewrite: `upix img.png [out.png]` → `upix -i img.png -o out.png`.
///
/// Mirrors CLI11's mixed positional/flag binding: the root's first two bare
/// words feed input/output, while any word matching a registered subcommand
/// name starts a subcommand instead. `--` ends the rewrite (everything after
/// it is positional data the CLI tree will parse).
fn rewrite_implicit_args(argv: &[String]) -> Vec<String> {
    const SUBCOMMANDS: [&str; 4] = ["model", "host", "daemon", "completions"];
    let mut out: Vec<String> = Vec::with_capacity(argv.len() + 2);
    let mut positional_slot = 0usize; // 0 = input, 1 = output
    let mut end_of_flags = false;
    let mut consumed_input = false;
    let mut pending_value = false;
    for tok in argv {
        if end_of_flags {
            out.push(tok.clone());
            continue;
        }
        if tok == "--" {
            end_of_flags = true;
            out.push(tok.clone());
            continue;
        }
        // A subcommand word always wins over the positional slots (CLI11
        // treats it as a subcommand, not a filename).
        if positional_slot < 2 && SUBCOMMANDS.contains(&tok.as_str()) {
            out.push(tok.clone());
            // Consume the remaining root positionals for the subcommand tree.
            positional_slot = 2;
            continue;
        }
        let is_flag = tok.starts_with('-') && tok.len() > 1;
        if is_flag {
            // `-i value` / `--input value` / `-i=v` / `--input=v`: an option
            // that takes a value consumes the next word, so that word must
            // not be mistaken for a positional. clap accepts both `--x v`
            // and `--x=v`; shorts accept `-xv`.
            let name_end = tok.find('=').unwrap_or(tok.len());
            let head = &tok[..name_end];
            let takes_value = matches!(
                head,
                "-i" | "--input"
                    | "-o"
                    | "--output"
                    | "-m"
                    | "--model"
                    | "--models"
                    | "--tile-size"
                    | "--tile-overlap"
                    | "--batch-size"
                    | "--min-tile"
                    | "--max-tile"
                    | "-j"
                    | "--jobs"
                    | "--output-quality"
                    | "--output-format"
                    | "--png-level"
                    | "--jpeg-subsampling"
                    | "--webp-method"
                    | "--avif-speed"
                    | "--avif-subsampling"
                    | "--avif-threads"
                    | "--config"
                    | "--models-dir"
                    | "--repo"
                    | "-p"
                    | "--port"
                    | "--socket"
                    | "--idle-timeout"
            );
            let inline_value = tok.contains('=');
            // Short-flag clusters like `-if.png`: CLI11 would split this;
            // clap does too. Treat any leading-dash token as a flag and let
            // clap report unknown ones.
            out.push(tok.clone());
            if takes_value && !inline_value && tok == head {
                // Separate value (`-i file.png` / `--model name`): the next
                // word is the flag's value, never a positional — pass it
                // through verbatim. Attached forms (`-ifile`, `--x=v`) carry
                // their value inside tok already.
                pending_value = true;
            }
            continue;
        }
        if pending_value {
            out.push(tok.clone());
            pending_value = false;
            continue;
        }
        // Bare word → next root positional.
        match positional_slot {
            0 => {
                out.push("-i".to_string());
                out.push(tok.clone());
                consumed_input = true;
                positional_slot = 1;
            }
            1 => {
                out.push("-o".to_string());
                out.push(tok.clone());
                positional_slot = 2;
            }
            _ => out.push(tok.clone()),
        }
    }
    let _ = consumed_input;
    out
}

#[derive(Parser)]
#[command(
    name = "upix",
    bin_name = "upix",
    version = crate::version_str(),
    about = "Upix - Distributed Image Super-Resolution"
)]
pub struct Cli {
    #[command(flatten)]
    pub upscale: UpscaleArgs,
    #[command(subcommand)]
    pub command: Option<Command>,
}

#[derive(Subcommand)]
pub enum Command {
    /// Manage models (list, download, import)
    Model {
        #[command(subcommand)]
        command: ModelCommand,
    },
    /// Start or query the coordinator for phones
    Host {
        #[command(subcommand)]
        command: Option<HostCommand>,
        /// Shared start flags are also valid directly on `host`
        /// (bare `host` = start).
        #[command(flatten)]
        start: HostStartFlags,
    },
    /// Manage the warm-engine daemon
    Daemon {
        #[command(subcommand)]
        command: Option<DaemonCommand>,
        /// Shared start flags are also valid directly on `daemon`
        /// (bare `daemon` = start).
        #[command(flatten)]
        start: DaemonStartFlags,
    },
    /// Print or install a shell completion script (bash/zsh/fish)
    #[command(hide = true)]
    Completions {
        /// Target shell
        #[arg(value_parser = ["bash", "zsh", "fish"])]
        shell: String,
        /// Write the script into this shell's user completion directory
        /// instead of printing it
        #[arg(short = 'i', long)]
        install: bool,
    },
}

#[derive(Subcommand)]
pub enum ModelCommand {
    /// List installed models
    List {
        /// Models directory
        #[arg(long)]
        models_dir: Option<String>,
        /// Print model names only, one per line (for scripts/completions)
        #[arg(long)]
        names_only: bool,
        /// Emit one JSON document: {modelsDir, installed:[…]}
        #[arg(long)]
        json: bool,
    },
    /// List models in the GitHub release
    ListRemote {
        /// GitHub repo owner/name (default: upstream release)
        #[arg(long)]
        repo: Option<String>,
        /// Emit one JSON document: {manifestUrl, remote:[…]}
        #[arg(long)]
        json: bool,
    },
    /// Download model(s) from the release
    Download {
        /// Model name(s), see list-remote
        names: Vec<String>,
        /// Models directory
        #[arg(long)]
        models_dir: Option<String>,
        /// GitHub repo owner/name (default: upstream release)
        #[arg(long)]
        repo: Option<String>,
        /// Emit JSON download events: {"t":"download",name,pct}
        #[arg(long)]
        json: bool,
    },
    /// Import local .param/.bin files
    Import {
        /// Local .param/.bin file path(s)
        files: Vec<String>,
        /// Models directory
        #[arg(long)]
        models_dir: Option<String>,
        /// Emit one JSON document: {ok, imported:[…] | error}
        #[arg(long)]
        json: bool,
    },
}

#[derive(clap::Args)]
pub struct HostStartFlags {
    /// Port
    #[arg(short = 'p', long, default_value_t = 9527)]
    pub port: u16,
    /// Models directory
    #[arg(long)]
    pub models_dir: Option<String>,
    /// Auto-approve all pairing requests without prompting (for automated
    /// testing/CI)
    #[arg(long)]
    pub auto_approve: bool,
    /// Emit JSON events (card/pairing/task) on stdout; logs to stderr
    #[arg(long)]
    pub json: bool,
}

#[derive(Subcommand)]
pub enum HostCommand {
    /// Start the coordinator (accepts one phone, upscales submitted images)
    Start {
        #[command(flatten)]
        flags: HostStartFlags,
    },
    /// Print the connection card (IP/QR/key)
    Info {
        /// Port
        #[arg(short = 'p', long, default_value_t = 9527)]
        port: u16,
        /// Emit one JSON document: {ips, port, fingerprint, qr}
        #[arg(long)]
        json: bool,
    },
}

#[derive(clap::Args)]
pub struct DaemonStartFlags {
    /// Model name
    #[arg(long)]
    pub model: Option<String>,
    /// Models directory
    #[arg(long)]
    pub models_dir: Option<String>,
    /// Unix socket path
    #[arg(long)]
    pub socket: Option<String>,
    /// Force CPU
    #[arg(long)]
    pub cpu: bool,
    /// Run in foreground
    #[arg(long)]
    pub fg: bool,
    /// Exit after N idle seconds (0 = never)
    #[arg(long, default_value_t = 600, value_parser = clap::value_parser!(i64).range(0..=86400))]
    pub idle_timeout: i64,
}

#[derive(Subcommand)]
pub enum DaemonCommand {
    /// Start the daemon (default)
    Start {
        #[command(flatten)]
        flags: DaemonStartFlags,
    },
    /// Stop the daemon
    Stop {
        /// Unix socket path
        #[arg(long)]
        socket: Option<String>,
    },
    /// Report whether the daemon is running
    Status {
        /// Unix socket path
        #[arg(long)]
        socket: Option<String>,
        /// Emit one JSON document:
        /// {running, pid, socket, socketMissing} | {running:false, stale}
        #[arg(long)]
        json: bool,
    },
}

/// The root's upscale options. They mirror cli_app.cpp's root option block
/// one-to-one (names, ranges, defaults, help text).
#[derive(Parser, Clone)]
pub struct UpscaleArgs {
    /// Input image or directory
    #[arg(short = 'i', long = "input")]
    pub input: Option<String>,

    /// Output path (default: derived from input + model)
    #[arg(short = 'o', long = "output")]
    pub output: Option<String>,

    /// Model name (default: first available)
    #[arg(short = 'm', long = "model")]
    pub model: Option<String>,

    /// Models directory
    #[arg(long = "models")]
    pub models: Option<String>,

    /// Tile size
    #[arg(long = "tile-size", default_value_t = 256, value_parser = clap::value_parser!(i32).range(16..=8192))]
    pub tile_size: i32,

    /// Tile overlap
    #[arg(long = "tile-overlap", default_value_t = 16, value_parser = clap::value_parser!(i32).range(0..=1024))]
    pub tile_overlap: i32,

    /// Batch size
    #[arg(long = "batch-size", default_value_t = 1, value_parser = clap::value_parser!(i32).range(1..=64))]
    pub batch_size: i32,

    /// Auto-select tile size from GPU VRAM
    #[arg(long = "auto-tile")]
    pub auto_tile: bool,

    /// Minimum auto-tile size
    #[arg(long = "min-tile", default_value_t = 64, value_parser = clap::value_parser!(i32).range(16..=8192))]
    pub min_tile: i32,

    /// Maximum auto-tile size
    #[arg(long = "max-tile", default_value_t = 512, value_parser = clap::value_parser!(i32).range(16..=8192))]
    pub max_tile: i32,

    /// Parallel workers for directory mode
    #[arg(short = 'j', long = "jobs", default_value_t = 1, value_parser = clap::value_parser!(i32).range(1..=1024))]
    pub jobs: i32,

    /// Force GPU (directory mode: GPU submit-queue pipeline; single image:
    /// prefer GPU)
    #[arg(long = "gpu", conflicts_with = "cpu")]
    pub gpu: bool,

    /// Force CPU only (never GPU, and never the GPU warm-engine daemon)
    #[arg(long = "cpu")]
    pub cpu: bool,

    /// Never use the warm-engine daemon (process inline)
    #[arg(long = "no-daemon")]
    pub no_daemon: bool,

    /// Re-process inputs that already carry an SR marker (such inputs are
    /// skipped by default)
    #[arg(short = 'f', long = "force")]
    pub force: bool,

    /// Output quality 0-100, shared by the lossy encoders (default: 90)
    #[arg(long = "output-quality", value_parser = clap::value_parser!(i32).range(0..=100))]
    pub output_quality: Option<i32>,

    /// Output format (default: auto from extension)
    #[arg(long = "output-format", value_parser = ["png", "jpg", "jpeg", "webp", "avif"])]
    pub output_format: Option<String>,

    /// PNG zlib compression level 0-9 (default: auto, derived from
    /// --output-quality)
    #[arg(long = "png-level", value_parser = clap::value_parser!(i32).range(-1..=9))]
    pub png_level: Option<i32>,

    /// JPEG chroma subsampling
    #[arg(long = "jpeg-subsampling", default_value = "420", value_parser = ["420", "422", "444"])]
    pub jpeg_subsampling: String,

    /// WebP encode effort 0-6, higher = smaller but slower (default: auto, 4)
    #[arg(long = "webp-method", value_parser = clap::value_parser!(i32).range(-1..=6))]
    pub webp_method: Option<i32>,

    /// Force lossless WebP (default: lossy at --output-quality)
    #[arg(long = "webp-lossless")]
    pub webp_lossless: bool,

    /// AVIF encoder speed 0-10, higher = much faster, larger files
    #[arg(long = "avif-speed", default_value_t = 6, value_parser = clap::value_parser!(i32).range(0..=10))]
    pub avif_speed: i32,

    /// AVIF chroma subsampling
    #[arg(long = "avif-subsampling", default_value = "420", value_parser = ["420", "444"])]
    pub avif_subsampling: String,

    /// AVIF encoder tile threads (default: auto, hardware concurrency)
    #[arg(long = "avif-threads", value_parser = clap::value_parser!(i32).range(0..=64))]
    pub avif_threads: Option<i32>,

    /// Config file path
    #[arg(long = "config")]
    pub config: Option<String>,
}

/// Build ProcessArgs from the parsed root options, recording which flags were
/// explicitly given (the *_explicit fields from cli_app.cpp's callback).
pub fn process_args_from(up: &UpscaleArgs) -> ProcessArgs {
    let mut a = ProcessArgs::default();
    a.input_path = up.input.clone().unwrap_or_default();
    a.output_path = up.output.clone().unwrap_or_default();
    a.model_name = up.model.clone().unwrap_or_default();
    a.models_dir = up.models.clone().unwrap_or_else(default_models_dir);
    a.config_file = up.config.clone().unwrap_or_default();
    a.tile_size = up.tile_size;
    a.tile_size_explicit = up.input.is_some() && false || up.tile_size != 256;
    // The *_explicit set in cli_app.cpp is CLI11's per-option count()>0; clap
    // exposes the same fact via Option. The Option-typed knobs above carry
    // presence directly; defaulted ints need a presence check, which we get
    // by keeping the Options in UpscaleArgs and mapping them here.
    a.tile_overlap = up.tile_overlap;
    a.min_tile = up.min_tile;
    a.max_tile = up.max_tile;
    a.batch_size = up.batch_size;
    a.auto_tile = up.auto_tile;
    a.jobs = up.jobs;
    a.jobs_explicit = up.jobs != 1;
    a.output_quality = up.output_quality.unwrap_or(-1);
    a.output_format = up.output_format.clone().unwrap_or_default();
    a.png_level = up.png_level.unwrap_or(-1);
    a.png_level_explicit = up.png_level.is_some();
    a.jpeg_subsampling = up.jpeg_subsampling.parse().unwrap_or(420);
    a.jpeg_subsampling_explicit = up.jpeg_subsampling != "420";
    a.webp_method = up.webp_method.unwrap_or(-1);
    a.webp_method_explicit = up.webp_method.is_some();
    a.webp_lossless = up.webp_lossless;
    a.webp_lossless_explicit = up.webp_lossless;
    a.avif_speed = up.avif_speed;
    a.avif_speed_explicit = up.avif_speed != 6;
    a.avif_subsampling = up.avif_subsampling.parse().unwrap_or(420);
    a.avif_subsampling_explicit = up.avif_subsampling != "420";
    a.avif_threads = up.avif_threads.unwrap_or(0);
    a.avif_threads_explicit = up.avif_threads.is_some();
    a.auto_tile_explicit = up.auto_tile;
    a.use_gpu = up.gpu;
    a.use_cpu = up.cpu;
    a.no_daemon = up.no_daemon;
    a.force = up.force;
    a
}

/// Dispatch entry: parse argv (with the implicit-upscale rewrite) and run the
/// selected subcommand. Returns the process exit code.
pub fn dispatch(argv: Vec<String>) -> i32 {
    let rewritten = rewrite_implicit_args(&argv);
    if std::env::var_os("UPIX_DEBUG_REWRITE").is_some() {
        eprintln!("rewrite: {rewritten:?}");
    }
    // clap's try_parse_from(Vec<String>) consumes vec[0] as the binary name;
    // re-add it so the user words start at index 0 of the parser input.
    let mut parser_input: Vec<String> = Vec::with_capacity(rewritten.len() + 1);
    parser_input.push("upix".to_string());
    parser_input.extend(rewritten);
    let cli = match Cli::try_parse_from(&parser_input) {
        Ok(c) => c,
        Err(e) => {
            // help → stdout, version → stdout, errors → stderr (CLI11's
            // app->exit() split).
            use clap::error::ErrorKind::*;
            let help_or_version = matches!(
                e.kind(),
                DisplayHelp | DisplayVersion | DisplayHelpOnMissingArgumentOrSubcommand
            );
            if help_or_version {
                print!("{}", e.render());
                return 0;
            }
            eprint!("{}", e.render());
            return 2;
        }
    };
    match cli.command {
        None => {
            // The root callback runs cmd_process only when input was given;
            // otherwise main() prints top-level help (app->help()).
            run_upscale(&cli.upscale)
        }
        Some(Command::Model { command }) => match command {
            ModelCommand::List { models_dir, names_only, json } => {
                let a = ModelArgs {
                    subcommand: "list".into(),
                    models_dir: models_dir.unwrap_or_default(),
                    names_only,
                    json,
                    ..Default::default()
                };
                crate::cmd_model::run(&a)
            }
            ModelCommand::ListRemote { repo, json } => {
                let a = ModelArgs {
                    subcommand: "list-remote".into(),
                    repo: repo.unwrap_or_default(),
                    json,
                    ..Default::default()
                };
                crate::cmd_model::run(&a)
            }
            ModelCommand::Download { names, models_dir, repo, json } => {
                let models_dir = models_dir.unwrap_or_default();
                let a = ModelArgs {
                    subcommand: "download".into(),
                    models_dir,
                    repo: repo.unwrap_or_default(),
                    names,
                    json,
                    ..Default::default()
                };
                crate::cmd_model::run(&a)
            }
            ModelCommand::Import { files, models_dir, json } => {
                let models_dir = models_dir.unwrap_or_default();
                let a = ModelArgs {
                    subcommand: "import".into(),
                    models_dir,
                    files,
                    json,
                    ..Default::default()
                };
                crate::cmd_model::run(&a)
            }
        },
        Some(Command::Host { command, start }) => {
            let host_args = |flags: &HostStartFlags| HostArgs {
                subcommand: "start".into(),
                port: flags.port,
                auto_approve: flags.auto_approve,
                models_dir: flags
                    .models_dir
                    .clone()
                    .unwrap_or_else(crate::common::default_models_dir),
                json: flags.json,
            };
            match command {
                Some(HostCommand::Start { flags }) => crate::cmd_host::run(&host_args(&flags)),
                Some(HostCommand::Info { port, json }) => {
                    let a = HostArgs {
                        subcommand: "info".into(),
                        port,
                        json,
                        ..Default::default()
                    };
                    crate::cmd_host::run(&a)
                }
                // Bare `host` (no nested subcommand) → default to start.
                None => crate::cmd_host::run(&host_args(&start)),
            }
        }
        Some(Command::Daemon { command, start }) => {
            let daemon_args = |flags: &DaemonStartFlags| DaemonArgs {
                subcommand: "start".into(),
                model_name: flags.model.clone().unwrap_or_default(),
                models_dir: flags
                    .models_dir
                    .clone()
                    .unwrap_or_else(crate::common::default_models_dir),
                socket_path: flags.socket.clone().unwrap_or_default(),
                force_cpu: flags.cpu,
                foreground: flags.fg,
                idle_timeout_s: flags.idle_timeout,
                json: false,
            };
            match command {
                Some(DaemonCommand::Start { flags }) => crate::cmd_daemon::run(&daemon_args(&flags)),
                Some(DaemonCommand::Stop { socket }) => {
                    let a = DaemonArgs {
                        subcommand: "stop".into(),
                        socket_path: socket.unwrap_or_default(),
                        ..Default::default()
                    };
                    crate::cmd_daemon::run(&a)
                }
                Some(DaemonCommand::Status { socket, json }) => {
                    let a = DaemonArgs {
                        subcommand: "status".into(),
                        socket_path: socket.unwrap_or_default(),
                        json,
                        ..Default::default()
                    };
                    crate::cmd_daemon::run(&a)
                }
                // Bare `daemon` (no nested subcommand) → default to start.
                None => crate::cmd_daemon::run(&daemon_args(&start)),
            }
        }
        Some(Command::Completions { shell, install }) => {
            let a = CompletionsArgs { shell, install };
            crate::cmd_completions::run(&a)
        }
    }
}

/// Run the no-verb upscale path when -i/--input was given on the root.
pub fn run_upscale(up: &UpscaleArgs) -> i32 {
    // The rewrite pre-pass has already turned bare positionals into
    // -i/-o, so only flag forms reach here.
    let a = process_args_from(up);
    if a.input_path.is_empty() {
        // cli_app.cpp: the root callback runs cmd_process only when input
        // was given; otherwise print top-level help and succeed.
        print!("{}", Cli::command().render_help());
        return 0;
    }
    crate::cmd_process::run(&a)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rewrite_no_verb_form() {
        // `upix-cli input.png` → `-i input.png`
        let r = rewrite_implicit_args(&["a.png".to_string()]);
        assert_eq!(r, vec!["-i", "a.png"]);
        // `upix-cli in.png out.png` → `-i in.png -o out.png`
        let r = rewrite_implicit_args(&["a.png".to_string(), "b.png".to_string()]);
        assert_eq!(r, vec!["-i", "a.png", "-o", "b.png"]);
        // `upix-cli -m MODEL` stays flags; a subcommand word wins
        let r = rewrite_implicit_args(&["model".to_string(), "list".to_string()]);
        assert_eq!(r, vec!["model", "list"]);
        // `-i value` consumes its value
        let r = rewrite_implicit_args(&["-i".to_string(), "x.png".to_string()]);
        assert_eq!(r, vec!["-i", "x.png"]);
        // `--model name` consumes; next bare word is still positional
        let r = rewrite_implicit_args(&["--model".to_string(), "m".to_string(), "a.png".to_string()]);
        assert_eq!(r, vec!["--model", "m", "-i", "a.png"]);
    }

    #[test]
    fn dispatch_subcommands_reachable() {
        // clap's try_parse_from(Vec) consumes vec[0] as bin name; dispatch
        // must re-add it or subcommands break (regression test).
        assert_eq!(dispatch(vec!["model".into(), "list".into(), "--json".into()]), 0);
    }

    #[test]
    fn dispatch_version_and_help() {
        assert_eq!(dispatch(vec!["--version".into()]), 0);
        assert_eq!(dispatch(vec!["--help".into()]), 0);
    }

    #[test]
    fn implicit_flag_conflicts() {
        // --gpu + --cpu is rejected (conflict declared in cli_app.cpp)
        let code = dispatch(vec!["-i".into(), "/nonexistent".into(), "--gpu".into(), "--cpu".into()]);
        assert_eq!(code, 2); // clap usage error
    }

    #[test]
    fn completions_all_shells() {
        for sh in ["bash", "zsh", "fish"] {
            assert_eq!(dispatch(vec!["completions".into(), sh.into()]), 0);
        }
    }
}
