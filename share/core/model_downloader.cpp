// See model_downloader.hpp. libcurl for HTTP(S), glaze for the manifest JSON,
// libsodium for sha256, libzip for extraction.
#include "core/model_downloader.hpp"
#include "core/model_catalog.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <curl/curl.h>
#include <zip.h>
#include <sodium.h>
#include <glaze/glaze.hpp>

namespace fs = std::filesystem;

namespace upix {
namespace {

// ── libcurl helpers ─────────────────────────────────────────────────────────

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl() {
    static CurlGlobal init;
    (void)init;
}

struct CurlDeleter {
    void operator()(CURL* p) const { curl_easy_cleanup(p); }
};
using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;

// Only https:// may be fetched — see UrlPolicy in model_downloader.hpp. Tests
// may additionally allow file:// so a hermetic local zip needs no server.
constexpr std::string_view kAllowedUrlScheme = "https://";
constexpr std::string_view kLocalFileUrlScheme = "file://";

auto url_is_allowed(std::string_view url, UrlPolicy policy) -> bool {
    if (url.starts_with(kAllowedUrlScheme)) {
        return true;
    }
    return policy == UrlPolicy::AllowLocalFileForTests &&
           url.starts_with(kLocalFileUrlScheme);
}

// Redirects must not drop to another scheme either (the initial URL is checked
// separately by url_is_allowed).
void restrict_redirect_protocols(CURL* curl, UrlPolicy policy) {
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,
                     policy == UrlPolicy::AllowLocalFileForTests ? "https,file" : "https");
#else
    (void)curl;
    (void)policy;
#endif
}

struct HttpBodyCtx {
    std::string* body;
    bool size_exceeded = false;
};

auto http_get(const std::string& url, std::string& body, std::string& error,
              long timeout_s = 30) -> bool {
    ensure_curl();
    if (!url_is_allowed(url, UrlPolicy::HttpsOnly)) {
        error = std::format("refusing non-https URL: {}", url);
        return false;
    }
    CurlPtr curl{curl_easy_init()};
    if (!curl) {
        error = "failed to init libcurl";
        return false;
    }
    HttpBodyCtx ctx{&body, false};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);  // GitHub → S3 redirect
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    restrict_redirect_protocols(curl.get(), UrlPolicy::HttpsOnly);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);  // do not read error bodies
    curl_easy_setopt(curl.get(), CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(kMaxManifestBytes));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                     +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                         auto* c = static_cast<HttpBodyCtx*>(ud);
                         if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size) {
                             c->size_exceeded = true;
                             return 0;
                         }
                         const size_t chunk = size * nmemb;
                         if (c->body->size() + chunk > kMaxManifestBytes) {
                             c->size_exceeded = true;
                             return 0;
                         }
                         c->body->append(ptr, chunk);
                         return chunk;
                     });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "upix-cli/0.1");

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        error = ctx.size_exceeded
            ? std::format("manifest exceeded maximum size ({} bytes)", kMaxManifestBytes)
            : std::string("HTTP request failed: ") + curl_easy_strerror(rc);
        return false;
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    // Only https is reachable (url_is_allowed), so this is a real HTTP failure:
    // 4xx/5xx already failed via CURLOPT_FAILONERROR, this catches 2xx-but-not-200.
    if (status != 200) {
        error = std::format("HTTP {} for {}", status, url);
        return false;
    }
    return true;
}

struct FileCtx {
    std::ofstream* out;
    DownloadProgress progress;
    const std::function<void(const DownloadProgress&)>* on_progress;
    bool size_exceeded = false;  // set by the write callback to explain the abort
};

