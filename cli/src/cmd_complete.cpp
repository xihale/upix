// `crossscale __complete WORDS...` — the dynamic shell-completion engine.
//
// Cobra/clap-style protocol: the bash/zsh/fish scripts emitted by
// cmd_completions.cpp are thin adapters that replay the command line being
// completed into this hidden command and print what it returns (one
// "candidate\tdescription" line per candidate). All context logic lives here,
// so the per-shell scripts stay trivial and can never drift:
//
//   - sub-commands and flags come straight from build_app()'s CLI11 tree —
//     an option added in cli_app.cpp completes everywhere automatically;
//   - model names come from ModelManager against the models dir in effect
//     (--models/--models-dir seen on the line wins over $CROSSSCALE_MODELS_DIR);
//   - input-file candidates are filtered to the decoders this binary actually
//     ships: png/jpg/jpeg/webp always, avif only when system libavif loads
//     (avif_decode_available()).
//
// Output protocol: "candidate" or "candidate\tdescription" lines, plus
// display-only hint lines starting with '!' (currently just "!(none)" for an
// input completion with no decodable files here) that the shell adapters print
// instead of offering — see cmd_completions.cpp.
//
// main.cpp dispatches argv[1] == "__complete" here before any CLI11 parsing.
// The last WORD is the partial word under the cursor (may be empty). Output is
// stdout lines only; diagnostics must never reach it, so nothing here logs and
// every filesystem/library probe fails silent.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli_app.hpp"
#include "common.hpp"  // default_models_dir
#include "core/model_manager.hpp"
#include "utils/image_avif.hpp"

namespace fs = std::filesystem;

namespace {

struct Cand {
    std::string value;
    std::string desc;
};

std::vector<Cand> g_out;

void emit(std::string value, std::string desc = {}) {
    g_out.push_back({std::move(value), std::move(desc)});
}

auto lower(std::string s) -> std::string {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ── decoder/format knowledge ────────────────────────────────────────────────

// Input extensions this binary can decode. avif joins only when the system
// libavif loads right now — the "还得看情况" case, resolved at TAB time.
auto image_exts() -> const std::set<std::string>& {
    static const std::set<std::string> exts = [] {
        std::set<std::string> s{"png", "jpg", "jpeg", "webp"};
        if (crossscale::avif_codec::avif_decode_available()) s.insert("avif");
        return s;
    }();
    return exts;
}

void complete_output_formats() {
    emit("png", "lossless");
    emit("jpg", "quality 1-100");
    emit("jpeg", "quality 1-100");
    emit("webp", "quality 1-100");
    if (crossscale::avif_codec::avif_encode_available()) {
        emit("avif", "needs system libavif");
    }
}

// ── file enumeration ────────────────────────────────────────────────────────

// Directory entries matching the typed prefix; directories always offered with
// a trailing '/' so paths stay navigable, files filtered by `exts` (null =
// any file). A leading "~" round-trips so home-relative typing keeps working.
//
// When an extension-filtered completion finds nothing decodable here, an
// '!' hint line ("(none)") is emitted instead of silence — the shell adapters
// print it as a message rather than falling back to listing every unrelated
// file in the folder.
void complete_files(const std::string& typed_prefix, const std::set<std::string>* exts,
                    bool dirs_only = false) {
    std::string real = typed_prefix;
    bool tilde = real.rfind('~', 0) == 0;
    if (tilde) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') return;
        real = std::string(home) + real.substr(1);  // "~/ab" → "$HOME/ab", "~" → "$HOME"
    }

    std::string dir_real, stem;
    const auto slash = real.rfind('/');
    if (slash == std::string::npos) {
        stem = real;
    } else {
        dir_real = real.substr(0, slash + 1);
        stem = real.substr(slash + 1);
    }

    // What to echo back in front of each name: exactly what the user typed,
    // with $HOME mapped back to ~ when applicable.
    std::string shown_dir = dir_real;
    if (tilde && !shown_dir.empty()) {
        const std::string home = std::getenv("HOME");
        if (shown_dir.rfind(home, 0) == 0) shown_dir = "~" + shown_dir.substr(home.size());
    }

    const fs::path dir = dir_real.empty() ? fs::path(".") : fs::path(dir_real);
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    ec.clear();  // unreadable/missing dir → zero hits → the "none here" hint below

    std::vector<Cand> hits;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        ec.clear();
        if (name.rfind(stem, 0) != 0) continue;
        if (stem.empty() && !name.empty() && name[0] == '.') continue;  // hide dotfiles
        const bool is_dir = it->is_directory(ec);
        ec.clear();
        if (!is_dir && !dirs_only && exts != nullptr) {
            std::string ext = lower(p.extension().string());  // ".png"
            if (ext.size() < 2 || exts->count(ext.substr(1)) == 0) continue;
        }
        hits.push_back({shown_dir + name + (is_dir ? "/" : ""), ""});
    }

