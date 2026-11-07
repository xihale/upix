// `upix model` — local catalog + GitHub release model management.
//
//   model list                     scan the models dir, print name/scale/size
//   model list-remote [--repo R]   fetch the release manifest, print catalog
//   model download NAME [...]      download model(s) into the models dir
//   model import FILE [...]        copy local .param/.bin pairs into the dir
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <vector>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"  // default_models_dir
#include "core/model_downloader.hpp"
#include "utils/logger.hpp"
#include "core/model_manager.hpp"
#include "core/model_install.hpp"
#include "core/model_metadata.hpp"

namespace fs = std::filesystem;

namespace {

// Case/dash-insensitive name match against the manifest (same normalization
// idea as ModelManager::get_model).
auto find_remote(const std::vector<upix::RemoteModel>& models,
                 const std::string& name) -> const upix::RemoteModel* {
    auto norm = [](std::string s) {
        for (char& c : s) {
            c = (c == '_' || c == ' ') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    auto key = norm(name);
    const auto it = std::ranges::find_if(models, [&](const auto& m) {
        return norm(m.name) == key;
    });
    return it != models.end() ? &*it : nullptr;
}

auto mb(double bytes) -> std::string {
    return std::format("{:.1f} MB", bytes / 1024.0 / 1024.0);
}

// "[动漫] ★" style annotation for catalog models; empty for user imports.
auto scene_suffix(const std::string& name) -> std::string {
    const auto meta = upix::classify_model_scene(name);
    if (meta.scene.empty()) return "";
    return std::format(" [{}]{}", meta.scene, meta.recommended ? " ★" : "");
}

// One-line legend so the tags/stars are self-explanatory in a terminal.
auto print_scene_legend() -> void {
    std::println("  ([场景] 适用：动漫/照片/综合/修复… · ★ 推荐)");
}

int cmd_model_list(const args::Model& a) {
    if (a.json) enable_machine_output();
    upix::ModelManager manager(a.models_dir.empty()
                                         ? fs::path(default_models_dir())
                                         : fs::path(a.models_dir));

    // Machine-readable form: names only, and quiet — the logger writes to
    // stdout by design (machine mode reroutes it to stderr), so any
    // diagnostic would pollute what scripts and shell completions consume.
    // Missing/unscannable dir → no output at all.
    if (a.names_only) {
        std::error_code ec;
        if (!fs::is_directory(manager.models_directory(), ec)) return 0;
        auto names = manager.scan_models();
        if (names.has_value()) {
            for (const auto& m : *names) std::println("{}", m.name);
        }
        return 0;
    }

    auto models = manager.scan_models();

    if (!models.has_value()) {
        LOG_ERROR("Model", "Failed to scan models: {}", models.error().message);
        return 1;
    }

    if (a.json) {
        // Same fields the desktop catalog shows; `size` keeps the human
        // formatting so the GUI needs no formatting logic.
        std::string json = std::format("{{\"modelsDir\":\"{}\",\"installed\":[",
                                       json_escape(manager.models_directory().string()));
        bool first = true;
        for (const auto& m : models.value()) {
            if (!first) json += ",";
            first = false;
            const auto meta = upix::classify_model_scene(m.name);
            json += std::format(
                R"({{"name":"{}","scale":{},"size":"{} MB","scene":"{}","recommended":{}}})",
                json_escape(m.name), m.scale_factor, m.total_size / 1024 / 1024,
                json_escape(meta.scene), meta.recommended ? "true" : "false");
        }
        json += "]}";
        emit_json_line(json);
        return 0;
    }

    std::println("Installed models ({}):", manager.models_directory().string());
    if (models.value().empty()) {
        std::println("  (none — try `upix model download <name>` or `model list-remote`)");
        return 0;
    }
    for (const auto& m : models.value()) {
        std::println("  {} ({}x, {} MB){}", m.name, m.scale_factor,
                     m.total_size / 1024 / 1024, scene_suffix(m.name));
    }
    print_scene_legend();
    return 0;
}

int cmd_model_list_remote(const args::Model& a) {
    if (a.json) enable_machine_output();
    std::string error;
    auto remote = upix::fetch_remote_models(a.repo, error);
    if (!remote) {
        LOG_ERROR("Model", "Failed to fetch remote models: {}", error);
        return 1;
    }

    if (a.json) {
        std::string json = std::format("{{\"manifestUrl\":\"{}\",\"remote\":[",
                                       json_escape(upix::manifest_url(a.repo)));
        bool first = true;
        for (const auto& m : *remote) {
            if (!first) json += ",";
            first = false;
            const auto meta = upix::classify_model_scene(m.name);
            json += std::format(
                R"({{"name":"{}","scale":{},"size":"{}","scene":"{}","recommended":{}}})",
                json_escape(m.name), m.scale, mb(m.size_bytes),
                json_escape(meta.scene), meta.recommended ? "true" : "false");
        }
        json += "]}";
        emit_json_line(json);
        return 0;
    }

    std::println("Models available in the release ({}):",
                 upix::manifest_url(a.repo));
    if (remote->empty()) {
        std::println("  (manifest is empty)");
    }
    for (const auto& m : *remote) {
        std::println("  {} ({}x, {}){}", m.name, m.scale, mb(m.size_bytes),
                     scene_suffix(m.name));
    }
    print_scene_legend();
    std::println("\nDownload with: upix model download <name>");
    return 0;
}

int cmd_model_download(const args::Model& a) {
    if (a.json) enable_machine_output();
    if (a.names.empty()) {
        LOG_ERROR("Model", "model download requires at least one model name (see `model list-remote`)");
        return 1;
    }

    std::string error;
    auto remote = upix::fetch_remote_models(a.repo, error);
    if (!remote) {
        LOG_ERROR("Model", "Failed to fetch remote models: {}", error);
        return 1;
    }

    fs::path dir = a.models_dir.empty() ? fs::path(default_models_dir()) : fs::path(a.models_dir);
    int failures = 0;
    for (const auto& name : a.names) {
        const auto* m = find_remote(*remote, name);
        if (m == nullptr) {
            LOG_ERROR("Model", "Model not found in release: {}", name);
            failures++;
            continue;
        }

        // Machine consumers key the event stream per model; build the event
        // prefix once (name is escaped exactly once).
        const std::string event_prefix =
            a.json ? std::format(R"({{"t":"download","name":"{}","pct":)", json_escape(m->name))
                   : std::string();
        if (!a.json) {
            std::print("Downloading {} ({}x, {}) ... ", m->name, m->scale,
                       mb(m->size_bytes));
            std::fflush(stdout);
        }
        auto last_pct = -1;
        auto result = upix::download_model(*m, dir.string(),
            [&](const upix::DownloadProgress& p) {
                if (p.total > 0) {
                    int pct = static_cast<int>(p.downloaded * 100 / p.total);
                    if (pct != last_pct && pct % 10 == 0) {
                        if (a.json) {
                            emit_json_line(event_prefix + std::to_string(pct) + "}");
                        } else {
                            std::print("{}% ", pct);
                            std::fflush(stdout);
                        }
                        last_pct = pct;
                    }
                }
            });
        if (!result.ok) {
            if (!a.json) std::println("");
            LOG_ERROR("Model", "Download failed: {}", result.message);
            if (a.json) {
                // Terminal event: without it a machine consumer that keyed on
                // the progress stream cannot tell a failure from a stall.
                emit_json_line(std::format(
                    R"({{"t":"download","name":"{}","error":"{}"}})",
                    json_escape(m->name), json_escape(result.message)));
            }
            failures++;
            continue;
        }
        if (a.json) {
            // The callback is throttled to 10% steps and may never see 100.
            if (last_pct < 100) emit_json_line(event_prefix + "100}");
        } else {
            std::println("done\n  {}\n  {}", result.param_path, result.bin_path);
        }
    }
    return failures == 0 ? 0 : 1;
}

// Import local .param/.bin files (user-provided models) — pair matching,
// staged install and rollback live in share (core/model_install.cpp); this
// command keeps only the human/JSON presentation.
int cmd_model_import(const args::Model& a) {
    if (a.json) enable_machine_output();
    if (a.files.empty()) {
        LOG_ERROR("Model", "model import requires at least one .param/.bin file path");
        return 1;
    }

    fs::path dir = a.models_dir.empty() ? fs::path(default_models_dir()) : fs::path(a.models_dir);
    auto result = upix::import_model_files(a.files, dir);
    for (const auto& warning : result.warnings) {
        LOG_WARNING("Model", "{}", warning);
    }
    if (a.json) {
        std::string json;
        if (result.ok) {
            json = R"({"ok":true,"imported":[)";
            for (size_t i = 0; i < result.imported.size(); ++i) {
                if (i > 0) json += ",";
                json += std::format(R"("{}")", json_escape(result.imported[i]));
            }
            json += "]}";
        } else {
            json = std::format(R"({{"ok":false,"error":"{}"}})",
                               json_escape(result.error));
        }
        emit_json_line(json);
    }
    return result.ok ? 0 : 1;
}

}  // namespace

int cmd_model(const args::Model& a) {
    if (a.subcommand == "list") return cmd_model_list(a);
    if (a.subcommand == "list-remote") return cmd_model_list_remote(a);
    if (a.subcommand == "download") return cmd_model_download(a);
    if (a.subcommand == "import") return cmd_model_import(a);
    LOG_ERROR("Model", "Unknown model subcommand: {}", a.subcommand);
    return 1;
}