auto http_download_to_file(const std::string& url, const fs::path& dest,
                           std::uint64_t expected_size,
                           const std::function<void(const DownloadProgress&)>& on_progress,
                           std::string& error, UrlPolicy policy) -> bool {
    ensure_curl();
    if (!url_is_allowed(url, policy)) {
        error = std::format("refusing non-https URL: {}", url);
        return false;
    }
    CurlPtr curl{curl_easy_init()};
    if (!curl) {
        error = "failed to init libcurl";
        return false;
    }

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = std::format("cannot open {} for writing", dest.string());
        return false;
    }

    FileCtx ctx{&out, DownloadProgress{0, expected_size}, &on_progress};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    restrict_redirect_protocols(curl.get(), policy);
    // A 404/5xx body must not be streamed into the staging file at all.
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                     +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                         auto* c = static_cast<FileCtx*>(ud);
                         const size_t chunk = size * nmemb;
                         // The manifest declares the artifact size: never write
                         // past it, so a broken or hostile server cannot fill
                         // the disk before the sha256 check gets a say.
                         if (c->progress.total > 0 &&
                             c->progress.downloaded + chunk > c->progress.total) {
                             c->size_exceeded = true;
                             return 0;  // abort the transfer
                         }
                         c->out->write(ptr, static_cast<std::streamsize>(chunk));
                         if (!c->out->good()) return 0;  // abort transfer
                         c->progress.downloaded += chunk;
                         if (c->on_progress && *c->on_progress) {
                             (*c->on_progress)(c->progress);
                         }
                         return chunk;
                     });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 0L);          // no total timeout
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);  // stall guard:
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);  // <1B/s for 60s → abort
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "upix-cli/0.1");

    CURLcode rc = curl_easy_perform(curl.get());
    out.close();
    if (rc != CURLE_OK) {
        error = ctx.size_exceeded
            ? std::format("download exceeded the declared size ({})",
                          expected_size)
            : std::string("download failed: ") + curl_easy_strerror(rc);
        return false;
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    // HTTP(S) must return 200. The test-only file:// transport has no HTTP
    // status (it reports 0), so it is exempt from this check.
    const bool local_file =
        policy == UrlPolicy::AllowLocalFileForTests &&
        url.starts_with(kLocalFileUrlScheme);
    if (status != 200 && !local_file) {
        error = std::format("HTTP {} for {}", status, url);
        return false;
    }
    return true;
}

auto sha256_file(const fs::path& path, std::string& error) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("cannot open {}", path.string());
        return {};
    }
    if (sodium_init() < 0) {
        error = "libsodium init failed";
        return {};
    }
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);

    std::array<char, 64 * 1024> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto got = in.gcount();
        if (got > 0) {
            crypto_hash_sha256_update(
                &st, reinterpret_cast<const unsigned char*>(buf.data()),
                static_cast<unsigned long long>(got));
        }
    }
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&st, digest);

    std::string hex;
    hex.reserve(sizeof(digest) * 2);
    for (unsigned char b : digest) {
        hex += std::format("{:02x}", b);
    }
    return hex;
}

