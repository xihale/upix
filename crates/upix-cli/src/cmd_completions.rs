//! `upix completions <bash|zsh|fish>` — emit a completion script.
//!
//! The scripts are thin adapters over the binary's own completion engine
//! (cmd_complete.rs, cobra/clap-style): on every TAB they replay the words
//! being completed into `<binary> __complete WORDS...` and print the returned
//! candidates. All context lives there — sub-commands and flags come from the
//! CLI definitions, model names from ModelManager, and input files are
//! filtered to decoders the running binary actually ships (avif only when
//! system libavif loads). Nothing is duplicated here anymore.
//!
//! `completions <shell> -i/--install` writes them into the shell's user
//! completion directory (see README.md § Shell Completions). All three resolve
//! the binary from the command line being completed ($1 / words[1] / the fish
//! command line), so `upix` and `upix-cli` both work.

use std::path::PathBuf;

use crate::args::CompletionsArgs;

const BASH: &str = r#"# bash completion for upix — thin client over `__complete`.
# Install: upix completions bash --install

_upix() {
    local -a lines
    mapfile -t lines < <("$1" __complete "${COMP_WORDS[@]:1}" 2>/dev/null)
    COMPREPLY=()
    local line
    for line in "${lines[@]}"; do
        COMPREPLY+=("${line%%$'\t'*}")
    done
}
# -o filenames: proper quoting/suffix for our file candidates;
# -o default: fall back to readline's filename completion when the engine
# returns nothing (free-form values like ports or tile sizes).
complete -o filenames -o default -F _upix upix upix-cli
"#;

const ZSH: &str = r#"#compdef upix upix-cli
# zsh completion for upix — thin client over `__complete`.
# Install: upix completions zsh --install

_upix() {
    local -a lines vals disps
    lines=("${(f)$($words[1] __complete "${(@)words[2,-1]}" 2>/dev/null)}")
    local line
    for line in "${lines[@]}"; do
        vals+=("${line%%$'\t'*}")
        disps+=("${line/$'\t'/ -- }")
    done
    if (( ${#vals} )); then
        compadd -d disps -- "${vals[@]}"
    else
        _files   # engine had nothing: free-form value, default to filenames
    fi
}
_upix "$@"
"#;

const FISH: &str = r#"# fish completion for upix — thin client over `__complete`.
# Install: upix completions fish --install

function __cs_complete
    set -l tokens (commandline -opc) (commandline -ct)
    command $tokens[1] __complete $tokens[2..-1] 2>/dev/null
end

# One rule each; the engine owns all context. -f keeps fish's own file
# completion out of the way (the engine emits file candidates itself).
complete -c upix     -f -a '(__cs_complete)'
complete -c upix-cli -f -a '(__cs_complete)'
"#;

fn home_dir() -> String {
    std::env::var("HOME").unwrap_or_default()
}

/// $XDG_DATA_HOME / $XDG_CONFIG_HOME aware: the env dir is already the root
/// (e.g. ~/.local/share), so the suffix differs between the two branches —
/// without the env var the default root is ~/.local/share or ~/.config.
fn user_dir(xdg_var: &str, xdg_suffix: &str, default_suffix: &str) -> PathBuf {
    if let Ok(x) = std::env::var(xdg_var) {
        if !x.is_empty() {
            return PathBuf::from(x).join(xdg_suffix);
        }
    }
    PathBuf::from(home_dir()).join(default_suffix)
}

fn write_to(path: &PathBuf, text: &str) -> bool {
    if let Some(parent) = path.parent() {
        if std::fs::create_dir_all(parent).is_err() {
            return false;
        }
    }
    std::fs::write(path, text).is_ok()
}

/// zsh has no guaranteed user dir on fpath. Prefer one that already exists
/// (oh-my-zsh's, or the common ~/.zfunc / ~/.zsh/completions); else fall back
/// to ~/.zfunc and have the caller print the fpath line to add.
fn zsh_completion_dir() -> (PathBuf, bool) {
    let home = home_dir();
    if !home.is_empty() {
        if PathBuf::from(&home).join(".oh-my-zsh").exists() {
            return (PathBuf::from(&home).join(".oh-my-zsh/completions"), false);
        }
        for cand in [
            PathBuf::from(&home).join(".zfunc"),
            PathBuf::from(&home).join(".zsh/completions"),
        ] {
            if cand.exists() {
                return (cand, false);
            }
        }
    }
    (PathBuf::from(home).join(".zfunc"), true) // needs the fpath hint below
}

/// Write `text` to `path`, report, and keep going. Returns false on failure.
fn report_write(path: &PathBuf, text: &str) -> bool {
    if !write_to(path, text) {
        eprintln!("completions: failed to write {}", path.display());
        return false;
    }
    println!("Installed: {}", path.display());
    true
}

fn install_completions(shell: &str) -> i32 {
    if home_dir().is_empty() {
        eprintln!("completions: --install needs $HOME");
        return 1;
    }
    // bash and fish autoload by file name, so write one copy per supported
    // invocation name. zsh resolves via the #compdef line — one file suffices.
    if shell == "bash" {
        let dir = user_dir(
            "XDG_DATA_HOME",
            "bash-completion/completions",
            ".local/share/bash-completion/completions",
        );
        for name in ["upix", "upix-cli"] {
            if !report_write(&dir.join(name), BASH) {
                return 1;
            }
        }
        println!("Restart your shell to pick it up.");
    } else if shell == "zsh" {
        let (dir, needs_fpath) = zsh_completion_dir();
        if !report_write(&dir.join("_upix"), ZSH) {
            return 1;
        }
        if needs_fpath {
            println!(
                "Add this to ~/.zshrc (before compinit), then restart zsh:\n  fpath=({} $fpath)",
                dir.display()
            );
        } else {
            println!("Restart zsh (or rerun compinit) to pick it up.");
        }
    } else {
        let dir = user_dir(
            "XDG_CONFIG_HOME",
            "fish/completions",
            ".config/fish/completions",
        );
        for name in ["upix", "upix-cli"] {
            if !report_write(&dir.join(format!("{name}.fish")), FISH) {
                return 1;
            }
        }
        println!("New fish sessions pick it up automatically.");
    }
    0
}

pub fn run(a: &CompletionsArgs) -> i32 {
    if a.install {
        return install_completions(&a.shell);
    }
    if a.shell == "bash" {
        print!("{BASH}");
    } else if a.shell == "zsh" {
        print!("{ZSH}");
    } else {
        print!("{FISH}");
    }
    println!();
    0
}
