// EXIF orientation: JPEG APP1 parse + pixel bake-in.
//
// (a) minimal JPEGs (Image::save_to_memory + a hand-built APP1 "Exif\0\0"
//     segment injected after SOI, IFD0 carrying exactly one Orientation
//     entry) must parse back as orientation 1-8;
// (b) every orientation 2-8 must transform a known gradient pixel-exactly
//     (checked against an independent, forward-mapping golden image) for
//     channels 1/3/4, plus dimension swaps for the transposing values;
// (c) truncated / mutated / hostile inputs must never read out of bounds
//     (every prefix of a valid file + seeded byte-flip fuzz + oversized
//     IFD entry counts).
//
// Built as `test_exif`; aborts on failure.
#include "utils/exif.hpp"
#include "utils/image.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace upix;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

// APP1 "Exif\0\0" segment whose little-endian TIFF IFD0 holds exactly one
// entry: Orientation (tag 0x0112, SHORT, count 1). Segment = FF E1 len16 +
// payload; total insertion unit for splicing after the SOI.
auto build_exif_app1(int orientation) -> std::vector<uint8_t> {
    std::vector<uint8_t> payload = {
        'E', 'x', 'i', 'f', 0, 0,
        // TIFF header, little-endian.
        'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
        // IFD0: 1 entry.
        0x01, 0x00,
        // Entry: tag 0x0112, type SHORT(3), count 1, value (LE u16) + pad.
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
        static_cast<uint8_t>(orientation & 0xFF),
        static_cast<uint8_t>((orientation >> 8) & 0xFF),
        0x00, 0x00,
        // next-IFD offset: none.
        0x00, 0x00, 0x00, 0x00,
    };
    std::vector<uint8_t> seg;
    seg.push_back(0xFF);
    seg.push_back(0xE1);
    const uint16_t seg_len = static_cast<uint16_t>(payload.size() + 2);
    seg.push_back(static_cast<uint8_t>(seg_len >> 8));
    seg.push_back(static_cast<uint8_t>(seg_len & 0xFF));
    seg.insert(seg.end(), payload.begin(), payload.end());
    return seg;
}

// Splice an EXIF APP1 segment right after the SOI of a JPEG byte stream.
// (Pointer-based insert ranges, not iterator ranges: GCC 16 -Warray-bounds
// fires a false positive on vector(begin(), begin()+2) here.)
auto inject_exif(const std::vector<uint8_t>& jpeg, int orientation)
    -> std::vector<uint8_t> {
    const auto* jb = jpeg.data();
    std::vector<uint8_t> out;
    out.reserve(jpeg.size() + 40);
    out.insert(out.end(), jb, jb + 2);
    const auto seg = build_exif_app1(orientation);
    out.insert(out.end(), seg.data(), seg.data() + seg.size());
    out.insert(out.end(), jb + 2, jb + jpeg.size());
    return out;
}

// Deterministic gradient with a unique value per (pixel, channel).
auto gradient_pixels(int w, int h, int c) -> std::vector<uint8_t> {
    std::vector<uint8_t> p(static_cast<size_t>(w) * h * c);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int k = 0; k < c; ++k) {
                p[(static_cast<size_t>(y) * w + x) * c + k] =
                    static_cast<uint8_t>((static_cast<size_t>(y) * w + x) * 8 + k * 3);
            }
        }
    }
    return p;
}

// Golden transform via the forward mapping (input-driven; the implementation
// works backward from output pixels, so this is an independent check):
//   2: in(x,y) -> out(w-1-x, y)          5: in(x,y) -> out(y, x)
//   3: in(x,y) -> out(w-1-x, h-1-y)      6: in(x,y) -> out(h-1-y, x)
//   4: in(x,y) -> out(x, h-1-y)          7: in(x,y) -> out(h-1-y, w-1-x)
//                                        8: in(x,y) -> out(y, w-1-x)
// Output dims stay w*h for 2-4 and become h*w for 5-8.
auto expected_dims(int w, int h, int orientation) -> std::pair<int, int> {
    return orientation >= 5 ? std::make_pair(h, w) : std::make_pair(w, h);
}