// Extract only model entries into a private stage. Ancillary archive entries
// are ignored, while every .param/.bin entry must use the published names.
auto extract_zip(const fs::path& zip_path, const fs::path& out_dir,
                 const std::string& model_name, std::string& error) -> bool {
    int err_code = 0;
    const std::string zip_path_str = zip_path.string();
    zip_t* raw = zip_open(zip_path_str.c_str(), ZIP_RDONLY, &err_code);
    if (!raw) {
        zip_error_t zerr;
        zip_error_init_with_code(&zerr, err_code);
        error = std::string("cannot open zip: ") + zip_error_strerror(&zerr);
        zip_error_fini(&zerr);
        return false;
    }
    struct ZipClose { void operator()(zip_t* p) const { zip_close(p); } };
    std::unique_ptr<zip_t, ZipClose> zip{raw};

    const std::string expected_param = model_name + ".param";
    const std::string expected_bin = model_name + ".bin";
    bool saw_param = false;
    bool saw_bin = false;
    zip_int64_t count = zip_get_num_entries(zip.get(), 0);
    if (count < 0) {
        error = "cannot enumerate zip entries";
        return false;
    }
    for (zip_int64_t i = 0; i < count; ++i) {
        const char* raw_name = zip_get_name(zip.get(), i, ZIP_FL_ENC_GUESS);
        if (!raw_name) {
            error = "cannot read zip entry name";
            return false;
        }
        std::string name(raw_name);
        auto slash = name.find_last_of('/');
        std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
        std::string lower = base;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!lower.ends_with(".param") && !lower.ends_with(".bin")) continue;

        bool is_param = name == expected_param;
        bool is_bin = name == expected_bin;
        if (!is_param && !is_bin) {
            error = std::format("unexpected model entry '{}' (expected '{}' and '{}')",
                                name, expected_param, expected_bin);
            return false;
        }
        bool& seen = is_param ? saw_param : saw_bin;
        if (seen) {
            error = std::format("duplicate model entry '{}'", name);
            return false;
        }
        seen = true;

        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(zip.get(), i, 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE)) {
            error = std::format("cannot inspect zip entry '{}'", name);
            return false;
        }
        zip_file_t* opened = zip_fopen_index(zip.get(), i, 0);
        if (!opened) {
            error = std::format("cannot read zip entry '{}'", name);
            return false;
        }
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> entry{opened, &zip_fclose};
        fs::path dest = out_dir / base;
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = std::format("cannot write {}", dest.string());
            return false;
        }
        std::array<char, 64 * 1024> buf;
        zip_uint64_t total = 0;
        while (true) {
            zip_int64_t got = zip_fread(entry.get(), buf.data(), buf.size());
            if (got < 0) {
                error = std::format("error reading zip entry '{}'", name);
                return false;
            }
            if (got == 0) break;
            out.write(buf.data(), static_cast<std::streamsize>(got));
            if (!out) {
                error = std::format("short write for {}", dest.string());
                return false;
            }
            total += static_cast<zip_uint64_t>(got);
        }
        out.close();
        if (!out.good()) {
            error = std::format("short write for {}", dest.string());
            return false;
        }
        if (total != stat.size) {
            error = std::format("truncated zip entry '{}' (read {} of {} bytes)",
                                name, total, stat.size);
            return false;
        }
    }
    if (!saw_param || !saw_bin) {
        error = std::format("zip must contain exactly '{}' and '{}'", expected_param,
                            expected_bin);
        return false;
    }
    return true;
}

auto create_stage(const fs::path& dir, std::string& error) -> fs::path {
    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path stage = dir / std::format(".upix-model-stage-{}-{}", tick,
                                           sequence.fetch_add(1));
        std::error_code ec;
        if (fs::create_directory(stage, ec)) return stage;
        if (ec && ec != std::errc::file_exists) {
            error = std::format("cannot create staging directory {}: {}",
                                stage.string(), ec.message());
            return {};
        }
    }
    error = std::format("cannot create a unique staging directory in {}", dir.string());
    return {};
}

}  // namespace

// Manifest JSON schema. Glaze reflection requires external linkage, so this
// lives at namespace scope (not inside the parse function).
struct ManifestJson {
    int version = 0;
    struct Entry {
        std::string name;
        int scale = 2;
        std::uint64_t size_bytes = 0;
        std::string sha256;
        std::string url;
    };
    std::vector<Entry> models;
};

auto catalog_models() -> std::vector<RemoteModel> {
    std::vector<RemoteModel> out;
    out.reserve(kModelCatalog.size());
    for (std::string_view name : kModelCatalog) {
        RemoteModel m;
        m.name = std::string(name);
        m.scale = catalog_scale_hint(name);
        out.push_back(std::move(m));
    }
    return out;
}

namespace {

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    return it != hay.end();
}

bool is_china_tz_name(std::string_view tz) {
    return contains_ci(tz, "Asia/Shanghai") ||
           contains_ci(tz, "Asia/Chongqing") ||
           contains_ci(tz, "Asia/Harbin") ||
           contains_ci(tz, "Asia/Urumqi") ||
           contains_ci(tz, "/PRC") ||
           tz == "PRC";
}

bool is_china_lang_tag(std::string_view tag) {
    return contains_ci(tag, "zh_CN") ||
           contains_ci(tag, "zh-CN") ||
           contains_ci(tag, "zh_Hans") ||
           contains_ci(tag, "zh-Hans");
}

bool is_official_model_url(std::string_view url) {
    return url.empty() ||
           url.starts_with(kHfMirrorBaseUrl) ||
           url.starts_with(kHfBaseUrl) ||
           url.starts_with(kGithubReleaseBaseUrl);
}

