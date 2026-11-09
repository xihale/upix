// SR XMP marker: build/probe round-trips through every encoder this
// codebase ships (PNG/JPEG/WebP/AVIF) plus escape and negative cases.
// Built as `test_sr_marker`. The AVIF leg is skipped when libavif is absent.
#include "utils/image.hpp"
#include "utils/sr_marker.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace upix;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

static auto make_test_image(int width, int height) -> Image {
    Image img(width, height, 3);
    for (int i = 0; i < width * height; ++i) {
        img.data()[i * 3 + 0] = static_cast<uint8_t>(i);
        img.data()[i * 3 + 1] = static_cast<uint8_t>(255 - i);
        img.data()[i * 3 + 2] = 128;
    }
    return img;
}

// Encode via save_to_memory with a marker, then probe the encoded bytes.
// Returns false when the encoder is unavailable (AVIF without libavif).
static auto round_trip(const std::string& format, const std::string& model,
                       int scale, int64_t ts) -> bool {
    auto img = make_test_image(16, 16);
    auto encoded = img.save_to_memory(format, 90, sr_marker::build_xmp(model, scale, ts));
    if (!encoded) {
        std::fprintf(stderr, "SKIP %s round-trip: %s\n",
                     format.c_str(), encoded.error().message.c_str());
        return false;
    }
    // Unmarked control first: same pixels, no marker string — probe must
    // stay nullopt so the needle search cannot false-positive on pixels.
    auto plain = img.save_to_memory(format, 90);
    CHECK(plain.has_value());
    CHECK(!sr_marker::probe(plain->data(), plain->size()).has_value());

    auto marker = sr_marker::probe(encoded->data(), encoded->size());
    CHECK(marker.has_value());
    CHECK(marker->model == model);
    CHECK(marker->scale == scale);
    CHECK(marker->ts == ts);

    // The marked output must still decode (marker chunks do not corrupt
    // the container) and keep its dimensions.
    const std::string fmt = format == "jpeg" ? "jpg" : format;
    auto decoded = Image::load_from_memory(fmt, *encoded);
    CHECK(decoded.has_value());
    CHECK(decoded->width() == 16);
    CHECK(decoded->height() == 16);
    std::fprintf(stderr, "OK %s round-trip\n", format.c_str());
    return true;
}

int main() {
    // build_xmp → probe round-trip on the bare string.
    const auto xmp = sr_marker::build_xmp("realsr-anime64v3", 4, 1724500000);
    auto marker = sr_marker::probe(
        reinterpret_cast<const uint8_t*>(xmp.data()), xmp.size());
    CHECK(marker.has_value());
    CHECK(marker->model == "realsr-anime64v3");
    CHECK(marker->scale == 4);
    CHECK(marker->ts == 1724500000);

    // XML escaping: model names with markup characters survive the trip.
    const auto xmp_esc = sr_marker::build_xmp("a<b>&\"c\"", 2, 1);
    auto marker_esc = sr_marker::probe(
        reinterpret_cast<const uint8_t*>(xmp_esc.data()), xmp_esc.size());
    CHECK(marker_esc.has_value());
    CHECK(marker_esc->model == "a<b>&\"c\"");
    CHECK(marker_esc->scale == 2);

    // Needle not present / garbage / truncated.
    const uint8_t junk[] = "just some bytes, no marker here";
    CHECK(!sr_marker::probe(junk, sizeof(junk)).has_value());
    CHECK(!sr_marker::probe(nullptr, 0).has_value());
    CHECK(!sr_marker::probe(reinterpret_cast<const uint8_t*>(xmp.data()),
                            xmp.size() / 2).has_value());

    // Marker bytes embedded anywhere in a larger buffer (the real probe
    // scans whole files: XMP sits at different offsets per container).
    std::vector<uint8_t> padded(4096, 0xAB);
    padded.insert(padded.begin() + 2048, xmp.begin(), xmp.end());
    auto padded_marker = sr_marker::probe(padded.data(), padded.size());
    CHECK(padded_marker.has_value());
    CHECK(padded_marker->model == "realsr-anime64v3");
    CHECK(padded_marker->ts == 1724500000);

    // Container round-trips.
    CHECK(round_trip("png", "model-png", 2, 1724500001));
    CHECK(round_trip("jpg", "model-jpg", 3, 1724500002));
    CHECK(round_trip("webp", "model-webp", 4, 1724500003));
    (void)round_trip("avif", "model-avif", 2, 1724500004);

    // Advanced encoder knobs must not disturb the marker: same XMP payload,
    // different codec settings.
    {
        auto img = make_test_image(16, 16);
        const auto xmp2 = sr_marker::build_xmp("opts-model", 2, 1724500006);

        EncoderOptions jpg_opts;
        jpg_opts.jpeg_subsampling = 444;
        auto jpg444 = img.save_to_memory("jpg", 90, xmp2, jpg_opts);
        CHECK(jpg444.has_value());
        CHECK(sr_marker::probe(jpg444->data(), jpg444->size()).has_value());

        EncoderOptions webp_opts;
        webp_opts.webp_lossless = true;
        webp_opts.webp_method = 6;
        auto webpll = img.save_to_memory("webp", 90, xmp2, webp_opts);
        CHECK(webpll.has_value());
        CHECK(sr_marker::probe(webpll->data(), webpll->size()).has_value());

        EncoderOptions png_opts;
        png_opts.png_level = 9;
        auto png9 = img.save_to_memory("png", 90, xmp2, png_opts);
        CHECK(png9.has_value());
        CHECK(sr_marker::probe(png9->data(), png9->size()).has_value());

        EncoderOptions avif_opts;
        avif_opts.avif_speed = 10;
        avif_opts.avif_subsampling = 444;
        auto avif444 = img.save_to_memory("avif", 90, xmp2, avif_opts);
        if (avif444) {
            CHECK(sr_marker::probe(avif444->data(), avif444->size()).has_value());
        } else {
            std::fprintf(stderr, "SKIP avif opts round-trip\n");
        }
        std::fprintf(stderr, "OK marker under encoder options\n");
    }

    // Streaming-PNG path: >=128 MiB raw sends Image::save through
    // save_png_streaming (libpng, file-based) instead of the libdeflate
    // memory writer. The libpng writer must emit the same iTXt marker.
    {
        auto big = make_test_image(8192, 5464);  // ~134 MiB raw
        const std::filesystem::path big_path =
            std::filesystem::temp_directory_path() / "upix_sr_marker_streaming.png";
        const auto save_result = big.save(big_path, 61,
                                          sr_marker::build_xmp("streaming", 2, 1724500005));
        CHECK(save_result.has_value());
        auto raw = Image::load_raw(big_path);
        CHECK(raw.has_value());
        auto streaming_marker = sr_marker::probe(raw->data(), raw->size());
        CHECK(streaming_marker.has_value());
        CHECK(streaming_marker->model == "streaming");
        CHECK(streaming_marker->ts == 1724500005);
        std::filesystem::remove(big_path);
        std::fprintf(stderr, "OK png streaming round-trip\n");
    }

    std::fprintf(stderr, "OK sr_marker\n");
    return 0;
}
