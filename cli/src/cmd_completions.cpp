// `crossscale completions <bash|zsh|fish>` — emit a completion script.
//
// The scripts complete sub-commands, per-subcommand flags, and model names
// for the value-taking --model/-m options. Model names are dynamic: the
// scripts shell out to `crossscale model list --names-only` (machine-readable,
// env-aware models dir), so they always reflect what is installed.
//
// Install commands live in README.md § Shell Completions and in the header
// comment of each script. `completions <shell> --install` writes them into
// the shell's user completion directory automatically. All three resolve the
// binary from the command line being completed ($1 / words[1] / commandline),
// so they work for `crossscale` and `crossscale-cli` alike.
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

constexpr const char* BASH = R"BASH(# bash completion for crossscale.
# Install:
#   crossscale completions bash > ~/.local/share/bash-completion/completions/crossscale
# (older distros: /etc/bash_completion.d/crossscale — name the file after the
#  command you actually invoke, e.g. crossscale-cli)

# Options that consume a value (their value must not be mistaken for a
# sub-command word while scanning the command line).
__crossscale_takes_value() {
    case "$1" in
        -i|--input|-o|--output|-m|--model|--models|--models-dir|--repo|\
        --tile-size|--tile-overlap|--batch-size|--min-tile|--max-tile|\
        -j|--jobs|--output-quality|--output-format|--config|--tile|\
        --duration|--warmup|--sample-ms|--out|--concurrency|-p|--port|\
        --socket|--idle-timeout) return 0 ;;
    esac
    return 1
}

_crossscale() {
    local cur prev sub nested i w skip
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Option values: complete where we can, else leave COMPREPLY unset so
    # bash falls back to its default (file names).
    if __crossscale_takes_value "$prev"; then
        case "$prev" in
            -m|--model)
                COMPREPLY=($(compgen -W "$("$1" model list --names-only 2>/dev/null)" -- "$cur"))
                ;;
            --models|--models-dir)
                COMPREPLY=($(compgen -d -- "$cur"))
                ;;
            --config|--socket|--out)
                COMPREPLY=($(compgen -f -- "$cur"))
                ;;
        esac
        return 0
    fi

    # Scan for the sub-command and nested sub-command words so far.
    sub=""; nested=""; skip=0
    for ((i = 1; i < COMP_CWORD; i++)); do
        w="${COMP_WORDS[i]}"
        if [[ $skip -eq 1 ]]; then skip=0
        elif [[ "$w" == -* ]]; then
            __crossscale_takes_value "$w" && skip=1
        elif [[ -z "$sub" ]]; then sub="$w"
        elif [[ -z "$nested" ]]; then nested="$w"
        fi
    done

    # Option names for the current (sub)command.
    if [[ "$cur" == -* ]]; then
        local opts="-h --help"
        case "$sub" in
            process) opts="$opts -i --input -o --output -m --model --models --tile-size --tile-overlap --batch-size --auto-tile --min-tile --max-tile -j --jobs --gpu --no-daemon --output-quality --output-format --config" ;;
            model)   opts="$opts --models-dir --repo --names-only" ;;
            perf)    opts="$opts --model --models-dir --tile --duration --warmup --sample-ms --out --concurrency --no-cpu-baseline" ;;
            host)    opts="$opts -p --port --models-dir --auto-approve" ;;
            daemon)  opts="$opts --model --models-dir --socket --cpu --fg --idle-timeout" ;;
            "")      opts="$opts -V --version" ;;
        esac
        COMPREPLY=($(compgen -W "$opts" -- "$cur"))
        return 0
    fi

    case "$sub" in
        "")
            # Root: a sub-command or an input image (implicit `process`).
            COMPREPLY=($(compgen -W "process model perf host daemon completions" -- "$cur") $(compgen -f -- "$cur"))
            ;;
        model)
            [[ -n "$nested" ]] && return 0
            COMPREPLY=($(compgen -W "list list-remote download import" -- "$cur"))
            ;;
        perf)
            [[ -n "$nested" ]] && return 0
            COMPREPLY=($(compgen -W "list-probes gpu-efficiency gpu-concurrency gpu-reuse" -- "$cur"))
            ;;
        host)
            [[ -n "$nested" ]] && return 0
            COMPREPLY=($(compgen -W "start info" -- "$cur"))
            ;;
        daemon)
            [[ -n "$nested" ]] && return 0
            COMPREPLY=($(compgen -W "start stop status" -- "$cur"))
            ;;
        completions)
            COMPREPLY=($(compgen -W "bash zsh fish" -- "$cur"))
            ;;
    esac
    return 0
}
complete -F _crossscale crossscale crossscale-cli
)BASH";