auto manifest_urls(ModelSource primary, const std::string& repo) -> std::vector<std::string> {
    if (!repo.empty()) {
        return {"https://github.com/" + repo + "/releases/download/models/manifest.json"};
    }
    std::vector<std::string> urls;
    urls.reserve(3);
    for (ModelSource src : model_source_chain(primary)) {
        urls.push_back(std::string(model_source_base_url(src)) + "/manifest.json");
    }
    return urls;
}

}  // namespace

auto is_china_locale() -> bool {
    if (const char* tz = std::getenv("TZ"); tz && *tz) {
        if (is_china_tz_name(tz)) return true;
    } else {
#ifdef _WIN32
        TIME_ZONE_INFORMATION tzi{};
        if (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID) {
            std::wstring_view std_name(tzi.StandardName);
            if (std_name.find(L"China Standard Time") != std::wstring_view::npos) return true;
        }
#else
        std::error_code ec;
        auto target = fs::read_symlink("/etc/localtime", ec);
        if (!ec && is_china_tz_name(target.string())) return true;
        std::ifstream tz_in("/etc/timezone");
        std::string line;
        if (tz_in && std::getline(tz_in, line) && is_china_tz_name(line)) return true;
#endif
    }

    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
        if (const char* val = std::getenv(var); val && *val && is_china_lang_tag(val)) {
            return true;
        }
    }
#ifdef _WIN32
    wchar_t locale_name[85]{};
    if (GetUserDefaultLocaleName(locale_name, 85) > 0) {
        std::wstring_view wloc(locale_name);
        if (wloc.starts_with(L"zh-CN") || wloc.starts_with(L"zh-Hans")) return true;
    }
#endif
    return false;
}

auto default_model_source() -> ModelSource {
    return is_china_locale() ? ModelSource::HfMirror : ModelSource::HuggingFace;
}

auto model_source_id(ModelSource src) -> std::string_view {
    switch (src) {
        case ModelSource::HfMirror:    return "hf-mirror";
        case ModelSource::HuggingFace: return "hf";
        case ModelSource::GitHub:      return "github";
    }
    return "hf";
}

auto parse_model_source(std::string_view id) -> std::optional<ModelSource> {
    if (id == "hf-mirror") return ModelSource::HfMirror;
    if (id == "hf" || id == "huggingface") return ModelSource::HuggingFace;
    if (id == "github") return ModelSource::GitHub;
    return std::nullopt;
}

auto model_source_base_url(ModelSource src) -> std::string_view {
    switch (src) {
        case ModelSource::HfMirror:    return kHfMirrorBaseUrl;
        case ModelSource::HuggingFace: return kHfBaseUrl;
        case ModelSource::GitHub:      return kGithubReleaseBaseUrl;
    }
    return kHfBaseUrl;
}

auto model_source_chain(ModelSource primary) -> std::vector<ModelSource> {
    switch (primary) {
        case ModelSource::HfMirror:
            return {ModelSource::HfMirror, ModelSource::HuggingFace, ModelSource::GitHub};
        case ModelSource::HuggingFace:
            return {ModelSource::HuggingFace, ModelSource::GitHub};
        case ModelSource::GitHub:
            return {ModelSource::GitHub, ModelSource::HuggingFace};
    }
    return {ModelSource::HuggingFace, ModelSource::GitHub};
}

auto fetch_remote_models_or_catalog(const std::string& repo)
    -> RemoteCatalogResult {
    return fetch_remote_models_or_catalog(repo, default_model_source());
}

auto fetch_remote_models_or_catalog(const std::string& repo,
                                    ModelSource source)
    -> RemoteCatalogResult {
    std::string err;
    if (auto remote = fetch_remote_models(repo, source, err)) {
        return {std::move(*remote), std::nullopt};
    }
    return {catalog_models(), err};
}

auto fetch_remote_models_or_catalog(const std::string& repo,
                                    std::string& error_out)
    -> RemoteCatalogResult {
    error_out.clear();
    if (auto remote = fetch_remote_models(repo, default_model_source(), error_out)) {
        return {std::move(*remote), std::nullopt};
    }
    return {catalog_models(), error_out};
}