    // Filtered input completion with no matches: say so ("no" — same wording
    // as `model list`) instead of letting the shell fall back to unrelated
    // filenames. Plain-path completions (output etc.) stay silent — there the
    // shell's own filename fallback is right for free-form typing.
    if (hits.empty() && exts != nullptr && !dirs_only) {
        emit("!(none)");
        return;
    }

    std::sort(hits.begin(), hits.end(),
              [](const Cand& a, const Cand& b) { return a.value < b.value; });
    for (Cand& c : hits) g_out.push_back(std::move(c));
}

// ── model names ─────────────────────────────────────────────────────────────

// Installed models for -m/--model. Pre-checking the directory keeps
// ModelManager's stdout warning out of the candidate stream.
void complete_models(const std::string& models_dir_override) {
    const fs::path dir = models_dir_override.empty()
                             ? fs::path(default_models_dir())
                             : fs::path(models_dir_override);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    crossscale::ModelManager manager(dir);
    auto models = manager.scan_models();
    if (!models.has_value()) return;
    for (const auto& m : *models) {
        emit(m.name, m.scale_factor > 0 ? std::to_string(m.scale_factor) + "x" : "");
    }
}

// ── CLI11 tree walking ──────────────────────────────────────────────────────

bool opt_is(const CLI::Option* o, std::initializer_list<const char*> longs,
            std::initializer_list<const char*> shorts = {}) {
    if (o == nullptr) return false;
    for (const char* l : longs)
        if (o->check_lname(l)) return true;
    for (const char* s : shorts)
        if (o->check_sname(s)) return true;
    return false;
}

// Flags consume no value token; everything registered via add_option does
// (including ->expected(0,1) optionals like input/output).
bool takes_value(const CLI::Option* o) { return o->get_expected_max() >= 1; }

const CLI::Option* find_option(const CLI::App* node, const std::string& tok) {
    const auto eq = tok.find('=');
    const std::string t = tok.substr(0, eq);
    if (t.rfind("--", 0) == 0 && t.size() > 2) {
        const std::string lname = t.substr(2);
        for (const CLI::Option* o : node->get_options())
            if (o->check_lname(lname)) return o;
    } else if (t.size() >= 2 && t[0] == '-') {
        const std::string sname = t.substr(1);
        for (const CLI::Option* o : node->get_options())
            if (o->check_sname(sname)) return o;
    }
    return nullptr;
}

const CLI::App* find_subcommand(const CLI::App* node, const std::string& name) {
    for (const CLI::App* s :
         node->get_subcommands([](const CLI::App*) { return true; })) {
        if (s->get_name() == name) return s;
    }
    return nullptr;
}

void complete_subcommands(const CLI::App* node, const std::string& cur) {
    for (const CLI::App* s :
         node->get_subcommands([](const CLI::App*) { return true; })) {
        // group("") hides a subcommand from help; keep it out of completions too.
        if (s->get_group().empty() || s->get_name().empty()) continue;
        if (cur.empty() || s->get_name().rfind(cur, 0) == 0) {
            emit(s->get_name(), s->get_description());
        }
    }
}

