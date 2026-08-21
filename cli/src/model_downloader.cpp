// See model_downloader.hpp. libcurl for HTTP(S), glaze for the manifest JSON,
// OpenSSL EVP for sha256, libzip for extraction.
#include "model_downloader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include <curl/curl.h>
#include <zip.h>
#include <openssl/evp.h>
#include <glaze/glaze.hpp>

namespace fs = std::filesystem;

namespace crossscale::cli {
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

auto http_get(const std::string& url, std::string& body, std::string& error,
              long timeout_s = 30) -> bool {
    ensure_curl();
    CurlPtr curl{curl_easy_init()};
    if (!curl) {
        error = "failed to init libcurl";
        return false;
    }
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);  // GitHub → S3 redirect
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                     +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                         auto* s = static_cast<std::string*>(ud);
                         s->append(ptr, size * nmemb);
                         return size * nmemb;
                     });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "crossscale-cli/0.1");

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        error = std::string("HTTP request failed: ") + curl_easy_strerror(rc);
        return false;
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    // HTTP must return 200; other protocols (file://) report 0 and rely on rc.
    if (status != 200 && status != 0) {
        std::ostringstream oss;
        oss << "HTTP " << status << " for " << url;
        error = oss.str();
        return false;
    }
    return true;
}

struct FileCtx {
    std::ofstream* out;
    DownloadProgress progress;
    const std::function<void(const DownloadProgress&)>* on_progress;
};

auto http_download_to_file(const std::string& url, const fs::path& dest,
                           std::uint64_t expected_size,
                           const std::function<void(const DownloadProgress&)>& on_progress,
                           std::string& error) -> bool {
    ensure_curl();
    CurlPtr curl{curl_easy_init()};
    if (!curl) {
        error = "failed to init libcurl";
        return false;
    }

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot open " + dest.string() + " for writing";
        return false;
    }

    FileCtx ctx{&out, DownloadProgress{0, expected_size}, &on_progress};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                     +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                         auto* c = static_cast<FileCtx*>(ud);
                         c->out->write(ptr, static_cast<std::streamsize>(size * nmemb));
                         if (!c->out->good()) return 0;  // abort transfer
                         c->progress.downloaded += size * nmemb;
                         if (c->on_progress && *c->on_progress) {
                             (*c->on_progress)(c->progress);
                         }
                         return size * nmemb;
                     });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 0L);          // no total timeout
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);  // stall guard:
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);  // <1B/s for 60s → abort
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "crossscale-cli/0.1");

    CURLcode rc = curl_easy_perform(curl.get());
    out.close();
    if (rc != CURLE_OK) {
        error = std::string("download failed: ") + curl_easy_strerror(rc);
        return false;
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    // HTTP must return 200; other protocols (file://) report 0 and rely on rc.
    if (status != 200 && status != 0) {
        std::ostringstream oss;
        oss << "HTTP " << status << " for " << url;
        error = oss.str();
        return false;
    }
    return true;
}

auto sha256_file(const fs::path& path, std::string& error) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return {};
    }
    struct Free { void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); } };
    std::unique_ptr<EVP_MD_CTX, Free> ctx{EVP_MD_CTX_new()};
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);

    std::array<char, 64 * 1024> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto got = in.gcount();
        if (got > 0) {
            EVP_DigestUpdate(ctx.get(), buf.data(), static_cast<size_t>(got));
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx.get(), digest, &len);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i) oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

// ── zip extraction (libzip) ─────────────────────────────────────────────────

