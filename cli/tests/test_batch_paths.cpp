// Folder batch gathering, output path computation, original backups,
// header-only dimension probing (including malformed JPEG vectors), and
// separable bicubic alpha scaling checks. Built as `test_batch_paths`.
#include "core/batch.hpp"
#include "core/config.hpp"
#include "core/tile_processor.hpp"
#include "utils/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace upix;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

auto write_bytes(const fs::path& p, const std::string& data) -> void {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    CHECK(f.is_open());
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    CHECK(f.good());
}

auto read_bytes(const fs::path& p) -> std::string {
    std::ifstream f(p, std::ios::binary);
    CHECK(f.is_open());
    return {std::istreambuf_iterator<char>(f), {}};
}

auto bicubic_kernel_ref(float t) -> float {
    constexpr float a = -0.5f;
    t = std::abs(t);
    if (t <= 1.0f) {
        return (a + 2.0f) * t * t * t - (a + 3.0f) * t * t + 1.0f;
    }
    if (t < 2.0f) {
        return a * t * t * t - 5.0f * a * t * t + 8.0f * a * t - 4.0f * a;
    }
    return 0.0f;
}

void test_probe_header_vectors() {
    // Hostile JPEG with seg_len == 0 (FF E0 00 00) — must return nullopt without looping.
    const std::vector<uint8_t> zero_len_jpeg = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x00, 0xFF, 0xC0,
        0x00, 0x0B, 0x08, 0x00, 0x20, 0x00, 0x40, 0x03, 0x01, 0x11, 0x00
    };
    CHECK(!Image::probe_header_dimensions(zero_len_jpeg.data(), zero_len_jpeg.size()).has_value());

    // Hostile JPEG with seg_len == 1.
    const std::vector<uint8_t> one_len_jpeg = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    CHECK(!Image::probe_header_dimensions(one_len_jpeg.data(), one_len_jpeg.size()).has_value());

    // Valid JPEG with 0xFF fill bytes + standalone RST0 (0xD0) before SOF0 (64x32).
    const std::vector<uint8_t> padded_rst_jpeg = {
        0xFF, 0xD8,
        0xFF, 0xFF, 0xE0, 0x00, 0x04, 0x12, 0x34,
        0xFF, 0xD0,
        0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x20, 0x00, 0x40, 0x03, 0x01, 0x11, 0x00
    };
    auto jpg_dims = Image::probe_header_dimensions(padded_rst_jpeg.data(), padded_rst_jpeg.size());
    CHECK(jpg_dims.has_value());
    CHECK(jpg_dims->first == 64 && jpg_dims->second == 32);

    // WebP VP8L (lossless) 17x9 encoded as (w-1=16, h-1=8).
    const uint32_t vp8l_bits = (16u) | (8u << 14);
    const std::vector<uint8_t> vp8l = {
        'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'E', 'B', 'P',
        'V', 'P', '8', 'L', 0x10, 0, 0, 0, 0x2F,
        static_cast<uint8_t>(vp8l_bits & 0xFF),
        static_cast<uint8_t>((vp8l_bits >> 8) & 0xFF),
        static_cast<uint8_t>((vp8l_bits >> 16) & 0xFF),
        static_cast<uint8_t>((vp8l_bits >> 24) & 0xFF),
    };
    auto webp_dims = Image::probe_header_dimensions(vp8l.data(), vp8l.size());
    CHECK(webp_dims.has_value());
    CHECK(webp_dims->first == 17 && webp_dims->second == 9);

    // WebP VP8X 320x240 encoded as (w-1=319, h-1=239).
    const std::vector<uint8_t> vp8x = {
        'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'E', 'B', 'P',
        'V', 'P', '8', 'X', 0x0A, 0, 0, 0, 0, 0, 0, 0,
        0x3F, 0x01, 0x00,  // 319 -> 320
        0xEF, 0x00, 0x00,  // 239 -> 240
    };
    auto vp8x_dims = Image::probe_header_dimensions(vp8x.data(), vp8x.size());
    CHECK(vp8x_dims.has_value());
    CHECK(vp8x_dims->first == 320 && vp8x_dims->second == 240);
}

