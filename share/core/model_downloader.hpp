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

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace upix {

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

enum class ModelSource {
    HfMirror,     // https://hf-mirror.com/xihale/upix-models/resolve/main
    HuggingFace,  // https://huggingface.co/xihale/upix-models/resolve/main
    GitHub,       // https://github.com/xihale/upix/releases/download/models
};

// Default release endpoint: the "models" tag of the upstream repo.
inline constexpr std::string_view kDefaultModelRepo = "xihale/upix";
inline constexpr std::string_view kHfModelRepo = "xihale/upix-models";
inline constexpr std::string_view kHfMirrorBaseUrl =
    "https://hf-mirror.com/xihale/upix-models/resolve/main";
inline constexpr std::string_view kHfBaseUrl =
    "https://huggingface.co/xihale/upix-models/resolve/main";
inline constexpr std::string_view kGithubReleaseBaseUrl =
    "https://github.com/xihale/upix/releases/download/models";

// Detect whether the host uses a Mainland China locale or timezone.
auto is_china_locale() -> bool;

// Default preferred source (`HfMirror` in Mainland China, `HuggingFace` elsewhere).
auto default_model_source() -> ModelSource;

// Serialization helpers (`"hf-mirror"`, `"hf"`, `"github"`).
auto model_source_id(ModelSource src) -> std::string_view;
auto parse_model_source(std::string_view id) -> std::optional<ModelSource>;
auto model_source_base_url(ModelSource src) -> std::string_view;

// Ordered primary -> fallback chain for official routes.
auto model_source_chain(ModelSource primary) -> std::vector<ModelSource>;

// Upper bound for manifest HTTP GET responses (10 MiB).
inline constexpr std::size_t kMaxManifestBytes = 10u << 20;

struct RemoteCatalogResult {
    std::vector<RemoteModel> models;
    // Populated with the network/parse error when falling back to the embedded catalog.
    std::optional<std::string> fallback_error;
};

// Embedded published catalog as manifest-shaped entries (names + scale hints;
// size_bytes/sha256/url stay empty because those are per-release facts).
auto catalog_models() -> std::vector<RemoteModel>;

// Fetch and parse the release manifest. Returns nullopt + error message in
// `error` on failure.
auto fetch_remote_models(const std::string& repo /* "owner/name" */,
                         std::string& error)
    -> std::optional<std::vector<RemoteModel>>;
auto fetch_remote_models(const std::string& repo,
                         ModelSource source,
                         std::string& error)
    -> std::optional<std::vector<RemoteModel>>;

// Same as fetch_remote_models(), falling back to catalog_models() on failure.
// When `fallback_error` is set, the caller should warn that downloads require
// live release metadata.
auto fetch_remote_models_or_catalog(const std::string& repo = "")
    -> RemoteCatalogResult;
auto fetch_remote_models_or_catalog(const std::string& repo,
                                    ModelSource source)
    -> RemoteCatalogResult;
auto fetch_remote_models_or_catalog(const std::string& repo,
                                    std::string& error_out)
    -> RemoteCatalogResult;

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
auto download_model(const RemoteModel& model,
                    const std::string& models_dir,
                    const std::function<void(const DownloadProgress&)>& on_progress,
                    ModelSource source,
                    UrlPolicy policy = UrlPolicy::HttpsOnly)
    -> ModelDownloadResult;

// Build the manifest URL for a repo ("owner/name" or "" for the default).
auto manifest_url(const std::string& repo) -> std::string;

}  // namespace upix
