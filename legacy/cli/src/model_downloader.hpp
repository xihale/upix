// GitHub "models" release client: manifest fetch + model download.
//
// The release layout (built by scripts/publish_models.py):
//   manifest.json                     — catalog (name/scale/size/sha256/url)
//   <model-name>.zip                  — one zip per model: <name>.param + <name>.bin
//
// Downloads and verifies the zip, extracts the exact model pair in a unique
// same-filesystem staging directory, then replaces the installed pair with
// backups and rollback on handled failures. Two-file replacement is not
// crash-atomic. All failures return a message instead of throwing.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace upix::cli {

struct RemoteModel {
    std::string name;
    int scale = 2;
    std::uint64_t size_bytes = 0;   // zip size
    std::string sha256;             // zip sha256, hex
    std::string url;                // zip download URL
};

struct DownloadProgress {
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;        // 0 = unknown
};

struct ModelDownloadResult {
    bool ok = false;
    std::string message;            // human-readable error on failure
    std::string param_path;         // installed files on success
    std::string bin_path;
};

// Default release endpoint: the "models" tag of the upstream repo.
inline constexpr std::string_view kDefaultModelRepo = "xihale/upix";

// Fetch and parse the release manifest. Returns nullopt + error message in
// `error` on failure.
auto fetch_remote_models(const std::string& repo /* "owner/name" */,
                         std::string& error)
    -> std::optional<std::vector<RemoteModel>>;

// Transport policy for a manifest-supplied artifact `url`. Production requires
// https: the manifest is remote input (a user-supplied --repo controls it) and
// libcurl's default protocol set includes file://, so an unfiltered URL could
// copy an arbitrary local file into the models dir — carrying a
// manifest-supplied sha256 that matches by construction. Tests may opt into
// file:// so a hermetic local zip can be "downloaded" without a server.
enum class UrlPolicy { HttpsOnly, AllowLocalFileForTests };

// Download `model` (from the manifest) and install into `models_dir`.
// `on_progress` is called with accumulated bytes (may be called frequently).
auto download_model(const RemoteModel& model,
                    const std::string& models_dir,
                    const std::function<void(const DownloadProgress&)>& on_progress,
                    UrlPolicy policy = UrlPolicy::HttpsOnly)
    -> ModelDownloadResult;

// Build the manifest URL for a repo ("owner/name" or "" for the default).
auto manifest_url(const std::string& repo) -> std::string;

}  // namespace upix::cli