constexpr const char* ZSH = R"ZSH(#compdef crossscale crossscale-cli
# zsh completion for crossscale.
# Install:
#   mkdir -p ~/.zfunc
#   crossscale completions zsh > ~/.zfunc/_crossscale
# and in ~/.zshrc, before `autoload -U compinit && compinit`:
#   fpath=(~/.zfunc $fpath)

_crossscale() {
    local -a models
    models=(${(f)"$(command "${words[1]}" model list --names-only 2>/dev/null)"})

    local -a commands
    commands=(
        'process:upscale an image or directory'
        'model:manage models (list, download, import)'
        'perf:GPU/CPU performance probes'
        'host:coordinator for phones'
        'daemon:manage the warm-engine daemon'
        'completions:emit shell completion scripts'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[show help]' \
        '(-V --version)'{-V,--version}'[show version]' \
        '1: :->top' \
        '*::: ->args'

    case $state in
        top)
            # Root takes a sub-command or an input image (implicit `process`).
            _describe -t commands 'crossscale command' commands
            _files
            ;;
        args)
            case $words[1] in
                process)
                    _arguments \
                        '(-i --input)'{-i,--input}'[input image or directory]:file:_files' \
                        '(-o --output)'{-o,--output}'[output path (default: derived from input + model)]:file:_files' \
                        '(-m --model)'{-m,--model}'[model name]:model:($models)' \
                        '--models[models directory]:directory:_files -/' \
                        '--tile-size[tile size]:size:(64 128 192 256 384 512)' \
                        '--tile-overlap[tile overlap]:overlap:(0 8 16 24 32)' \
                        '--batch-size[tile batch size]:size:(1 2 4 8 16)' \
                        '--auto-tile[auto-select tile size from GPU VRAM]' \
                        '--min-tile[minimum auto-tile size]:size:' \
                        '--max-tile[maximum auto-tile size]:size:' \
                        '(-j --jobs)'{-j,--jobs}'[parallel workers for directory mode]:count:' \
                        '--gpu[force GPU for directory mode]' \
                        '--no-daemon[never use the warm-engine daemon]' \
                        '--output-quality[output quality 0-100]:quality:' \
                        '--output-format[output format]:format:(png jpg jpeg webp avif)' \
                        '--config[config file]:file:_files' \
                        '1:input file:_files' \
                        '2:output file:_files'
                    ;;
                model)
                    local -a model_commands
                    model_commands=(
                        'list:list installed models'
                        'list-remote:list models in the GitHub release'
                        'download:download model(s) from the release'
                        'import:import local .param/.bin files'
                    )
                    _arguments -C \
                        '1: :->sub' \
                        '*::: ->subargs'
                    case $state in
                        sub)
                            _describe -t model-commands 'model subcommand' model_commands
                            ;;
                        subargs)
                            case $words[1] in
                                list)
                                    _arguments \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '--names-only[print names only, one per line]'
                                    ;;
                                list-remote)
                                    _arguments '--repo[GitHub repo owner/name]:repo:'
                                    ;;
                                download)
                                    _arguments \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '--repo[GitHub repo owner/name]:repo:' \
                                        '*:model name:'
                                    ;;
                                import)
                                    _arguments \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '*:param/bin file:_files'
                                    ;;
                            esac
                            ;;
                    esac
                    ;;
                perf)
                    local -a perf_commands
                    perf_commands=(
                        'list-probes:list available perf probes'
                        'gpu-efficiency:measure GPU vs CPU tile throughput'
                        'gpu-concurrency:scaling across N concurrent engines'
                        'gpu-reuse:fresh vs reused VkCompute per tile'
                    )
                    _arguments -C \
                        '1: :->sub' \
                        '*::: ->subargs'
                    case $state in
                        sub)
                            _describe -t perf-commands 'perf subcommand' perf_commands
                            ;;
                        subargs)
                            case $words[1] in
                                gpu-efficiency)
                                    _arguments \
                                        '--model[model name]:model:($models)' \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '--tile[tile size]:size:' \
                                        '--duration[duration (e.g. 5s)]:duration:' \
                                        '--warmup[warmup (e.g. 1s)]:warmup:' \
                                        '--sample-ms[sample interval ms]:ms:' \
                                        '--no-cpu-baseline[skip the CPU baseline]' \
                                        '--out[output file (default: stdout)]:file:_files'
                                    ;;
                                gpu-concurrency|gpu-reuse)
                                    _arguments \
                                        '--model[model name]:model:($models)' \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '--tile[tile size]:size:' \
                                        '--concurrency[concurrent engines]:count:' \
                                        '--duration[duration (e.g. 6s)]:duration:' \
                                        '--warmup[warmup (e.g. 2s)]:warmup:' \
                                        '--sample-ms[sample interval ms]:ms:' \
                                        '--out[output file (default: stdout)]:file:_files'
                                    ;;
                            esac
                            ;;
                    esac
                    ;;
                host)
                    local -a host_commands
                    host_commands=(
                        'start:start the coordinator (default)'
                        'info:print the connection card (IP/QR/key)'
                    )
                    _arguments -C \
                        '1: :->sub' \
                        '*::: ->subargs'
                    case $state in
                        sub)
                            _describe -t host-commands 'host subcommand' host_commands
                            ;;
                        subargs)
                            case $words[1] in
                                start)
                                    _arguments \
                                        '(-p --port)'{-p,--port}'[port]:port:' \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '--auto-approve[auto-approve pairing requests (CI)]'
                                    ;;
                                info)
                                    _arguments '(-p --port)'{-p,--port}'[port]:port:'
                                    ;;
                            esac
                            ;;
                    esac
                    ;;
                daemon)
                    local -a daemon_commands
                    daemon_commands=(
                        'start:start the daemon (default)'
                        'stop:stop the daemon'
                        'status:report whether the daemon is running'
                    )
                    _arguments -C \
                        '1: :->sub' \
                        '*::: ->subargs'
                    case $state in
                        sub)
                            _describe -t daemon-commands 'daemon subcommand' daemon_commands
                            ;;
                        subargs)
                            case $words[1] in
                                start)
                                    _arguments \
                                        '--model[model name]:model:($models)' \
                                        '--models-dir[models directory]:directory:_files -/' \
                                        '--socket[unix socket path]:file:_files' \
                                        '--cpu[force CPU]' \
                                        '--fg[run in foreground]' \
                                        '--idle-timeout[idle seconds before exit (0 = never)]:seconds:'
                                    ;;
                                stop|status)
                                    _arguments '--socket[unix socket path]:file:_files'
                                    ;;
                            esac
                            ;;
                    esac
                    ;;
                completions)
                    _arguments '1:shell:(bash zsh fish)'
                    ;;
            esac
            ;;
    esac
}