auto manifest_url(const std::string& repo) -> std::string {
    if (!repo.empty()) {
        return "https://github.com/" + repo + "/releases/download/models/manifest.json";
    }
    return std::string(model_source_base_url(default_model_source())) + "/manifest.json";
}

auto fetch_remote_models(const std::string& repo, std::string& error)
    -> std::optional<std::vector<RemoteModel>> {
    return fetch_remote_models(repo, default_model_source(), error);
}

auto fetch_remote_models(const std::string& repo, ModelSource source, std::string& error)
    -> std::optional<std::vector<RemoteModel>> {
    std::string last_err;
    for (const auto& url : manifest_urls(source, repo)) {
        std::string body;
        std::string req_err;
        if (!http_get(url, body, req_err)) {
            last_err = std::move(req_err);
            continue;
        }

        ManifestJson parsed;
        auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, body);
        if (err) {
            last_err = "failed to parse manifest: " + glz::format_error(err, body);
            continue;
        }
        if (parsed.version != 1) {
            last_err = std::format(
                "unsupported manifest version {} (this build understands version 1)",
                parsed.version);
            continue;
        }

        std::vector<RemoteModel> out;
        out.reserve(parsed.models.size());
        for (auto& m : parsed.models) {
            out.push_back(RemoteModel{std::move(m.name), m.scale, m.size_bytes,
                                      std::move(m.sha256), std::move(m.url)});
        }
        error.clear();
        return out;
    }

    error = last_err.empty() ? "failed to fetch manifest" : std::move(last_err);
    return std::nullopt;
}

auto download_model(const RemoteModel& model,
                    const std::string& models_dir,
                    const std::function<void(const DownloadProgress&)>& on_progress,
                    UrlPolicy policy) -> ModelDownloadResult {
    return download_model(model, models_dir, on_progress, default_model_source(), policy);
}

