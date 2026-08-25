// `crossscale completions <bash|zsh|fish>` — emit a completion script.
//
// The scripts are thin adapters over the binary's own completion engine
// (cmd_complete.cpp, cobra/clap-style): on every TAB they replay the words
// being completed into `<binary> __complete WORDS...` and print the returned
// candidates. All context lives there — sub-commands and flags come from the
// CLI11 definitions in cli_app.cpp, model names from ModelManager, and input
// files are filtered to decoders the running binary actually ships (avif only
// when system libavif loads). Nothing is duplicated here anymore.
//
// `completions <shell> -i/--install` writes them into the shell's user
// completion directory (see README.md § Shell Completions). All three resolve
// the binary from the command line being completed ($1 / words[1] / the fish
// command line), so `crossscale` and `crossscale-cli` both work.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "args.hpp"
#include "cli_app.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* BASH = R"BASH(# bash completion for crossscale — thin client over `__complete`.
# Install: crossscale completions bash --install

_crossscale() {
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
complete -o filenames -o default -F _crossscale crossscale crossscale-cli
)BASH";

constexpr const char* ZSH = R"ZSH(#compdef crossscale crossscale-cli
# zsh completion for crossscale — thin client over `__complete`.
# Install: crossscale completions zsh --install

_crossscale() {
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
_crossscale "$@"
)ZSH";

constexpr const char* FISH = R"FISH(# fish completion for crossscale — thin client over `__complete`.
# Install: crossscale completions fish --install

function __cs_complete
    set -l tokens (commandline -opc) (commandline -ct)
    command $tokens[1] __complete $tokens[2..-1] 2>/dev/null
end

# One rule each; the engine owns all context. -f keeps fish's own file
# completion out of the way (the engine emits file candidates itself).
complete -c crossscale     -f -a '(__cs_complete)'
complete -c crossscale-cli -f -a '(__cs_complete)'
)FISH";

}  // namespace

namespace {

auto home_dir() -> std::string {
    if (const char* h = std::getenv("HOME")) return h;
    return {};
}

// $XDG_DATA_HOME / $XDG_CONFIG_HOME aware: the env dir is already the root
// (e.g. ~/.local/share), so the suffix differs between the two branches —
// without the env var the default root is ~/.local/share or ~/.config.
auto user_dir(const char* xdg_var, const std::string& xdg_suffix,
              const std::string& default_suffix) -> fs::path {
    if (const char* x = std::getenv(xdg_var)) {
        if (*x) return fs::path(x) / xdg_suffix;
    }
    return fs::path(home_dir()) / default_suffix;
}

auto write_to(const fs::path& path, std::string_view text) -> bool {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

// zsh has no guaranteed user dir on fpath. Prefer one that already exists
// (oh-my-zsh's, or the common ~/.zfunc / ~/.zsh/completions); else fall back
// to ~/.zfunc and have the caller print the fpath line to add.
auto zsh_completion_dir() -> std::pair<fs::path, bool> {
    const std::string home = home_dir();
    if (!home.empty()) {
        std::error_code ec;
        if (fs::exists(fs::path(home) / ".oh-my-zsh", ec)) {
            return {fs::path(home) / ".oh-my-zsh/completions", false};
        }
        for (const fs::path& cand : {fs::path(home) / ".zfunc",
                                     fs::path(home) / ".zsh/completions"}) {
            if (fs::exists(cand, ec)) return {cand, false};
        }
    }
    return {fs::path(home) / ".zfunc", true};  // needs the fpath hint below
}

// Write `text` to `path`, report, and keep going. Returns false on failure.
auto report_write(const fs::path& path, std::string_view text) -> bool {
    if (!write_to(path, text)) {
        std::cerr << "completions: failed to write " << path.string() << "\n";
        return false;
    }
    std::cout << "Installed: " << path.string() << "\n";
    return true;
}

auto install_completions(const std::string& shell) -> int {
    if (home_dir().empty()) {
        std::cerr << "completions: --install needs $HOME\n";
        return 1;
    }
    // bash and fish autoload by file name, so write one copy per supported
    // invocation name. zsh resolves via the #compdef line — one file suffices.
    if (shell == "bash") {
        const fs::path dir = user_dir("XDG_DATA_HOME", "bash-completion/completions",
                                      ".local/share/bash-completion/completions");
        for (const char* name : {"crossscale", "crossscale-cli"}) {
            if (!report_write(dir / name, BASH)) return 1;
        }
        std::cout << "Restart your shell to pick it up.\n";
    } else if (shell == "zsh") {
        const auto [dir, needs_fpath] = zsh_completion_dir();
        if (!report_write(dir / "_crossscale", ZSH)) return 1;
        if (needs_fpath) {
            std::cout << "Add this to ~/.zshrc (before compinit), then restart zsh:\n"
                      << "  fpath=(" << dir.string() << " $fpath)\n";
        } else {
            std::cout << "Restart zsh (or rerun compinit) to pick it up.\n";
        }
    } else {
        const fs::path dir = user_dir("XDG_CONFIG_HOME", "fish/completions",
                                      ".config/fish/completions");
        for (const char* name : {"crossscale", "crossscale-cli"}) {
            if (!report_write(dir / (std::string(name) + ".fish"), FISH)) return 1;
        }
        std::cout << "New fish sessions pick it up automatically.\n";
    }
    return 0;
}

}  // namespace

int cmd_completions(const args::Completions& a) {
    if (a.install) return install_completions(a.shell);
    if (a.shell == "bash") {
        std::cout << BASH;
    } else if (a.shell == "zsh") {
        std::cout << ZSH;
    } else {
        std::cout << FISH;
    }
    std::cout << "\n";
    return 0;
}