_crossscale "$@"
)ZSH";

constexpr const char* FISH = R"FISH(# fish completion for crossscale.
# Install (name the file after the command you invoke):
#   crossscale completions fish > ~/.config/fish/completions/crossscale.fish
#   crossscale completions fish > ~/.config/fish/completions/crossscale-cli.fish

# Register each rule for both invocation names.
function __cs
    complete -c crossscale $argv
    complete -c crossscale-cli $argv
end

function __crossscale_models
    set -l tokens (commandline -op)
    set -l bin $tokens[1]
    command $bin model list --names-only 2>/dev/null
end

function __crossscale_no_sub
    not __fish_seen_subcommand_from process model perf host daemon completions
end

__cs -s h -l help -d "show help"
__cs -s V -l version -d "show version"

# Root: a sub-command or an input image (implicit `process`); fish keeps its
# default file completion alive next to these words.
__cs -n __crossscale_no_sub -a "process\t'upscale an image or directory' model\t'manage models (list, download, import)' perf\t'GPU/CPU performance probes' host\t'coordinator for phones' daemon\t'manage the warm-engine daemon' completions\t'emit shell completion scripts'"

# process
__cs -n '__fish_seen_subcommand_from process' -s i -l input -r -d "input image or directory"
__cs -n '__fish_seen_subcommand_from process' -s o -l output -r -d "output path (default: derived from input + model)"
__cs -n '__fish_seen_subcommand_from process' -s m -l model -x -a "(__crossscale_models)" -d "model name"
__cs -n '__fish_seen_subcommand_from process' -l models -x -a "(__fish_complete_directories)" -d "models directory"
__cs -n '__fish_seen_subcommand_from process' -l tile-size -x -a "64 128 192 256 384 512" -d "tile size"
__cs -n '__fish_seen_subcommand_from process' -l tile-overlap -x -a "0 8 16 24 32" -d "tile overlap"
__cs -n '__fish_seen_subcommand_from process' -l batch-size -x -a "1 2 4 8 16" -d "tile batch size"
__cs -n '__fish_seen_subcommand_from process' -l auto-tile -d "auto-select tile size from GPU VRAM"
__cs -n '__fish_seen_subcommand_from process' -l min-tile -x -d "minimum auto-tile size"
__cs -n '__fish_seen_subcommand_from process' -l max-tile -x -d "maximum auto-tile size"
__cs -n '__fish_seen_subcommand_from process' -s j -l jobs -x -d "parallel workers for directory mode"
__cs -n '__fish_seen_subcommand_from process' -l gpu -d "force GPU for directory mode"
__cs -n '__fish_seen_subcommand_from process' -l no-daemon -d "never use the warm-engine daemon"
__cs -n '__fish_seen_subcommand_from process' -l output-quality -x -d "output quality 0-100"
__cs -n '__fish_seen_subcommand_from process' -l output-format -x -a "png jpg jpeg webp avif" -d "output format"
__cs -n '__fish_seen_subcommand_from process' -l config -r -d "config file"