auto download_model(const RemoteModel& model,
                    const std::string& models_dir,
                    const std::function<void(const DownloadProgress&)>& on_progress,
                    ModelSource source,
                    UrlPolicy policy) -> ModelDownloadResult {
    ModelDownloadResult res;
    if (model.name.empty() || model.name.find_first_of("/\\") != std::string::npos) {
        res.message = "model name must be a nonempty filename without path separators";
        return res;
    }
    std::error_code ec;
    fs::path dir(models_dir);
    fs::create_directories(dir, ec);
    if (ec) {
        res.message = std::format("cannot create models directory {}: {}",
                                  dir.string(), ec.message());
        return res;
    }

    std::string expected = model.sha256;
    if (expected.empty()) {
        res.message = "manifest entry has no sha256 — refusing to install an "
                      "unverified download";
        return res;
    }
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string error;
    fs::path stage = create_stage(dir, error);
    if (stage.empty()) {
        res.message = error;
        return res;
    }
    auto discard_stage = [&] {
        std::error_code ignored;
        fs::remove_all(stage, ignored);
    };

    std::vector<std::string> urls;
    if (policy == UrlPolicy::HttpsOnly && is_official_model_url(model.url)) {
        for (ModelSource s : model_source_chain(source)) {
            urls.push_back(std::string(model_source_base_url(s)) + "/" + model.name + ".zip");
        }
    } else {
        urls.push_back(model.url);
    }

    fs::path zip_path = stage / "download.zip";
    bool downloaded_ok = false;
    std::string last_err;
    for (const auto& url : urls) {
        std::string dl_err;
        if (!http_download_to_file(url, zip_path, model.size_bytes, on_progress, dl_err, policy)) {
            std::error_code rm_ec;
            fs::remove(zip_path, rm_ec);
            last_err = std::move(dl_err);
            continue;
        }
        std::string hash_err;
        std::string hash = sha256_file(zip_path, hash_err);
        if (!hash_err.empty() || hash != expected) {
            std::error_code rm_ec;
            fs::remove(zip_path, rm_ec);
            last_err = hash_err.empty()
                ? std::format("sha256 mismatch: expected {}, got {}", expected, hash)
                : std::move(hash_err);
            continue;
        }
        downloaded_ok = true;
        break;
    }
    if (!downloaded_ok) {
        discard_stage();
        res.message = last_err.empty() ? "download failed" : std::move(last_err);
        return res;
    }

    fs::path extracted = stage / "extracted";
    fs::create_directory(extracted, ec);
    if (ec || !extract_zip(zip_path, extracted, model.name, error)) {
        discard_stage();
        res.message = ec ? std::format("cannot create extraction directory: {}", ec.message())
                         : error;
        return res;
    }

    const fs::path param = dir / (model.name + ".param");
    const fs::path bin = dir / (model.name + ".bin");
    const fs::path staged_param = extracted / param.filename();
    const fs::path staged_bin = extracted / bin.filename();
    const fs::path backup_param = stage / "previous.param";
    const fs::path backup_bin = stage / "previous.bin";
    bool had_param = false;
    bool had_bin = false;
    bool saved_param = false;
    bool saved_bin = false;
    bool installed_param = false;
    bool installed_bin = false;

    auto inspect_destination = [&](const fs::path& path, bool& present) {
        auto link_status = fs::symlink_status(path, ec);
        if (link_status.type() == fs::file_type::not_found &&
            (!ec || ec == std::errc::no_such_file_or_directory)) {
            ec.clear();
            present = false;
            return true;
        }
        if (ec) return false;
        present = true;
        if (link_status.type() == fs::file_type::regular) return true;
        if (link_status.type() == fs::file_type::symlink) {
            auto target_status = fs::status(path, ec);
            if (ec) return false;
            if (target_status.type() == fs::file_type::regular) return true;
        }
        error = std::format("existing model destination {} is not a regular file",
                            path.string());
        return false;
    };
    if (!inspect_destination(param, had_param) || !inspect_destination(bin, had_bin)) {
        if (error.empty()) {
            error = std::format("cannot inspect existing model files: {}", ec.message());
        }
        res.message = error;
        discard_stage();
        return res;
    }
    auto rename_file = [&](const fs::path& from, const fs::path& to) {
        fs::rename(from, to, ec);
        return !ec;
    };
    auto rollback = [&]() -> std::string {
        std::string failures;
        auto remove_installed = [&](const fs::path& path, bool installed) {
            if (!installed) return;
            std::error_code rollback_ec;
            fs::remove(path, rollback_ec);
            if (rollback_ec) failures += std::format("; cannot remove {}: {}",
                                                       path.string(), rollback_ec.message());
        };
        remove_installed(param, installed_param);
        remove_installed(bin, installed_bin);
        auto restore = [&](const fs::path& backup, const fs::path& dest, bool saved) {
            if (!saved) return;
            std::error_code rollback_ec;
            fs::rename(backup, dest, rollback_ec);
            if (rollback_ec) failures += std::format("; cannot restore {} from {}: {}",
                dest.string(), backup.string(), rollback_ec.message());
        };
        restore(backup_param, param, saved_param);
        restore(backup_bin, bin, saved_bin);
        return failures;
    };

    bool committed = true;
    if (had_param) {
        committed = rename_file(param, backup_param);
        saved_param = committed;
    }
    if (committed && had_bin) {
        committed = rename_file(bin, backup_bin);
        saved_bin = committed;
    }
    if (committed) {
        committed = rename_file(staged_param, param);
        installed_param = committed;
    }
    if (committed) {
        committed = rename_file(staged_bin, bin);
        installed_bin = committed;
    }
    if (!committed) {
        std::string failure = error.empty() ? ec.message() : error;
        std::string rollback_error = rollback();
        if (rollback_error.empty()) {
            discard_stage();
            res.message = std::format("model install failed: {} (previous files restored)", failure);
        } else {
            res.message = std::format(
                "model install failed: {}; rollback also failed{}. Recovery backups and staged files are in {}",
                failure, rollback_error, stage.string());
        }
        return res;
    }

    res.param_path = param.string();
    res.bin_path = bin.string();
    res.ok = true;
    discard_stage();
    return res;
}

}  // namespace upix
