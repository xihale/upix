//! POD parameter structs, one per subcommand — Rust port of `cli/src/args.hpp`.
//!
//! The clap definitions in `cli_def.rs` bind into these plain structs (same
//! layering as the C++: the schema lives in the CLI definitions, the business
//! logic in the cmd_* modules takes a plain struct with no CLI dependency).

/// `process` (the implicit root verb). The largest surface.
#[derive(Debug, Clone)]
pub struct ProcessArgs {
    pub input_path: String,
    pub output_path: String,
    pub model_name: String,
    /// Pre-filled from `default_models_dir()` before parsing.
    pub models_dir: String,
    pub config_file: String, // empty = none
    pub tile_size: i32,
    pub tile_overlap: i32,
    pub min_tile: i32,
    pub max_tile: i32,
    pub batch_size: i32,
    pub tile_overlap_explicit: bool,
    pub min_tile_explicit: bool,
    pub max_tile_explicit: bool,
    pub batch_size_explicit: bool,
    pub jobs: i32,
    /// Set when -j/--jobs is given (unset: GPU dir mode sizes workers by VRAM).
    pub jobs_explicit: bool,
    /// -1 = "not set"; downstream treats unset as quality 90
    /// (kDefaultOutputQuality). Help text spells the default out.
    pub output_quality: i32,
    /// empty = auto from extension
    pub output_format: String,
    // Per-format encoder knobs (see upix::EncoderOptions). Same override
    // semantics as output_quality: *_explicit is set when the flag was given,
    // and only explicit flags override the config file. Field defaults equal
    // the EncoderOptions defaults so a bare `upix` run and the config-file
    // path behave identically.
    pub png_level: i32, // -1 = auto (from --output-quality)
    pub png_level_explicit: bool,
    pub jpeg_subsampling: i32, // 420 / 422 / 444
    pub jpeg_subsampling_explicit: bool,
    pub webp_method: i32, // -1 = auto (libwebp default 4)
    pub webp_method_explicit: bool,
    pub webp_lossless: bool, // one-sided flag, like --gpu
    pub webp_lossless_explicit: bool,
    pub avif_speed: i32, // 0-10
    pub avif_speed_explicit: bool,
    pub avif_subsampling: i32, // 420 / 444
    pub avif_subsampling_explicit: bool,
    pub avif_threads: i32, // 0 = auto (hardware concurrency)
    pub avif_threads_explicit: bool,
    pub auto_tile: bool,
    pub auto_tile_explicit: bool,
    pub tile_size_explicit: bool,
    pub use_gpu: bool,
    /// Explicit --cpu (excludes --gpu): CPU-only engine, and no GPU
    /// warm-engine daemon.
    pub use_cpu: bool,
    pub no_daemon: bool,
    /// Re-process inputs carrying an SR marker (default: skipped).
    pub force: bool,
}

impl Default for ProcessArgs {
    fn default() -> Self {
        Self {
            input_path: String::new(),
            output_path: String::new(),
            model_name: String::new(),
            models_dir: crate::common::default_models_dir(),
            config_file: String::new(),
            tile_size: 256,
            tile_overlap: 16,
            min_tile: 64,
            max_tile: 512,
            batch_size: 1,
            tile_overlap_explicit: false,
            min_tile_explicit: false,
            max_tile_explicit: false,
            batch_size_explicit: false,
            jobs: 1,
            jobs_explicit: false,
            output_quality: -1,
            output_format: String::new(),
            png_level: -1,
            png_level_explicit: false,
            jpeg_subsampling: 420,
            jpeg_subsampling_explicit: false,
            webp_method: -1,
            webp_method_explicit: false,
            webp_lossless: false,
            webp_lossless_explicit: false,
            avif_speed: 6,
            avif_speed_explicit: false,
            avif_subsampling: 420,
            avif_subsampling_explicit: false,
            avif_threads: 0,
            avif_threads_explicit: false,
            auto_tile: false,
            auto_tile_explicit: false,
            tile_size_explicit: false,
            use_gpu: false,
            use_cpu: false,
            no_daemon: false,
            force: false,
        }
    }
}

/// `model {list, list-remote, download, import}`. One instance per nested
/// subcommand so each carries its own models_dir/repo.
#[derive(Debug, Clone, Default)]
pub struct ModelArgs {
    /// list / list-remote / download / import
    pub subcommand: String,
    /// Pre-filled from `default_models_dir()` before parsing.
    pub models_dir: String,
    /// "owner/name", empty = default upstream
    pub repo: String,
    /// download: model names from the manifest
    pub names: Vec<String>,
    /// import: local .param/.bin paths
    pub files: Vec<String>,
    /// list: one name per line, no header/sizes (machine-readable; used by
    /// completions)
    pub names_only: bool,
    /// one-shot JSON document / JSON-lines events on stdout (desktop GUI +
    /// script consumers)
    pub json: bool,
}

/// `host {start, info}` — the coordination server phones pair with.
#[derive(Debug, Clone, Default)]
pub struct HostArgs {
    /// start / info (bare `host` = start)
    pub subcommand: String,
    pub port: u16,
    /// start: auto-approve pairing requests (testing/CI)
    pub auto_approve: bool,
    /// Pre-filled from `default_models_dir()` before parsing.
    pub models_dir: String,
    /// JSON events on stdout; logs/prompts to stderr
    pub json: bool,
}

/// `daemon {start, stop, status}` — the local warm-engine daemon.
#[derive(Debug, Clone, Default)]
pub struct DaemonArgs {
    /// start / stop / status (bare `daemon` = start)
    pub subcommand: String,
    pub model_name: String,
    /// Pre-filled from `default_models_dir()` before parsing.
    pub models_dir: String,
    /// empty = default_serve_socket_path()
    pub socket_path: String,
    pub force_cpu: bool,
    pub foreground: bool,
    /// exit after this many idle seconds (0 = never)
    pub idle_timeout_s: i64,
    /// status: one-shot JSON document on stdout
    pub json: bool,
}

/// `completions <bash|zsh|fish>` — emit a shell completion script to stdout,
/// or with -i/--install write it into the shell's user completion directory.
#[derive(Debug, Clone, Default)]
pub struct CompletionsArgs {
    /// bash / zsh / fish
    pub shell: String,
    /// write to the standard dir instead of printing
    pub install: bool,
}
