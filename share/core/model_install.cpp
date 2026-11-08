#include "core/model_install.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "core/engine_setup.hpp"  // probe_model_sanity (warn-only)
#include "utils/logger.hpp"

namespace fs = std::filesystem;

namespace upix {
namespace {

// Refuse destinations that are not regular files (a directory, fifo, …):
// the install renames over them, which either fails halfway through the
// pair or would move the user's own entry aside. A symlink to a regular
// file is accepted — that is how a model name is pointed at a copy.
auto inspect_destination(const fs::path& path, bool& present) -> bool {
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
}

}  // namespace

auto import_model_files(const std::vector<std::string>& files,
                        const std::filesystem::path& dir) -> ImportOutcome {
    ImportOutcome out;
    if (files.empty()) {
        out.error = "import requires at least one .param/.bin file path";
        return out;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        out.error = std::format("cannot create models directory {}: {}",
                                dir.string(), ec.message());
        return out;
    }

    int failures = 0;
    std::vector<fs::path> params, bins;
    for (const auto& raw : files) {
        fs::path src(raw);
        std::string ext = src.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".param") params.push_back(src);
        else if (ext == ".bin") bins.push_back(src);
        else {
            LOG_ERROR("Model", "Unsupported file type (expected .param/.bin): {}", raw);
            if (out.error.empty())
                out.error = std::format("unsupported file type: {}", raw);
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
            if (out.error.empty())
                out.error = std::format("no matching .bin for {}", param.string());
            failures++;
            continue;
        }
        const fs::path dst_param = dir / param.filename();
        const fs::path dst_bin = dir / it->filename();
        bool had_param = false;
        bool had_bin = false;
        ec.clear();
        if (!inspect_destination(dst_param, had_param) ||
            !inspect_destination(dst_bin, had_bin)) {
            LOG_ERROR("Model", "Destination is not a regular file: {}",
                      (!had_param ? dst_param : dst_bin).string());
            if (out.error.empty()) {
                out.error = std::format("destination is not a regular file: {}",
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
            if (out.error.empty())
                out.error = std::format("import failed: {}", param.string());
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
        LOG_INFO("Model", "Imported {}\n  {}\n  {}", param.stem().string(),
                 dst_param.string(), dst_bin.string());
        out.imported.push_back(param.stem().string());
        // One synthetic forward against a throwaway CPU engine: non-finite
        // output = conversion mis-executes on this ncnn build. Warn-only —
        // the import stands, the user decides.
        if (auto warning = probe_model_sanity(dst_param, dst_bin)) {
            LOG_WARNING("Model", "{}: {}", param.stem().string(), *warning);
            out.warnings.push_back(std::format("{}: {}", param.stem().string(), *warning));
        }
        bins.erase(it);
    }
    for (const auto& b : bins) {
        LOG_ERROR("Model", "No matching .param for {}", b.string());
        if (out.error.empty())
            out.error = std::format("no matching .param for {}", b.string());
        failures++;
    }
    out.ok = failures == 0;
    return out;
}

}  // namespace upix