void test_probe_dimensions_on_disk(const fs::path& tmp) {
    Image img(29, 13, 3);
    std::memset(img.data(), 120, img.size());
    const fs::path png = tmp / "probe.png";
    const fs::path jpg = tmp / "probe.jpg";
    const fs::path webp = tmp / "probe.webp";
    CHECK(img.save(png, 90).has_value());
    CHECK(img.save(jpg, 90).has_value());
    CHECK(img.save(webp, 90).has_value());

    for (const auto& p : {png, jpg, webp}) {
        auto d = Image::probe_dimensions(p);
        CHECK(d.has_value());
        CHECK(d->first == 29 && d->second == 13);
    }
    CHECK(!Image::probe_dimensions(tmp / "does_not_exist.png").has_value());
}

void test_gather_and_paths(const fs::path& tmp) {
    const fs::path empty_in = tmp / "empty_in";
    const fs::path empty_out = tmp / "empty_out_should_not_exist";
    fs::create_directories(empty_in);
    std::string err;
    auto empty_tasks = gather_directory_tasks(empty_in.string(), empty_out.string(), err);
    CHECK(empty_tasks.has_value());
    CHECK(empty_tasks->empty());
    CHECK(!fs::exists(empty_out));

    const fs::path batch_root = tmp / "album";
    write_bytes(batch_root / "top.png", "p");
    write_bytes(batch_root / "sub" / "nested.jpg", "j");
    write_bytes(batch_root / "upix_AnimeSharp-x4" / "already.png", "skip");
    write_bytes(batch_root / "upix_orig" / "orig.png", "skip");
    write_bytes(batch_root / "sub" / "upix_orig" / "orig2.jpg", "skip");
    write_bytes(batch_root / "readme.txt", "ignore");

    const fs::path out_dir = tmp / "album_out";
    auto rec_tasks = gather_directory_tasks(batch_root.string(), out_dir.string(), err, true);
    CHECK(rec_tasks.has_value());
    CHECK(rec_tasks->size() == 2);
    CHECK(fs::exists(out_dir));

    const fs::path flat_out = tmp / "album_flat";
    auto flat_tasks = gather_directory_tasks(batch_root.string(), flat_out.string(), err, false);
    CHECK(flat_tasks.has_value());
    CHECK(flat_tasks->size() == 1);
    CHECK(fs::path(flat_tasks->front().first).filename() == "top.png");

    // compute_batch_output_path priority: overwrite > batch_root > custom_out_dir > sibling
    ProcessingConfig cfg;
    cfg.output_format = "png";
    const BatchInput folder_item{(batch_root / "sub" / "nested.jpg").string(), batch_root.string()};
    const BatchInput single_item{(tmp / "solo.jpg").string(), ""};

    CHECK(compute_batch_output_path(folder_item, "AnimeSharp-x4", cfg, "", true) ==
          batch_root / "sub" / "nested.jpg");
    CHECK(compute_batch_output_path(folder_item, "AnimeSharp-x4", cfg, "", false) ==
          batch_root / "upix_AnimeSharp-x4" / "sub" / "nested.png");
    CHECK(compute_batch_output_path(folder_item, "AnimeSharp-x4", cfg, (tmp / "exp").string(), false) ==
          tmp / "exp" / "sub" / "nested.png");
    CHECK(compute_batch_output_path(single_item, "AnimeSharp-x4", cfg, (tmp / "exp").string(), false) ==
          tmp / "exp" / "solo_AnimeSharp-x4.png");

    cfg.output_format.clear();  // Follow input
    CHECK(compute_batch_output_path(single_item, "AnimeSharp-x4", cfg, "", false) ==
          tmp / "solo_AnimeSharp-x4.jpg");
}