void complete_option_names(const CLI::App* node, const std::string& cur) {
    for (const CLI::Option* o : node->get_options()) {
        if (o->get_group().empty()) continue;
        const std::string all = o->get_name(false, true);  // "-i,--input"
        for (size_t start = 0; start < all.size();) {
            const size_t comma = all.find(',', start);
            const std::string name =
                all.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!name.empty() && name[0] == '-' && name.rfind(cur, 0) == 0) {
                emit(name, o->get_description());
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
}

// Value completion for the option whose value is being typed.
void complete_option_value(const CLI::Option* o, const std::string& cur,
                           const std::string& models_dir_override) {
    if (opt_is(o, {"model"}, {"m"})) {
        complete_models(models_dir_override);
    } else if (opt_is(o, {"input"}, {"i"})) {
        complete_files(cur, &image_exts());
    } else if (opt_is(o, {"models", "models-dir"})) {
        complete_files(cur, nullptr, /*dirs_only=*/true);
    } else if (opt_is(o, {"output-format"})) {
        complete_output_formats();
    } else if (opt_is(o, {"output", "out", "config", "socket"}, {"o"})) {
        complete_files(cur, nullptr);  // plain paths: let every entry through
    }
    // Free-form values (ports, tile sizes, durations, repo, …): no candidates.
}

int cmd_complete_impl(int argc, char* argv[]) {
    std::vector<std::string> words(argv, argv + argc);
    const std::string cur = words.empty() ? std::string{} : words.back();
    if (!words.empty()) words.pop_back();

    auto app_holder = build_app();
    const CLI::App* root = app_holder.get();

    const CLI::App* node = root;          // deepest subcommand reached
    const CLI::App* parent = nullptr;     // one level up (for model import/download)
    const CLI::Option* pending = nullptr; // option awaiting its value
    int positionals = 0;                  // positional words at the current node
    bool only_positional = false;         // seen "--"
    std::string models_dir_override;

    for (const std::string& tok : words) {
        if (pending != nullptr) {
            if (opt_is(pending, {"models", "models-dir"}) && !tok.empty()) {
                models_dir_override = tok;
            }
            pending = nullptr;
            continue;
        }
        if (!only_positional && tok == "--") {
            only_positional = true;
            continue;
        }
        if (!only_positional && tok.size() >= 2 && tok[0] == '-') {
            const CLI::Option* o = find_option(node, tok);
            if (o != nullptr && takes_value(o)) {
                const auto eq = tok.find('=');
                if (eq == std::string::npos) {
                    pending = o;  // bare "--x": value is the next word
                } else if (opt_is(o, {"models", "models-dir"})) {
                    models_dir_override = tok.substr(eq + 1);  // inline "--x=v"
                }
            }
            continue;
        }
        if (const CLI::App* sub = find_subcommand(node, tok)) {
            parent = node;
            node = sub;
            positionals = 0;
        } else {
            ++positionals;
        }
    }

    if (pending != nullptr) {
        complete_option_value(pending, cur, models_dir_override);
    } else if (!only_positional && !cur.empty() && cur[0] == '-') {
        complete_option_names(node, cur);
    } else if (node == root) {
        if (positionals == 0) {
            // Root, first word: sub-command or input image (`crossscale img.png`
            // upscales directly).
            complete_subcommands(root, cur);
            complete_files(cur, &image_exts());
        } else {
            complete_files(cur, nullptr);  // later root positionals: output path
        }
    } else if (node->get_name() == "completions") {
        if (positionals == 0) {
            emit("bash"); emit("zsh"); emit("fish");
        }
    } else if (node->get_name() == "import" && parent != nullptr &&
               parent->get_name() == "model") {
        static const std::set<std::string> ncnn_exts{"param", "bin"};
        complete_files(cur, &ncnn_exts);
    } else if (positionals == 0) {
        // Nested verb slots: model/perf/host/daemon sub-subcommands.
        complete_subcommands(node, cur);
    }
    // Everything else (daemon/host/perf verbs, `model download`'s remote
    // catalog): no candidates rather than misleading ones.

    for (const Cand& c : g_out) {
        std::cout << c.value;
        if (!c.desc.empty()) std::cout << '\t' << c.desc;
        std::cout << '\n';
    }
    return 0;
}

}  // namespace

int cmd_complete(int argc, char* argv[]) {
    try {
        return cmd_complete_impl(argc, argv);
    } catch (...) {
        return 0;  // a completion query must never surface an error
    }
}
