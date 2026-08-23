// AVIF container sniff + optional system-libavif round-trip.
// Built as `test_image_avif`. The round-trip is skipped when libavif is absent.
#include "utils/image.hpp"
#include "utils/image_avif.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace crossscale;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

static auto make_ispe_stub(uint32_t width, uint32_t height) -> std::vector<uint8_t> {
    auto be = [](uint32_t value) -> std::vector<uint8_t> {
        return {
            static_cast<uint8_t>(value >> 24),
            static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value)
        };
    };
    std::vector<uint8_t> out;
    auto append = [&](const void* data, size_t n) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + n);
    };
    auto append_be = [&](uint32_t value) {
        const auto encoded = be(value);
        out.insert(out.end(), encoded.begin(), encoded.end());
    };

    append_be(20);
    append("ftyp", 4);
    append("avif", 4);
    append_be(0);
    append("avif", 4);

    append_be(48);
    append("meta", 4);
    append_be(0);
    append_be(36);
    append("iprp", 4);
    append_be(28);
    append("ipco", 4);
    append_be(20);
    append("ispe", 4);
    append_be(0);
    append_be(width);
    append_be(height);
    return out;
}

int main() {
    CHECK(!avif_codec::looks_like_avif(nullptr, 0));

    const uint8_t png_sig[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    CHECK(!avif_codec::looks_like_avif(png_sig, sizeof(png_sig)));

    const auto stub = make_ispe_stub(320, 240);
    CHECK(avif_codec::looks_like_avif(stub.data(), stub.size()));
    const auto sniffed = avif_codec::sniff_dimensions(stub.data(), stub.size());
    CHECK(sniffed.first == 320);
    CHECK(sniffed.second == 240);

    Image src(16, 16, 3);
    for (int i = 0; i < 16 * 16; ++i) {
        src.data()[i * 3 + 0] = static_cast<uint8_t>(i);
        src.data()[i * 3 + 1] = static_cast<uint8_t>(255 - i);
        src.data()[i * 3 + 2] = 128;
    }
    auto encoded = src.save_to_memory("avif", 80);
    if (!encoded) {
        std::fprintf(stderr, "SKIP avif round-trip: %s\n", encoded.error().message.c_str());
        return 0;
    }
    CHECK(avif_codec::looks_like_avif(encoded->data(), encoded->size()));
    auto loaded = avif_codec::load(*encoded);
    CHECK(loaded.has_value());
    CHECK(loaded->width() == 16);
    CHECK(loaded->height() == 16);
    CHECK(loaded->channels() == 3);

    int max_delta = 0;
    for (int i = 0; i < 16 * 16 * 3; ++i) {
        max_delta = std::max(max_delta, std::abs(src.data()[i] - loaded->data()[i]));
    }
    CHECK(max_delta < 40);
    std::fprintf(stderr, "OK avif round-trip (max delta %d)\n", max_delta);
    return 0;
}