# model
__cs -f -n '__fish_seen_subcommand_from model; and not __fish_seen_subcommand_from list list-remote download import' -a "list\t'list installed models' list-remote\t'list models in the GitHub release' download\t'download model(s) from the release' import\t'import local .param/.bin files'"
__cs -n '__fish_seen_subcommand_from model' -l models-dir -x -a "(__fish_complete_directories)" -d "models directory"
__cs -n '__fish_seen_subcommand_from model' -l repo -x -d "GitHub repo owner/name"
__cs -n '__fish_seen_subcommand_from model; and __fish_seen_subcommand_from list' -l names-only -d "print names only, one per line"

# perf
__cs -f -n '__fish_seen_subcommand_from perf; and not __fish_seen_subcommand_from list-probes gpu-efficiency gpu-concurrency gpu-reuse' -a "list-probes\t'list available perf probes' gpu-efficiency\t'measure GPU vs CPU tile throughput' gpu-concurrency\t'scaling across N concurrent engines' gpu-reuse\t'fresh vs reused VkCompute per tile'"
__cs -n '__fish_seen_subcommand_from perf' -l model -x -a "(__crossscale_models)" -d "model name"
__cs -n '__fish_seen_subcommand_from perf' -l models-dir -x -a "(__fish_complete_directories)" -d "models directory"
__cs -n '__fish_seen_subcommand_from perf' -l tile -x -d "tile size"
__cs -n '__fish_seen_subcommand_from perf' -l duration -x -d "duration (e.g. 5s)"
__cs -n '__fish_seen_subcommand_from perf' -l warmup -x -d "warmup (e.g. 1s)"
__cs -n '__fish_seen_subcommand_from perf' -l sample-ms -x -d "sample interval ms"
__cs -n '__fish_seen_subcommand_from perf' -l out -r -d "output file (default: stdout)"
__cs -n '__fish_seen_subcommand_from perf; and __fish_seen_subcommand_from gpu-concurrency gpu-reuse' -l concurrency -x -d "concurrent engines"
__cs -n '__fish_seen_subcommand_from perf; and __fish_seen_subcommand_from gpu-efficiency' -l no-cpu-baseline -d "skip the CPU baseline"

# host
__cs -f -n '__fish_seen_subcommand_from host; and not __fish_seen_subcommand_from start info' -a "start\t'start the coordinator (default)' info\t'print the connection card (IP/QR/key)'"
__cs -n '__fish_seen_subcommand_from host' -s p -l port -x -d "port"
__cs -n '__fish_seen_subcommand_from host; and not __fish_seen_subcommand_from info' -l models-dir -x -a "(__fish_complete_directories)" -d "models directory"
__cs -n '__fish_seen_subcommand_from host; and not __fish_seen_subcommand_from info' -l auto-approve -d "auto-approve pairing requests (CI)"

# daemon
__cs -f -n '__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from start stop status' -a "start\t'start the daemon (default)' stop\t'stop the daemon' status\t'report whether the daemon is running'"
__cs -n '__fish_seen_subcommand_from daemon' -l socket -r -d "unix socket path"
__cs -n '__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from stop status' -l model -x -a "(__crossscale_models)" -d "model name"
__cs -n '__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from stop status' -l models-dir -x -a "(__fish_complete_directories)" -d "models directory"
__cs -n '__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from stop status' -l cpu -d "force CPU"
__cs -n '__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from stop status' -l fg -d "run in foreground"
__cs -n '__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from stop status' -l idle-timeout -x -d "idle seconds before exit (0 = never)"

# completions
__cs -f -n '__fish_seen_subcommand_from completions' -x -a "bash zsh fish"
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
