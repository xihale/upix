// End-to-end checks for staged model downloads through curl's file:// protocol.
// Run: xmake build test_model_downloader && cli/build/test_model_downloader
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <zip.h>

#include "model_downloader.hpp"

namespace fs = std::filesystem;
using upix::cli::RemoteModel;
using Entry = std::pair<std::string, std::string>;

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

static bool make_zip(const fs::path& path, const std::vector<Entry>& entries) {
    int error = 0;
    zip_t* zip = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!zip) return false;
    for (const auto& [name, data] : entries) {
        zip_source_t* source = zip_source_buffer(zip, data.data(), data.size(), 0);
        if (!source || zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
            if (source) zip_source_free(source);
            zip_discard(zip);
            return false;
        }
    }
    return zip_close(zip) == 0;
}

static RemoteModel model_for(const std::string& name, const fs::path& zip) {
    return {name, 2, fs::file_size(zip), sha256_hex(zip), "file://" + zip.string()};
}

// This suite builds its zips locally and "downloads" them over file://, which
// is exactly what the production default (UrlPolicy::HttpsOnly) refuses — a
// manifest's url is remote input, so an unfiltered scheme would let a hostile
// repo copy an arbitrary local file into the models dir.
static constexpr upix::cli::UrlPolicy kPolicy =
    upix::cli::UrlPolicy::AllowLocalFileForTests;

static bool no_stage_residue(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().starts_with(".upix-model-stage-")) return false;
    }
    return true;
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "upix-dl-test";
    fs::remove_all(tmp);
    fs::path models_dir = tmp / "models";
    fs::create_directories(models_dir);

    fs::path valid_zip = tmp / "valid.zip";
    if (!make_zip(valid_zip, {{"foo.param", "param-one"}, {"foo.bin", "bin-one"},
                              {"metadata/readme.txt", "ignored"}}))
        return std::puts("valid zip build failed"), 1;
    RemoteModel valid = model_for("foo", valid_zip);
    auto first = upix::cli::download_model(valid, models_dir.string(), {}, kPolicy);
    if (!first.ok || read_file(models_dir / "foo.param") != "param-one" ||
        read_file(models_dir / "foo.bin") != "bin-one" || !no_stage_residue(models_dir))
        return std::fprintf(stderr, "valid install failed: %s\n", first.message.c_str()), 1;

    fs::path replacement_zip = tmp / "replacement.zip";
    if (!make_zip(replacement_zip, {{"foo.param", "param-two"}, {"foo.bin", "bin-two"}}))
        return std::puts("replacement zip build failed"), 1;
    auto replacement = upix::cli::download_model(model_for("foo", replacement_zip),
                                                   models_dir.string(), {}, kPolicy);
    if (!replacement.ok || read_file(models_dir / "foo.param") != "param-two" ||
        read_file(models_dir / "foo.bin") != "bin-two" || !no_stage_residue(models_dir))
        return std::puts("valid replacement failed"), 1;

    const std::vector<std::vector<Entry>> invalid_archives = {
        {{"foo.param", "only-param"}},
        {{"foo.param", "p"}, {"other.param", "extra-p"}, {"foo.bin", "b"}},
        {{"other.param", "wrong-name"}, {"foo.bin", "b"}},
        {{"foo.param", "p1"}, {"nested/foo.param", "p2"}, {"foo.bin", "b"}},
        {{"FOO.param", "case-variant"}, {"foo.bin", "b"}},
        {{"foo.PARAM", "case-variant-extension"}, {"foo.bin", "b"}},
    };
    for (std::size_t i = 0; i < invalid_archives.size(); ++i) {
        fs::path invalid_zip = tmp / ("invalid-" + std::to_string(i) + ".zip");
        if (!make_zip(invalid_zip, invalid_archives[i]))
            return std::puts("invalid zip build failed"), 1;
        auto rejected = upix::cli::download_model(model_for("foo", invalid_zip),
                                                    models_dir.string(), {}, kPolicy);
        if (rejected.ok || read_file(models_dir / "foo.param") != "param-two" ||
            read_file(models_dir / "foo.bin") != "bin-two" || !no_stage_residue(models_dir))
            return std::puts("invalid archive changed installed model or left residue"), 1;
    }

    RemoteModel bad_sha = valid;
    bad_sha.sha256 = "deadbeef";
    auto rejected_sha = upix::cli::download_model(bad_sha, models_dir.string(), {}, kPolicy);
    if (rejected_sha.ok || read_file(models_dir / "foo.param") != "param-two" ||
        read_file(models_dir / "foo.bin") != "bin-two" || !no_stage_residue(models_dir))
        return std::puts("bad sha changed installed model or left residue"), 1;

    const fs::path outside_param = tmp / "escaped.param";
    const fs::path outside_bin = tmp / "escaped.bin";
    write_file(outside_param, "outside-param");
    write_file(outside_bin, "outside-bin");
    const fs::path path_zip = tmp / "path-name.zip";
    if (!make_zip(path_zip, {{"../escaped.param", "new-param"}, {"../escaped.bin", "new-bin"}}))
        return std::puts("path-name zip build failed"), 1;
    auto rejected_path = upix::cli::download_model(model_for("../escaped", path_zip),
                                                  models_dir.string(), {}, kPolicy);
    if (rejected_path.ok || read_file(outside_param) != "outside-param" ||
        read_file(outside_bin) != "outside-bin" || !no_stage_residue(models_dir))
        return std::puts("model name escaped installation directory"), 1;

    fs::remove(models_dir / "foo.bin");
    fs::create_directories(models_dir / "foo.bin");
    write_file(models_dir / "foo.bin" / "keep.txt", "unrelated contents");
    auto rejected_destination = upix::cli::download_model(
        model_for("foo", replacement_zip), models_dir.string(), {}, kPolicy);
    if (rejected_destination.ok || read_file(models_dir / "foo.param") != "param-two" ||
        read_file(models_dir / "foo.bin" / "keep.txt") != "unrelated contents" ||
        !no_stage_residue(models_dir))
        return std::puts("nonregular destination was changed or left stage residue"), 1;

    fs::remove_all(tmp);
    std::puts("test_model_downloader: all OK");
    return 0;
}
