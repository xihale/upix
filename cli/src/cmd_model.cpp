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
#include "model_downloader.hpp"
#include "utils/logger.hpp"
#include "core/engine_setup.hpp"  // probe_model_sanity
#include "core/model_manager.hpp"
#include "core/model_metadata.hpp"

namespace fs = std::filesystem;

namespace {

// Case/dash-insensitive name match against the manifest (same normalization
// idea as ModelManager::get_model).
auto find_remote(const std::vector<upix::cli::RemoteModel>& models,
                 const std::string& name) -> const upix::cli::RemoteModel* {
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
    auto remote = upix::cli::fetch_remote_models(a.repo, error);
    if (!remote) {
        LOG_ERROR("Model", "Failed to fetch remote models: {}", error);
        return 1;
    }

    if (a.json) {
        std::string json = std::format("{{\"manifestUrl\":\"{}\",\"remote\":[",
                                       json_escape(upix::cli::manifest_url(a.repo)));
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
                 upix::cli::manifest_url(a.repo));
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
    auto remote = upix::cli::fetch_remote_models(a.repo, error);
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
        auto result = upix::cli::download_model(*m, dir.string(),
            [&](const upix::cli::DownloadProgress& p) {
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

// Import local .param/.bin files (user-provided models): each file is copied
// into the models dir; pairs are matched by stem. A .zip is copied as-is for
// later manual extraction? No — keep it simple: only .param/.bin.
int cmd_model_import(const args::Model& a) {
    if (a.json) enable_machine_output();
    if (a.files.empty()) {
        LOG_ERROR("Model", "model import requires at least one .param/.bin file path");
        return 1;
    }

    fs::path dir = a.models_dir.empty() ? fs::path(default_models_dir()) : fs::path(a.models_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("Model", "Cannot create models directory {}: {}", dir.string(), ec.message());
        return 1;
    }

    int failures = 0;
    std::string first_error;
    std::vector<std::string> imported;
    std::vector<fs::path> params, bins;
    for (const auto& raw : a.files) {
        fs::path src(raw);
        std::string ext = src.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".param") params.push_back(src);
        else if (ext == ".bin") bins.push_back(src);
        else {
            LOG_ERROR("Model", "Unsupported file type (expected .param/.bin): {}", raw);
            if (first_error.empty()) first_error = std::format("Unsupported file type: {}", raw);
            failures++;
        }
    }

    // Match pairs by stem.
    for (const auto& param : params) {
        auto it = std::find_if(bins.begin(), bins.end(), [&](const fs::path& b) {
            return b.stem() == param.stem();
        });
        if (it == bins.end()) {
            LOG_ERROR("Model", "No matching .bin for {}", param.string());
            if (first_error.empty()) first_error = std::format("No matching .bin for {}", param.string());
            failures++;
            continue;
        }
        const fs::path dst_param = dir / param.filename();
        const fs::path dst_bin = dir / it->filename();
        // Refuse destinations that are not regular files (a directory, fifo, …):
        // the install renames over them, which either fails halfway through the
        // pair or would move the user's own entry aside. A symlink to a regular
        // file is accepted — that is how a model name is pointed at a copy.
        auto inspect_destination = [&](const fs::path& path, bool& present) -> bool {
            std::error_code st_ec;
            const auto status = fs::symlink_status(path, st_ec);
            if (status.type() == fs::file_type::not_found &&
                (!st_ec || st_ec == std::errc::no_such_file_or_directory)) {
                present = false;
                return true;
            }
            if (st_ec) return false;
            present = true;
            if (status.type() == fs::file_type::regular) return true;
            if (status.type() == fs::file_type::symlink) {
                std::error_code target_ec;
                const auto target = fs::status(path, target_ec);
                return !target_ec && target.type() == fs::file_type::regular;
            }
            return false;
        };
        bool had_param = false;
        bool had_bin = false;
        ec.clear();
        if (!inspect_destination(dst_param, had_param) ||
            !inspect_destination(dst_bin, had_bin)) {
            LOG_ERROR("Model", "Destination is not a regular file: {}",
                      (!had_param ? dst_param : dst_bin).string());
            if (first_error.empty()) {
                first_error = std::format("Destination is not a regular file: {}",
                                          dst_param.string());
            }
            failures++;
            continue;
        }
        const bool replacing = had_param || had_bin;
        // Stage both copies beside their destinations, back the previous pair
        // up, then rename into place. A failed or partial copy must never leave
        // a .param without its matching .bin (the scanner would list a model
        // that cannot load), and a failure after the first rename must put the
        // previous pair back.
        const fs::path tmp_param =
            dir / (param.filename().string() + ".upix-import");
        const fs::path tmp_bin = dir / (it->filename().string() + ".upix-import");
        const fs::path bak_param =
            dir / (param.filename().string() + ".upix-backup");
        const fs::path bak_bin = dir / (it->filename().string() + ".upix-backup");
        bool saved_param = false;
        bool saved_bin = false;
        bool installed_param = false;
        bool installed_bin = false;
        bool ok = true;
        fs::copy_file(param, tmp_param, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            fs::copy_file(*it, tmp_bin, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            ok = false;
        }
        if (ok && had_param) {
            fs::rename(dst_param, bak_param, ec);
            saved_param = !ec;
            ok = saved_param;
        }
        if (ok && had_bin) {
            fs::rename(dst_bin, bak_bin, ec);
            saved_bin = !ec;
            ok = saved_bin;
        }
        if (ok) {
            fs::rename(tmp_param, dst_param, ec);
            installed_param = !ec;
            ok = installed_param;
        }
        if (ok) {
            fs::rename(tmp_bin, dst_bin, ec);
            installed_bin = !ec;
            ok = installed_bin;
        }
        if (!ok) {
            std::error_code rm_ec;
            if (installed_param) fs::remove(dst_param, rm_ec);
            if (installed_bin) fs::remove(dst_bin, rm_ec);
            fs::remove(tmp_param, rm_ec);
            fs::remove(tmp_bin, rm_ec);
            // Restore only what this attempt moved aside.
            if (saved_param) fs::rename(bak_param, dst_param, rm_ec);
            if (saved_bin) fs::rename(bak_bin, dst_bin, rm_ec);
            LOG_ERROR("Model", "Import failed: {}: {}", param.string(), ec.message());
            if (first_error.empty()) first_error = std::format("Import failed: {}", param.string());
            failures++;
            continue;
        }
        {
            std::error_code rm_ec;
            if (saved_param) fs::remove(bak_param, rm_ec);
            if (saved_bin) fs::remove(bak_bin, rm_ec);
        }
        if (replacing) {
            LOG_WARNING("Model", "Replaced existing model files for {}",
                        param.stem().string());
        }
        std::println("Imported {}\n  {}\n  {}", param.stem().string(),
                     dst_param.string(), dst_bin.string());
        imported.push_back(param.stem().string());
        // One synthetic forward against a throwaway CPU engine: non-finite
        // output = conversion mis-executes on this ncnn build. Warn-only —
        // the import stands, the user decides.
        if (auto warning = upix::probe_model_sanity(dst_param, dst_bin)) {
            LOG_WARNING("Model", "{}: {}", param.stem().string(), *warning);
        }
        bins.erase(it);
    }
    for (const auto& b : bins) {
        LOG_ERROR("Model", "No matching .param for {}", b.string());
        if (first_error.empty()) first_error = std::format("No matching .param for {}", b.string());
        failures++;
    }
    if (a.json) {
        std::string json;
        if (failures == 0) {
            json = R"({"ok":true,"imported":[)";
            for (size_t i = 0; i < imported.size(); ++i) {
                if (i > 0) json += ",";
                json += std::format(R"("{}")", json_escape(imported[i]));
            }
            json += "]}";
        } else {
            json = std::format(R"({{"ok":false,"error":"{}"}})",
                               json_escape(first_error));
        }
        emit_json_line(json);
    }
    return failures == 0 ? 0 : 1;
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