// Extract all .param/.bin entries into out_dir (flattened: entry base name
// only, so a zip containing foo.param + foo.bin lands as models/foo.*).
auto extract_zip(const fs::path& zip_path, const fs::path& out_dir,
                 std::vector<fs::path>& written, std::string& error) -> bool {
    int err_code = 0;
    zip_t* raw = zip_open(zip_path.c_str(), ZIP_RDONLY, &err_code);
    if (!raw) {
        zip_error_t zerr;
        zip_error_init_with_code(&zerr, err_code);
        error = std::string("cannot open zip: ") + zip_error_strerror(&zerr);
        zip_error_fini(&zerr);
        return false;
    }
    struct ZipClose { void operator()(zip_t* p) const { zip_close(p); } };
    std::unique_ptr<zip_t, ZipClose> zip{raw};

    zip_int64_t count = zip_get_num_entries(zip.get(), 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        const char* name = zip_get_name(zip.get(), i, ZIP_FL_ENC_GUESS);
        if (!name) continue;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!lower.ends_with(".param") && !lower.ends_with(".bin")) continue;

        std::string base = std::strrchr(name, '/') ? std::strrchr(name, '/') + 1 : name;
        fs::path dest = out_dir / base;
        struct FileClose { void operator()(zip_file_t* p) const { zip_fclose(p); } };
        std::unique_ptr<zip_file_t, FileClose> entry{zip_fopen_index(zip.get(), i, 0)};
        if (!entry) {
            error = "cannot read zip entry '" + std::string(name) + "'";
            return false;
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + dest.string();
            return false;
        }
        char buf[64 * 1024];
        zip_int64_t got;
        while ((got = zip_fread(entry.get(), buf, sizeof buf)) > 0) {
            out.write(buf, static_cast<std::streamsize>(got));
        }
        out.close();
        if (!out.good()) {
            error = "short write for " + dest.string();
            return false;
        }
        written.push_back(dest);
    }
    return true;
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

auto manifest_url(const std::string& repo) -> std::string {
    std::string_view r = repo.empty() ? kDefaultModelRepo : repo;
    return "https://github.com/" + std::string(r) + "/releases/download/models/manifest.json";
}

auto fetch_remote_models(const std::string& repo, std::string& error)
    -> std::optional<std::vector<RemoteModel>> {
    std::string body;
    if (!http_get(manifest_url(repo), body, error)) {
        return std::nullopt;
    }

    ManifestJson parsed;
    auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, body);
    if (err) {
        error = "failed to parse manifest: " + glz::format_error(err, body);
        return std::nullopt;
    }
    if (parsed.version != 1) {
        error = "unsupported manifest version " + std::to_string(parsed.version) +
                " (this build understands version 1)";
        return std::nullopt;
    }

    std::vector<RemoteModel> out;
    out.reserve(parsed.models.size());
    for (auto& m : parsed.models) {
        out.push_back(RemoteModel{std::move(m.name), m.scale, m.size_bytes,
                                  std::move(m.sha256), std::move(m.url)});
    }
    return out;
}

auto download_model(const RemoteModel& model,
                    const std::string& models_dir,
                    const std::function<void(const DownloadProgress&)>& on_progress)
    -> ModelDownloadResult {
    ModelDownloadResult res;
    std::error_code ec;
    fs::path dir(models_dir);
    fs::create_directories(dir, ec);
    if (ec) {
        res.message = "cannot create models directory " + dir.string() + ": " + ec.message();
        return res;
    }

    // 1. Download zip to a temp file in the destination directory.
    fs::path zip_path = dir / (model.name + ".zip.part");
    std::string error;
    if (!http_download_to_file(model.url, zip_path, model.size_bytes, on_progress, error)) {
        fs::remove(zip_path, ec);
        res.message = error;
        return res;
    }

    // 2. Verify sha256.
    std::string expected = model.sha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string hash = sha256_file(zip_path, error);
    if (!error.empty() || hash != expected) {
        fs::remove(zip_path, ec);
        res.message = error.empty() ? ("sha256 mismatch: expected " + expected + ", got " + hash)
                                    : error;
        return res;
    }

    // 3. Extract .param/.bin into the models directory.
    std::vector<fs::path> written;
    if (!extract_zip(zip_path, dir, written, error)) {
        fs::remove(zip_path, ec);
        res.message = error;
        return res;
    }
    fs::remove(zip_path, ec);
    if (written.size() != 2) {
        for (const auto& p : written) fs::remove(p, ec);
        res.message = "zip did not contain exactly one .param + one .bin (got " +
                      std::to_string(written.size()) + " files)";
        return res;
    }
    for (const auto& p : written) {
        if (p.extension() == ".param") res.param_path = p.string();
        if (p.extension() == ".bin") res.bin_path = p.string();
    }

    res.ok = true;
    return res;
}

}  // namespace crossscale::cli