auto check_transform(int w, int h, int c, int orientation) -> void {
    auto pixels = gradient_pixels(w, h, c);
    const auto golden = pixels;  // pristine copy for the reference fill
    int ow = 0;
    int oh = 0;
    std::tie(ow, oh) = expected_dims(w, h, orientation);

    std::vector<uint8_t> ref(static_cast<size_t>(ow) * oh * c, 0xAB);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int dx = 0;
            int dy = 0;
            switch (orientation) {
                case 2: dx = w - 1 - x; dy = y;             break;
                case 3: dx = w - 1 - x; dy = h - 1 - y;     break;
                case 4: dx = x;         dy = h - 1 - y;     break;
                case 5: dx = y;         dy = x;             break;
                case 6: dx = h - 1 - y; dy = x;             break;
                case 7: dx = h - 1 - y; dy = w - 1 - x;     break;
                default: dx = y;        dy = w - 1 - x;     break;  // 8
            }
            for (int k = 0; k < c; ++k) {
                ref[(static_cast<size_t>(dy) * ow + dx) * c + k] =
                    golden[(static_cast<size_t>(y) * w + x) * c + k];
            }
        }
    }

    CHECK(exif::apply_exif_orientation_u8(pixels, w, h, c, orientation));
    CHECK(w == ow);
    CHECK(h == oh);
    CHECK(pixels.size() >= static_cast<size_t>(ow) * oh * c);
    CHECK(std::memcmp(pixels.data(), ref.data(),
                      static_cast<size_t>(ow) * oh * c) == 0);
}

// xorshift64* — deterministic fuzz source.
struct Rng {
    uint64_t s;
    auto next() -> uint64_t {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    auto below(uint64_t n) -> uint64_t { return n == 0 ? 0 : next() % n; }
};

}  // namespace

