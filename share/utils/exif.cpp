// EXIF orientation: JPEG APP1 parse + pixel bake-in. See exif.hpp.
#include "exif.hpp"
#include "image.hpp"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace upix::exif {
namespace {

auto be16(const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

auto le16(const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8) | p[0]);
}

// Orientation out of a TIFF header + IFD0, everything bounds-checked against
// [tiff, tiff + tiff_len). Any malformation returns 1 (no-op semantics).
auto orientation_from_tiff(const uint8_t* tiff, size_t tiff_len) -> int {
    if (tiff_len < 8) {
        return 1;
    }
    bool little = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        little = true;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
        little = false;
    } else {
        return 1;
    }
    const auto rd16 = [&](const uint8_t* p) {
        return little ? le16(p) : be16(p);
    };
    const auto rd32 = [&](const uint8_t* p) -> uint32_t {
        if (little) {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };
    if (rd16(tiff + 2) != 42) {
        return 1;  // TIFF magic
    }
    const uint32_t ifd_off = rd32(tiff + 4);
    if (static_cast<size_t>(ifd_off) + 2 > tiff_len) {
        return 1;
    }
    const uint16_t entries = rd16(tiff + ifd_off);
    // 12 bytes per entry, accumulated in size_t so a hostile entry count
    // can neither overflow nor walk past the segment.
    if (static_cast<size_t>(ifd_off) + 2 + 12ull * entries > tiff_len) {
        return 1;
    }
    for (uint16_t i = 0; i < entries; ++i) {
        const uint8_t* e = tiff + ifd_off + 2 + 12ull * i;
        if (rd16(e) != 0x0112) continue;  // Orientation
        if (rd16(e + 2) != 3) continue;   // type must be SHORT
        if (rd32(e + 4) < 1) continue;    // count
        // count == 1: the value sits in the first two bytes of the value
        // field (left-justified, endian-following).
        const uint16_t value = rd16(e + 8);
        return value >= 1 && value <= 8 ? value : 1;
    }
    return 1;
}

// Mirror left-right (orientation 2), in place.
auto mirror_horizontal(uint8_t* p, int w, int h, int c) -> void {
    const size_t stride = static_cast<size_t>(w) * c;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = p + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w / 2; ++x) {
            uint8_t* a = row + static_cast<size_t>(x) * c;
            uint8_t* b = row + static_cast<size_t>(w - 1 - x) * c;
            for (int k = 0; k < c; ++k) {
                std::swap(a[k], b[k]);
            }
        }
    }
}

// Mirror top-bottom (orientation 4), in place. Row pairs travel through one
// temporary row as three memcpys — no per-pixel work (matters on 50 MP
// phone shots).
auto mirror_vertical(uint8_t* p, int w, int h, int c) -> void {
    const size_t stride = static_cast<size_t>(w) * c;
    std::vector<uint8_t> tmp(stride);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = p + static_cast<size_t>(y) * stride;
        uint8_t* b = p + static_cast<size_t>(h - 1 - y) * stride;
        std::memcpy(tmp.data(), a, stride);
        std::memcpy(a, b, stride);
        std::memcpy(b, tmp.data(), stride);
    }
}

// 180° (orientation 3), in place: reverse the whole pixel sequence.
auto rotate_180(uint8_t* p, int w, int h, int c) -> void {
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n / 2; ++i) {
        uint8_t* a = p + i * c;
        uint8_t* b = p + (n - 1 - i) * c;
        for (int k = 0; k < c; ++k) {
            std::swap(a[k], b[k]);
        }
    }
}

// Transposing orientations (5-8): dst receives the swapped-geometry frame
// (width' = h, height' = w); src and dst must not overlap.
//
//   out(x, y) = in(y, x)               orientation 5 (transpose)
//   out(x, y) = in(y, h-1-x)           orientation 6 (rotate 90° CW)
//   out(x, y) = in(w-1-y, h-1-x)       orientation 7 (transverse)
//   out(x, y) = in(w-1-y, x)           orientation 8 (rotate 90° CCW)
auto transpose_transform(const uint8_t* src, uint8_t* dst, int w, int h,
                         int c, int orientation) -> void {
    const int ow = h;
    const int oh = w;
    const size_t ostride = static_cast<size_t>(ow) * c;
    for (int y = 0; y < oh; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * ostride;
        for (int x = 0; x < ow; ++x) {
            int sx = 0;
            int sy = 0;
            switch (orientation) {
                case 5:  sx = y;             sy = x;             break;
                case 6:  sx = y;             sy = h - 1 - x;     break;
                case 7:  sx = w - 1 - y;     sy = h - 1 - x;     break;
                default: sx = w - 1 - y;     sy = x;             break;  // 8
            }
            const uint8_t* s = src + (static_cast<size_t>(sy) * w + sx) * c;
            uint8_t* d = row + static_cast<size_t>(x) * c;
            for (int k = 0; k < c; ++k) {
                d[k] = s[k];
            }
        }
    }
}

}  // namespace