void test_backup_original_once(const fs::path& tmp) {
    const fs::path src = tmp / "photos" / "pic.png";
    write_bytes(src, "ORIGINAL_DATA");

    std::string err;
    CHECK(backup_original_once(src, err));
    const fs::path bak = tmp / "photos" / "upix_orig" / "pic.png";
    CHECK(fs::exists(bak));
    CHECK(read_bytes(bak) == "ORIGINAL_DATA");

    // Simulate second overwrite — existing backup must NEVER be overwritten.
    write_bytes(src, "UPSCALED_DATA");
    CHECK(backup_original_once(src, err));
    CHECK(read_bytes(bak) == "ORIGINAL_DATA");
}

void test_bicubic_separable() {
    // Constant stays constant.
    {
        std::vector<uint8_t> src(16 * 16, 137);
        std::vector<uint8_t> dst(32 * 32, 0);
        TileProcessor::bicubic_scale_u8(src.data(), 16, 16, dst.data(), 32, 32);
        for (uint8_t v : dst) CHECK(v == 137);
    }

    // Separable matches naive 2D Catmull-Rom within 1 ULP on non-integer ratio.
    const uint32_t sw = 23, sh = 19, dw = 61, dh = 47;
    std::vector<uint8_t> src(sw * sh);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<uint8_t>((i * 73 + 19) % 256);
    }
    std::vector<uint8_t> dst(dw * dh, 0);
    TileProcessor::bicubic_scale_u8(src.data(), sw, sh, dst.data(), dw, dh);

    const float sx_ratio = static_cast<float>(sw) / static_cast<float>(dw);
    const float sy_ratio = static_cast<float>(sh) / static_cast<float>(dh);
    for (uint32_t dy = 0; dy < dh; ++dy) {
        const float sy = (static_cast<float>(dy) + 0.5f) * sy_ratio - 0.5f;
        const int sy0 = static_cast<int>(std::floor(sy));
        for (uint32_t dx = 0; dx < dw; ++dx) {
            const float sx = (static_cast<float>(dx) + 0.5f) * sx_ratio - 0.5f;
            const int sx0 = static_cast<int>(std::floor(sx));
            float sum = 0.0f, wsum = 0.0f;
            for (int m = -1; m <= 2; ++m) {
                const int cy = std::clamp(sy0 + m, 0, static_cast<int>(sh) - 1);
                const float wy = bicubic_kernel_ref(sy - static_cast<float>(sy0 + m));
                for (int n = -1; n <= 2; ++n) {
                    const int cx = std::clamp(sx0 + n, 0, static_cast<int>(sw) - 1);
                    const float wx = bicubic_kernel_ref(sx - static_cast<float>(sx0 + n));
                    const float w = wx * wy;
                    sum += static_cast<float>(src[static_cast<size_t>(cy) * sw + static_cast<size_t>(cx)]) * w;
                    wsum += w;
                }
            }
            const uint8_t ref = static_cast<uint8_t>(
                std::clamp(std::abs(wsum) > 1e-6f ? sum / wsum : 0.0f, 0.0f, 255.0f));
            const uint8_t got = dst[static_cast<size_t>(dy) * dw + dx];
            CHECK(std::abs(static_cast<int>(got) - static_cast<int>(ref)) <= 1);
        }
    }
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "upix-batch-paths-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    test_probe_header_vectors();
    test_probe_dimensions_on_disk(tmp);
    test_gather_and_paths(tmp);
    test_backup_original_once(tmp);
    test_bicubic_separable();

    BatchRequest req;
    CHECK(!req.overwrite_original);
    CHECK(req.backup_original);
    BatchStats stats;
    CHECK(!stats.cancelled);
    CHECK(stats.failed() == 0);

    fs::remove_all(tmp);
    std::puts("test_batch_paths: all OK");
    return 0;
}