int main() {
    // ── (a) parse: every orientation round-trips through a real JPEG. ──────
    std::vector<uint8_t> jpeg;
    {
        Image img(16, 12, 3);
        auto px = gradient_pixels(16, 12, 3);
        std::memcpy(img.data(), px.data(), px.size());
        auto saved = img.save_to_memory("jpg", 92);
        CHECK(saved.has_value());
        jpeg = std::move(*saved);
        CHECK(jpeg.size() > 100);
        CHECK(jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    }
    CHECK(exif::parse_jpeg_exif_orientation(jpeg.data(), jpeg.size()) == 1);

    for (int o = 1; o <= 8; ++o) {
        const auto tagged = inject_exif(jpeg, o);
        const int parsed = exif::parse_jpeg_exif_orientation(
            tagged.data(), tagged.size());
        CHECK(parsed == o);
        // The injected marker must not disturb the decoder.
        auto decoded = Image::load_from_memory("jpg", tagged);
        CHECK(decoded.has_value());
        CHECK(decoded->width() == 16);
        CHECK(decoded->height() == 12);
        // Full local pipeline: decode → autorotate → dimensions swap for 5-8.
        if (o >= 5) {
            CHECK(exif::autorotate_image(*decoded, o));
            CHECK(decoded->width() == 12);
            CHECK(decoded->height() == 16);
        } else if (o >= 2) {
            CHECK(exif::autorotate_image(*decoded, o));
            CHECK(decoded->width() == 16);
            CHECK(decoded->height() == 12);
        } else {
            CHECK(!exif::autorotate_image(*decoded, o));
        }
        std::fprintf(stderr, "OK parse orientation=%d\n", o);
    }

    // ── (b) pixel-level transforms, every orientation, several geometries. ──
    for (int o = 2; o <= 8; ++o) {
        check_transform(7, 5, 3, o);    // odd, landscape
        check_transform(5, 7, 3, o);    // odd, portrait
        check_transform(6, 6, 3, o);    // square
        check_transform(1, 8, 3, o);    // degenerate width
        check_transform(8, 1, 3, o);    // degenerate height
        std::fprintf(stderr, "OK transform orientation=%d\n", o);
    }
    check_transform(9, 4, 1, 6);  // channels 1 and 4 on the classic portrait
    check_transform(9, 4, 4, 6);  // value
    check_transform(9, 4, 1, 8);
    check_transform(9, 4, 4, 8);
    check_transform(9, 4, 1, 3);
    check_transform(9, 4, 4, 3);
    std::fprintf(stderr, "OK transforms for channels 1/4\n");

    // Tail slack (Image's +16 upload slack) must survive every transform
    // untouched, and the vector size must not change.
    for (int o = 2; o <= 8; ++o) {
        auto pixels = gradient_pixels(7, 5, 3);
        pixels.resize(pixels.size() + 16, 0x5C);
        int w = 7;
        int h = 5;
        CHECK(exif::apply_exif_orientation_u8(pixels, w, h, 3, o));
        CHECK(pixels.size() == static_cast<size_t>(7) * 5 * 3 + 16);
        for (const uint8_t tail : std::vector<uint8_t>(
                 pixels.end() - 16, pixels.end())) {
            CHECK(tail == 0x5C);
        }
    }
    std::fprintf(stderr, "OK tail slack preserved\n");

    // ── invalid apply() arguments: refused, pixels untouched. ───────────────
    {
        auto pixels = gradient_pixels(6, 4, 3);
        const auto pristine = pixels;
        int w = 6;
        int h = 4;
        for (const int bad : {0, 1, 9, -1, 255}) {
            CHECK(!exif::apply_exif_orientation_u8(pixels, w, h, 3, bad));
        }
        CHECK(!exif::apply_exif_orientation_u8(pixels, w, h, 0, 6));
        CHECK(!exif::apply_exif_orientation_u8(pixels, w, h, 5, 6));
        CHECK(w == 6 && h == 4);
        CHECK(pixels == pristine);

        std::vector<uint8_t> short_buffer(10, 0);
        int sw = 6;
        int sh = 4;
        CHECK(!exif::apply_exif_orientation_u8(short_buffer, sw, sh, 3, 6));
        CHECK(short_buffer.size() == 10);
    }
    std::fprintf(stderr, "OK invalid apply arguments rejected\n");

    // ── (c) hostile / truncated parse inputs. ───────────────────────────────
    CHECK(exif::parse_jpeg_exif_orientation(nullptr, 0) == 1);
    CHECK(exif::parse_jpeg_exif_orientation(nullptr, 100) == 1);
    {
        const uint8_t soi[] = {0xFF, 0xD8};
        CHECK(exif::parse_jpeg_exif_orientation(soi, sizeof(soi)) == 1);
        const uint8_t not_jpeg[] = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
        CHECK(exif::parse_jpeg_exif_orientation(not_jpeg, sizeof(not_jpeg)) == 1);
    }

    const auto tagged6 = inject_exif(jpeg, 6);

    // Wrong APP1 namespace (XMP-style) is not EXIF.
    {
        std::vector<uint8_t> xmp_app1 = {
            0xFF, 0xE1, 0x00, 0x0E,
            'h', 't', 't', 'p', ':', '/', '/', 'x', 'm', 'p', 0, 0, 0,
        };
        std::vector<uint8_t> f(jpeg.begin(), jpeg.begin() + 2);
        f.insert(f.end(), xmp_app1.begin(), xmp_app1.end());
        f.insert(f.end(), jpeg.begin() + 2, jpeg.end());
        CHECK(exif::parse_jpeg_exif_orientation(f.data(), f.size()) == 1);
    }

    // Every prefix of a valid orientation-6 file: parse must return 1 or 6
    // and never touch memory beyond the buffer (ASan/valgrind territory,
    // but the loop itself is the regression guard).
    for (size_t n = 0; n <= tagged6.size(); ++n) {
        const int v = exif::parse_jpeg_exif_orientation(tagged6.data(), n);
        CHECK(v == 1 || v == 6);
    }
    std::fprintf(stderr, "OK all %zu prefixes of a valid file\n", tagged6.size());

    // Seeded byte-flip fuzz on the valid file: any mutation is fine as long
    // as parsing stays in 1-8 and does not crash.
    {
        Rng rng{0x5eed1234u};
        std::vector<uint8_t> buf = tagged6;
        for (int iter = 0; iter < 5000; ++iter) {
            for (int flips = 0; flips < 4; ++flips) {
                buf[rng.below(buf.size())] =
                    static_cast<uint8_t>(rng.next() & 0xFF);
            }
            const int v = exif::parse_jpeg_exif_orientation(buf.data(), buf.size());
            CHECK(v >= 1 && v <= 8);
        }
    }
    std::fprintf(stderr, "OK 5000 mutation iterations\n");

    // Oversized IFD entry count (0xFFFF) inside a small segment, plus a
    // next-IFD style offset far past the segment: both must read as 1.
    {
        std::vector<uint8_t> hostile = {
            0xFF, 0xD8,
            0xFF, 0xE1, 0x00, 0x16,             // APP1, len 22 (tiny)
            'E', 'x', 'i', 'f', 0, 0,
            'I', 'I', 0x2A, 0x00,
            0xFF, 0xFF, 0xFF, 0x9F,             // IFD offset far out
            0xFF, 0xFF,                         // 65535 entries
        };
        CHECK(exif::parse_jpeg_exif_orientation(hostile.data(), hostile.size()) == 1);
        // Same body, sane IFD offset but 0xFFFF entries: bound check rejects.
        std::vector<uint8_t> hostile2 = {
            0xFF, 0xD8,
            0xFF, 0xE1, 0x00, 0x16,
            'E', 'x', 'i', 'f', 0, 0,
            'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
            0xFF, 0xFF,
        };
        CHECK(exif::parse_jpeg_exif_orientation(hostile2.data(), hostile2.size()) == 1);
    }

    // Bad TIFF magic / byte-order mark and out-of-range orientation values
    // degrade to 1 rather than trusting the entry.
    {
        auto with_tiff_patch = [&](std::vector<uint8_t> f, size_t at,
                                   std::initializer_list<uint8_t> bytes) {
            for (size_t i = 0; i < bytes.size(); ++i) {
                f[at + i] = *(bytes.begin() + i);
            }
            return f;
        };
        const size_t tiff = 2 + 4 + 6;  // SOI + FF E1 len16 + "Exif\0\0"
        // Entry starts at tiff + 8 + 2; value field at +8 within the entry.
        const size_t value_at = tiff + 8 + 2 + 8;
        CHECK(exif::parse_jpeg_exif_orientation(
                  with_tiff_patch(tagged6, tiff + 2, {0x2B, 0x00}).data(),
                  tagged6.size()) == 1);                         // magic != 42
        CHECK(exif::parse_jpeg_exif_orientation(
                  with_tiff_patch(tagged6, tiff, {'X', 'X'}).data(),
                  tagged6.size()) == 1);                         // byte order
        CHECK(exif::parse_jpeg_exif_orientation(
                  with_tiff_patch(tagged6, value_at, {0x00, 0x00}).data(),
                  tagged6.size()) == 1);                         // value 0
        CHECK(exif::parse_jpeg_exif_orientation(
                  with_tiff_patch(tagged6, value_at, {0x09, 0x00}).data(),
                  tagged6.size()) == 1);                         // value 9
        CHECK(exif::parse_jpeg_exif_orientation(
                  with_tiff_patch(tagged6, value_at, {0x07, 0x00}).data(),
                  tagged6.size()) == 7);                         // still honest
    }
    std::fprintf(stderr, "OK hostile EXIF bodies\n");

    std::printf("ALL EXIF TESTS PASSED\n");
    return 0;
}