auto parse_jpeg_exif_orientation(const uint8_t* data, size_t len) -> int {
    if (data == nullptr || len < 4) {
        return 1;
    }
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return 1;  // not a JPEG (SOI)
    }
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (data[pos] != 0xFF) {
            return 1;  // desynced from the marker stream
        }
        size_t m = pos + 1;
        while (m < len && data[m] == 0xFF) {
            ++m;  // fill bytes before the marker code
        }
        if (m >= len) {
            return 1;
        }
        const uint8_t marker = data[m];
        // Standalone markers (TEM, RSTn) carry no length; SOI mid-stream is
        // malformed — all bail or skip without payload.
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            pos = m + 1;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            return 1;  // SOS/EOI: EXIF must precede the entropy-coded data
        }
        if (len - m < 3) {
            return 1;  // truncated length field
        }
        const size_t seg_len = be16(data + m + 1);  // includes its own 2 bytes
        if (seg_len < 2) {
            return 1;
        }
        const size_t payload = seg_len - 2;
        // Layout: FF(at pos) marker(at m) len_hi len_lo payload... — the
        // payload starts three bytes past the marker code.
        const size_t seg_start = m + 3;
        if (payload > len - seg_start) {
            return 1;  // truncated segment
        }
        if (marker == 0xE1 && payload >= 6 &&
            std::memcmp(data + seg_start, "Exif\0\0", 6) == 0) {
            return orientation_from_tiff(data + seg_start + 6, payload - 6);
        }
        pos = seg_start + payload;
    }
    return 1;
}

auto apply_exif_orientation_u8(std::vector<uint8_t>& pixels, int& w, int& h,
                               int channels, int orientation) -> bool {
    if (orientation < 2 || orientation > 8) {
        return false;
    }
    if (w <= 0 || h <= 0 || channels <= 0) {
        return false;
    }
    const size_t needed =
        static_cast<size_t>(w) * static_cast<size_t>(h) * channels;
    if (pixels.size() < needed) {
        return false;
    }
    uint8_t* p = pixels.data();
    switch (orientation) {
        case 2:
            mirror_horizontal(p, w, h, channels);
            return true;
        case 3:
            rotate_180(p, w, h, channels);
            return true;
        case 4:
            mirror_vertical(p, w, h, channels);
            return true;
        default:
            break;
    }
    // 5-8 swap the geometry; transform through a scratch frame and copy
    // back so any tail slack beyond w*h*c (e.g. Image's +16 upload slack)
    // survives untouched.
    auto dst = std::make_unique_for_overwrite<uint8_t[]>(needed);
    transpose_transform(p, dst.get(), w, h, channels, orientation);
    std::memcpy(p, dst.get(), needed);
    std::swap(w, h);
    return true;
}

auto autorotate_image(Image& image, int orientation) -> bool {
    if (orientation < 2 || orientation > 8) {
        return false;
    }
    const int w = image.width();
    const int h = image.height();
    const int c = image.channels();
    if (w <= 0 || h <= 0 || c <= 0) {
        return false;
    }
    if (orientation <= 4) {
        switch (orientation) {
            case 2: mirror_horizontal(image.data(), w, h, c); break;
            case 3: rotate_180(image.data(), w, h, c); break;
            default: mirror_vertical(image.data(), w, h, c); break;
        }
        return true;
    }
    // 5-8 transpose: build a fresh frame (Image's constructor adds the +16
    // upload slack the Vulkan path reads) and move it in — one pixel copy.
    Image rotated(h, w, c);
    transpose_transform(image.data(), rotated.data(), w, h, c, orientation);
    image = std::move(rotated);
    return true;
}

}  // namespace upix::exif
