// `crossscale model` — local catalog + GitHub release model management.
//
//   model list                     scan the models dir, print name/scale/size
//   model list-remote [--repo R]   fetch the release manifest, print catalog
//   model download NAME [...]      download model(s) into the models dir
//   model import FILE [...]        copy local .param/.bin pairs into the dir
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "args.hpp"
#include "cli_app.hpp"
#include "model_downloader.hpp"
#include "utils/logger.hpp"
#include "core/model_manager.hpp"

namespace fs = std::filesystem;

namespace {

// Case/dash-insensitive name match against the manifest (same normalization
// idea as ModelManager::get_model).
auto find_remote(const std::vector<crossscale::cli::RemoteModel>& models,
                 const std::string& name) -> const crossscale::cli::RemoteModel* {
    auto norm = [](std::string s) {
        for (char& c : s) {
            c = (c == '_') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    auto key = norm(name);
    for (const auto& m : models) {
        if (norm(m.name) == key) return &m;
    }
    return nullptr;
}

auto mb(double bytes) -> std::string {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (bytes / 1024.0 / 1024.0) << " MB";
    return oss.str();
}

int cmd_model_list(const args::Model& a) {
    crossscale::ModelManager manager(a.models_dir.empty()
                                         ? fs::current_path() / "models"
                                         : fs::path(a.models_dir));
    auto models = manager.scan_models();

    if (!models.has_value()) {
        LOG_ERROR("Model", "Failed to scan models: " + models.error().message);
        return 1;
    }

    std::cout << "Installed models (" << manager.models_directory().string() << "):\n";
    if (models.value().empty()) {
        std::cout << "  (none — try `crossscale model download <name>` or `model list-remote`)\n";
        return 0;
    }
    for (const auto& m : models.value()) {
        std::cout << "  " << m.name << " (" << m.scale_factor << "x, "
                  << (m.total_size / 1024 / 1024) << " MB)\n";
    }
    return 0;
}

int cmd_model_list_remote(const args::Model& a) {
    std::string error;
    auto remote = crossscale::cli::fetch_remote_models(a.repo, error);
    if (!remote) {
        LOG_ERROR("Model", "Failed to fetch remote models: " + error);
        return 1;
    }

    std::cout << "Models available in the release (" << crossscale::cli::manifest_url(a.repo)
              << "):\n";
    if (remote->empty()) {
        std::cout << "  (manifest is empty)\n";
    }
    for (const auto& m : *remote) {
        std::cout << "  " << m.name << " (" << m.scale << "x, " << mb(m.size_bytes) << ")\n";
    }
    std::cout << "\nDownload with: crossscale model download <name>\n";
    return 0;
}

int cmd_model_download(const args::Model& a) {
    if (a.names.empty()) {
        LOG_ERROR("Model", "model download requires at least one model name (see `model list-remote`)");
        return 1;
    }

    std::string error;
    auto remote = crossscale::cli::fetch_remote_models(a.repo, error);
    if (!remote) {
        LOG_ERROR("Model", "Failed to fetch remote models: " + error);
        return 1;
    }

    fs::path dir = a.models_dir.empty() ? fs::path("models") : fs::path(a.models_dir);
    int failures = 0;
    for (const auto& name : a.names) {
        const auto* m = find_remote(*remote, name);
        if (m == nullptr) {
            LOG_ERROR("Model", "Model not found in release: " + name);
            failures++;
            continue;
        }

        std::cout << "Downloading " << m->name << " (" << m->scale << "x, " << mb(m->size_bytes)
                  << ") ... " << std::flush;
        auto last_pct = -1;
        auto result = crossscale::cli::download_model(*m, dir.string(),
            [&](const crossscale::cli::DownloadProgress& p) {
                if (p.total > 0) {
                    int pct = static_cast<int>(p.downloaded * 100 / p.total);
                    if (pct != last_pct && pct % 10 == 0) {
                        std::cout << pct << "% " << std::flush;
                        last_pct = pct;
                    }
                }
            });
        if (!result.ok) {
            std::cout << "\n";
            LOG_ERROR("Model", "Download failed: " + result.message);
            failures++;
            continue;
        }
        std::cout << "done\n  " << result.param_path << "\n  " << result.bin_path << "\n";
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

    fs::path dir = a.models_dir.empty() ? fs::path("models") : fs::path(a.models_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("Model", "Cannot create models directory " + dir.string() + ": " + ec.message());
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
            LOG_ERROR("Model", "Unsupported file type (expected .param/.bin): " + raw);
            failures++;
        }
    }

    // Match pairs by stem.
    for (const auto& param : params) {
        auto it = std::find_if(bins.begin(), bins.end(), [&](const fs::path& b) {
            return b.stem() == param.stem();
        });
        if (it == bins.end()) {
            LOG_ERROR("Model", "No matching .bin for " + param.string());
            failures++;
            continue;
        }
        fs::path dst_param = dir / param.filename();
        fs::path dst_bin = dir / it->filename();
        fs::copy_file(param, dst_param, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Model", "Copy failed: " + param.string() + ": " + ec.message());
            failures++;
            continue;
        }
        fs::copy_file(*it, dst_bin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Model", "Copy failed: " + it->string() + ": " + ec.message());
            failures++;
            continue;
        }
        std::cout << "Imported " << param.stem().string() << "\n  " << dst_param.string()
                  << "\n  " << dst_bin.string() << "\n";
        bins.erase(it);
    }
    for (const auto& b : bins) {
        LOG_ERROR("Model", "No matching .param for " + b.string());
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
    LOG_ERROR("Model", "Unknown model subcommand: " + a.subcommand);
    return 1;
}
