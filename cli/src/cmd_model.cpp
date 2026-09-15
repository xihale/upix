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

int cmd_model_list(const args::Model& a) {
    upix::ModelManager manager(a.models_dir.empty()
                                         ? fs::path(default_models_dir())
                                         : fs::path(a.models_dir));

    // Machine-readable form: names only, and quiet — the logger writes to
    // stdout by design, so any diagnostic would pollute what scripts and
    // shell completions consume. Missing/unscannable dir → no output at all.
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

    std::println("Installed models ({}):", manager.models_directory().string());
    if (models.value().empty()) {
        std::println("  (none — try `upix model download <name>` or `model list-remote`)");
        return 0;
    }
    for (const auto& m : models.value()) {
        std::println("  {} ({}x, {} MB)", m.name, m.scale_factor,
                     m.total_size / 1024 / 1024);
    }
    return 0;
}

int cmd_model_list_remote(const args::Model& a) {
    std::string error;
    auto remote = upix::cli::fetch_remote_models(a.repo, error);
    if (!remote) {
        LOG_ERROR("Model", "Failed to fetch remote models: {}", error);
        return 1;
    }

    std::println("Models available in the release ({}):",
                 upix::cli::manifest_url(a.repo));
    if (remote->empty()) {
        std::println("  (manifest is empty)");
    }
    for (const auto& m : *remote) {
        std::println("  {} ({}x, {})", m.name, m.scale, mb(m.size_bytes));
    }
    std::println("\nDownload with: upix model download <name>");
    return 0;
}

int cmd_model_download(const args::Model& a) {
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

        std::print("Downloading {} ({}x, {}) ... ", m->name, m->scale,
                   mb(m->size_bytes));
        std::fflush(stdout);
        auto last_pct = -1;
        auto result = upix::cli::download_model(*m, dir.string(),
            [&](const upix::cli::DownloadProgress& p) {
                if (p.total > 0) {
                    int pct = static_cast<int>(p.downloaded * 100 / p.total);
                    if (pct != last_pct && pct % 10 == 0) {
                        std::print("{}% ", pct);
                        std::fflush(stdout);
                        last_pct = pct;
                    }
                }
            });
        if (!result.ok) {
            std::println("");
            LOG_ERROR("Model", "Download failed: {}", result.message);
            failures++;
            continue;
        }
        std::println("done\n  {}\n  {}", result.param_path, result.bin_path);
    }
    return failures == 0 ? 0 : 1;
}

// Import local .param/.bin files (user-provided models): each file is copied
// into the models dir; pairs are matched by stem. A .zip is copied as-is for
// later manual extraction? No — keep it simple: only .param/.bin.
int cmd_model_import(const args::Model& a) {
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
            failures++;
            continue;
        }
        fs::path dst_param = dir / param.filename();
        fs::path dst_bin = dir / it->filename();
        fs::copy_file(param, dst_param, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Model", "Copy failed: {}: {}", param.string(), ec.message());
            failures++;
            continue;
        }
        fs::copy_file(*it, dst_bin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Model", "Copy failed: {}: {}", it->string(), ec.message());
            failures++;
            continue;
        }
        std::println("Imported {}\n  {}\n  {}", param.stem().string(),
                     dst_param.string(), dst_bin.string());
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
        failures++;
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
