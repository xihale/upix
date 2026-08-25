// Minimal end-to-end check for the model download path (no network): builds a
// DEFLATE zip on disk, serves it via curl's file:// protocol, and asserts
// download_model verifies sha256 and extracts <name>.param/.bin correctly.
// Run: xmake build test_model_downloader && cli/build/test_model_downloader
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

#include <openssl/evp.h>
#include <zip.h>

#include "model_downloader.hpp"

namespace fs = std::filesystem;
using upix::cli::RemoteModel;

static void write_file(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

static std::string sha256_hex(const fs::path& p) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::string bytes = read_file(p);
    EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    char hex[65];
    for (unsigned int i = 0; i < len; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "upix-dl-test";
    fs::remove_all(tmp);
    fs::path models_dir = tmp / "models";
    fs::create_directories(tmp);

    // 1. Model files + DEFLATE zip with full-name entries (publish layout).
    write_file(tmp / "foo.param", "param-bytes");
    write_file(tmp / "foo.bin", "bin-bytes");
    fs::path zip_path = tmp / "foo.zip";
    zip_t* z = zip_open(zip_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    if (!z) return std::puts("zip_open failed"), 1;
    zip_source_t* s1 = zip_source_buffer(z, "param-bytes", 11, 0);
    zip_source_t* s2 = zip_source_buffer(z, "bin-bytes", 9, 0);
    bool ok = zip_file_add(z, "foo.param", s1, ZIP_FL_ENC_UTF_8) >= 0 &&
              zip_file_add(z, "foo.bin", s2, ZIP_FL_ENC_UTF_8) >= 0 &&
              zip_close(z) == 0;
    if (!ok) return std::puts("zip build failed"), 1;

    RemoteModel model{"foo", 2, fs::file_size(zip_path), sha256_hex(zip_path),
                      "file://" + zip_path.string()};

    // 2. Happy path.
    auto res = upix::cli::download_model(model, models_dir.string(), {});
    if (!res.ok || read_file(models_dir / "foo.param") != "param-bytes" ||
        read_file(models_dir / "foo.bin") != "bin-bytes" ||
        fs::exists(models_dir / "foo.zip.part")) {
        return std::puts("happy path failed"), 1;
    }

    // 3. Bad sha256 → rejected, nothing installed.
    RemoteModel bad = model;
    bad.sha256 = "deadbeef";
    auto res2 = upix::cli::download_model(bad, models_dir.string(), {});
    if (res2.ok || res2.message.find("sha256 mismatch") == std::string::npos ||
        fs::exists(models_dir / "foo.zip.part")) {
        return std::puts("bad-sha path failed"), 1;
    }
    fs::remove_all(tmp);
    std::puts("test_model_downloader: all OK");
    return 0;
}
