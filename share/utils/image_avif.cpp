#include "image_avif.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace crossscale::avif_codec {
namespace {

using avifResult = int;

constexpr avifResult kAvifOk = 0;
constexpr avifResult kAvifNoCodecAvailable = 15;
constexpr int kAvifPixelFormatYuv420 = 3;
constexpr int kAvifRgbFormatRgb = 0;
constexpr int kAvifRgbFormatRgba = 1;
constexpr int kAvifSpeedStill = 6;

struct AvifImage;
struct AvifDecoder;
struct AvifEncoder;

struct AvifRWData {
    uint8_t* data;
    size_t size;
};

// libavif 1.x public layout (SONAME .so.16). Trailing tail absorbs fields
// added after 1.0 so a newer SetDefaults cannot write past this struct.
struct AvifRGBImage {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    int format;
    int chroma_upsampling;
    int chroma_downsampling;
    int avoid_libyuv;
    int ignore_alpha;
    int alpha_premultiplied;
    int is_float;
    int max_threads;
    uint8_t* pixels;
    uint32_t row_bytes;
    uint8_t tail[128];
};

// Frozen 1.0 prefix of avifEncoder. We never allocate this; avifEncoderCreate
// returns the library-sized object and we only write quality/speed through it.
struct AvifEncoderOverlay {
    int codec_choice;
    int max_threads;
    int speed;
    int keyframe_interval;
    uint64_t timescale;
    int repetition_count;
    uint32_t extra_layer_count;
    int quality;
    int quality_alpha;
};

static_assert(offsetof(AvifEncoderOverlay, timescale) == 16);
static_assert(offsetof(AvifEncoderOverlay, quality) == 32);

struct AvifImageHead {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

using FnVersion = const char* (*)();
using FnResultToString = const char* (*)(avifResult);
using FnDecoderCreate = AvifDecoder* (*)();
using FnDecoderDestroy = void (*)(AvifDecoder*);
using FnDecoderReadMemory = avifResult (*)(AvifDecoder*, AvifImage*, const uint8_t*, size_t);
using FnImageCreate = AvifImage* (*)(uint32_t, uint32_t, uint32_t, int);
using FnImageCreateEmpty = AvifImage* (*)();
using FnImageDestroy = void (*)(AvifImage*);
using FnRgbSetDefaults = void (*)(AvifRGBImage*, const AvifImage*);
using FnRgbAllocatePixels = avifResult (*)(AvifRGBImage*);
using FnRgbFreePixels = void (*)(AvifRGBImage*);
using FnYuvToRgb = avifResult (*)(const AvifImage*, AvifRGBImage*);
using FnRgbToYuv = avifResult (*)(AvifImage*, const AvifRGBImage*);
using FnEncoderCreate = AvifEncoder* (*)();
using FnEncoderDestroy = void (*)(AvifEncoder*);
using FnEncoderWrite = avifResult (*)(AvifEncoder*, const AvifImage*, AvifRWData*);
using FnRwDataFree = void (*)(AvifRWData*);

struct AvifApi {
    void* lib = nullptr;
    FnVersion version = nullptr;
    FnResultToString result_to_string = nullptr;
    FnDecoderCreate decoder_create = nullptr;
    FnDecoderDestroy decoder_destroy = nullptr;
    FnDecoderReadMemory decoder_read_memory = nullptr;
    FnImageCreate image_create = nullptr;
    FnImageCreateEmpty image_create_empty = nullptr;
    FnImageDestroy image_destroy = nullptr;
    FnRgbSetDefaults rgb_set_defaults = nullptr;
    FnRgbAllocatePixels rgb_allocate_pixels = nullptr;
    FnRgbFreePixels rgb_free_pixels = nullptr;
    FnYuvToRgb yuv_to_rgb = nullptr;
    FnRgbToYuv rgb_to_yuv = nullptr;
    FnEncoderCreate encoder_create = nullptr;
    FnEncoderDestroy encoder_destroy = nullptr;
    FnEncoderWrite encoder_write = nullptr;
    FnRwDataFree rw_data_free = nullptr;

    auto decode_ready() const -> bool {
        return lib && decoder_create && decoder_destroy && decoder_read_memory &&
               image_create_empty && image_destroy && rgb_set_defaults &&
               rgb_allocate_pixels && rgb_free_pixels && yuv_to_rgb;
    }

    auto encode_ready() const -> bool {
        return decode_ready() && image_create && rgb_to_yuv && encoder_create &&
               encoder_destroy && encoder_write && rw_data_free;
    }
};

#if defined(_WIN32)
using LibHandle = HMODULE;

auto open_lib(const char* name) -> LibHandle {
    return LoadLibraryA(name);
}

auto close_lib(LibHandle handle) -> void {
    if (handle) {
        FreeLibrary(handle);
    }
}

auto load_sym(LibHandle handle, const char* name) -> void* {
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}
#else
using LibHandle = void*;

auto open_lib(const char* name) -> LibHandle {
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

auto close_lib(LibHandle handle) -> void {
    if (handle) {
        dlclose(handle);
    }
}

auto load_sym(LibHandle handle, const char* name) -> void* {
    return dlsym(handle, name);
}
#endif

auto try_open_avif() -> LibHandle {
#if defined(_WIN32)
    const char* names[] = {"libavif-16.dll", "avif-16.dll", "libavif.dll", "avif.dll"};
#elif defined(__APPLE__)
    const char* names[] = {
        "libavif.16.dylib",
        "libavif.dylib",
        "/opt/homebrew/lib/libavif.16.dylib",
        "/opt/homebrew/lib/libavif.dylib",
        "/usr/local/lib/libavif.16.dylib",
        "/usr/local/lib/libavif.dylib",
    };
#else
    const char* names[] = {"libavif.so.16", "libavif.so"};
#endif
    for (const char* name : names) {
        if (LibHandle handle = open_lib(name)) {
            return handle;
        }
    }
    return nullptr;
}

auto result_text(const AvifApi& api, avifResult result) -> std::string {
    if (api.result_to_string) {
        if (const char* text = api.result_to_string(result)) {
            if (text[0] != '\0') {
                return text;
            }
        }
    }
    return std::to_string(result);
}

auto missing_library_message() -> std::string {
    return "AVIF needs libavif. Install: pacman -S libavif | apt install libavif16 | brew install libavif";
}

auto missing_encoder_message() -> std::string {
    return "libavif cannot encode AVIF (no aom/svt-av1). Reinstall libavif with an encoder";
}

// Shared by load_api() (logged, for real decode/encode work) and the quiet
// completion probes below.
auto bind_symbols(LibHandle handle) -> AvifApi {
    AvifApi api;
    api.lib = handle;
    auto bind = [&](const char* name) { return load_sym(handle, name); };
    api.version = reinterpret_cast<FnVersion>(bind("avifVersion"));
    api.result_to_string = reinterpret_cast<FnResultToString>(bind("avifResultToString"));
    api.decoder_create = reinterpret_cast<FnDecoderCreate>(bind("avifDecoderCreate"));
    api.decoder_destroy = reinterpret_cast<FnDecoderDestroy>(bind("avifDecoderDestroy"));
    api.decoder_read_memory =
        reinterpret_cast<FnDecoderReadMemory>(bind("avifDecoderReadMemory"));
    api.image_create = reinterpret_cast<FnImageCreate>(bind("avifImageCreate"));
    api.image_create_empty = reinterpret_cast<FnImageCreateEmpty>(bind("avifImageCreateEmpty"));
    api.image_destroy = reinterpret_cast<FnImageDestroy>(bind("avifImageDestroy"));
    api.rgb_set_defaults = reinterpret_cast<FnRgbSetDefaults>(bind("avifRGBImageSetDefaults"));
    api.rgb_allocate_pixels =
        reinterpret_cast<FnRgbAllocatePixels>(bind("avifRGBImageAllocatePixels"));
    api.rgb_free_pixels = reinterpret_cast<FnRgbFreePixels>(bind("avifRGBImageFreePixels"));
    api.yuv_to_rgb = reinterpret_cast<FnYuvToRgb>(bind("avifImageYUVToRGB"));
    api.rgb_to_yuv = reinterpret_cast<FnRgbToYuv>(bind("avifImageRGBToYUV"));
    api.encoder_create = reinterpret_cast<FnEncoderCreate>(bind("avifEncoderCreate"));
    api.encoder_destroy = reinterpret_cast<FnEncoderDestroy>(bind("avifEncoderDestroy"));
    api.encoder_write = reinterpret_cast<FnEncoderWrite>(bind("avifEncoderWrite"));
    api.rw_data_free = reinterpret_cast<FnRwDataFree>(bind("avifRWDataFree"));
    return api;
}

// libavif 1.x is the only accepted ABI; anything else is treated as absent.
auto version_is_1x(const AvifApi& api) -> bool {
    const char* version = api.version ? api.version() : "";
    return version != nullptr && version[0] == '1' && version[1] == '.';
}

auto load_api() -> const AvifApi& {
    static std::once_flag once;
    static AvifApi api;
    std::call_once(once, [] {
        LibHandle handle = try_open_avif();
        if (!handle) {
            return;
        }
        api = bind_symbols(handle);

        if (!api.decode_ready()) {
            LOG_WARNING("Image", "libavif loaded but required decode symbols are missing");
            close_lib(handle);
            api = AvifApi{};
            return;
        }
        if (!version_is_1x(api)) {
            LOG_WARNING(
                "Image",
                std::string("libavif version is not 1.x (got ") +
                    (api.version ? api.version() : "unknown") + "), refusing to use it"
            );
            close_lib(handle);
            api = AvifApi{};
            return;
        }
        LOG_INFO("Image", std::string("Using system libavif ") + api.version());
    });
    return api;
}

auto be32(const uint8_t* p) -> uint32_t {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

auto box_type_eq(const uint8_t* type, const char* want) -> bool {
    return std::memcmp(type, want, 4) == 0;
}

auto find_brand(const uint8_t* data, size_t size, const char* brand) -> bool {
    if (size < 12 || !box_type_eq(data + 4, "ftyp")) {
        return false;
    }
    const uint32_t box_size = be32(data);
    if (box_size < 16) {
        return false;
    }
    const size_t limit = std::min(size, static_cast<size_t>(box_size));
    for (size_t offset = 8; offset + 4 <= limit; offset += 4) {
        if (box_type_eq(data + offset, brand)) {
            return true;
        }
    }
    return false;
}

struct IspeWalk {
    uint32_t width = 0;
    uint32_t height = 0;
};

auto walk_boxes(const uint8_t* data, size_t begin, size_t end, IspeWalk& out, int depth) -> void {
    if (depth > 12 || begin >= end) {
        return;
    }
    size_t offset = begin;
    while (offset + 8 <= end) {
        const uint32_t size32 = be32(data + offset);
        const uint8_t* type = data + offset + 4;
        size_t header = 8;
        uint64_t box_size = size32;
        if (size32 == 1) {
            if (offset + 16 > end) {
                return;
            }
            box_size = (static_cast<uint64_t>(be32(data + offset + 8)) << 32) |
                       be32(data + offset + 12);
            header = 16;
        } else if (size32 == 0) {
            box_size = end - offset;
        }
        if (box_size < header || offset + box_size > end) {
            return;
        }
        const size_t payload_begin = offset + header;
        const size_t payload_end = offset + static_cast<size_t>(box_size);
        if (box_type_eq(type, "ispe") && payload_end - payload_begin >= 12) {
            out.width = be32(data + payload_begin + 4);
            out.height = be32(data + payload_begin + 8);
            return;
        }
        if (box_type_eq(type, "meta") && payload_end >= payload_begin + 4) {
            walk_boxes(data, payload_begin + 4, payload_end, out, depth + 1);
        } else if (box_type_eq(type, "iprp") || box_type_eq(type, "ipco")) {
            walk_boxes(data, payload_begin, payload_end, out, depth + 1);
        }
        if (out.width > 0 && out.height > 0) {
            return;
        }
        offset = payload_end;
    }
}

// Thread count shared by the encoder (parallel tiles) and RGB conversions.
auto codec_threads() -> int {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? static_cast<int>(hw) : 4;
}

}  // namespace

auto looks_like_avif(const uint8_t* data, size_t size) -> bool {
    if (data == nullptr || size < 16) {
        return false;
    }
    return find_brand(data, size, "avif") || find_brand(data, size, "avis");
}

auto sniff_dimensions(const uint8_t* data, size_t size) -> std::pair<uint32_t, uint32_t> {
    if (data == nullptr || size < 16 || !looks_like_avif(data, size)) {
        return {0, 0};
    }
    IspeWalk found;
    walk_boxes(data, 0, size, found, 0);
    return {found.width, found.height};
}

auto load(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
    const AvifApi& api = load_api();
    if (!api.decode_ready()) {
        return std::unexpected(ImageError{24, missing_library_message()});
    }

    AvifDecoder* decoder = api.decoder_create();
    AvifImage* image = api.image_create_empty();
    if (!decoder || !image) {
        if (image) {
            api.image_destroy(image);
        }
        if (decoder) {
            api.decoder_destroy(decoder);
        }
        return std::unexpected(ImageError{25, "AVIF decoder allocation failed"});
    }

    const avifResult read_result =
        api.decoder_read_memory(decoder, image, buffer.data(), buffer.size());
    api.decoder_destroy(decoder);
    decoder = nullptr;
    if (read_result != kAvifOk) {
        api.image_destroy(image);
        return std::unexpected(
            ImageError{25, "AVIF decode failed: " + result_text(api, read_result)}
        );
    }

    const auto* head = reinterpret_cast<const AvifImageHead*>(image);
    if (head->width == 0 || head->height == 0) {
        api.image_destroy(image);
        return std::unexpected(ImageError{25, "AVIF decode produced empty image"});
    }

    AvifRGBImage rgb{};
    api.rgb_set_defaults(&rgb, image);
    rgb.format = kAvifRgbFormatRgb;
    rgb.depth = 8;
    rgb.max_threads = codec_threads();  // parallel YUV→RGB when libyuv steps aside
    const avifResult alloc_result = api.rgb_allocate_pixels(&rgb);
    if (alloc_result != kAvifOk || rgb.pixels == nullptr || rgb.width == 0 || rgb.height == 0) {
        api.image_destroy(image);
        return std::unexpected(ImageError{25, "AVIF RGB allocation failed"});
    }

    const avifResult convert_result = api.yuv_to_rgb(image, &rgb);
    if (convert_result != kAvifOk) {
        api.rgb_free_pixels(&rgb);
        api.image_destroy(image);
        return std::unexpected(
            ImageError{25, "AVIF YUV to RGB failed: " + result_text(api, convert_result)}
        );
    }

    Image out(static_cast<int>(rgb.width), static_cast<int>(rgb.height), 3);
    const size_t row_bytes = static_cast<size_t>(rgb.width) * 3;
    const size_t src_stride = rgb.row_bytes != 0 ? rgb.row_bytes : row_bytes;
    for (uint32_t y = 0; y < rgb.height; ++y) {
        std::memcpy(
            out.data() + static_cast<size_t>(y) * row_bytes,
            rgb.pixels + static_cast<size_t>(y) * src_stride,
            row_bytes
        );
    }

    api.rgb_free_pixels(&rgb);
    api.image_destroy(image);
    return out;
}

auto save(const Image& image, int quality) -> expected<std::vector<uint8_t>, ImageError> {
    if (image.channels() != 3 && image.channels() != 4) {
        return std::unexpected(ImageError{18, "AVIF save requires 3 or 4 channels"});
    }
    if (image.width() <= 0 || image.height() <= 0) {
        return std::unexpected(ImageError{26, "AVIF encode failed: empty image"});
    }

    const AvifApi& api = load_api();
    if (!api.decode_ready()) {
        return std::unexpected(ImageError{24, missing_library_message()});
    }
    if (!api.encode_ready()) {
        return std::unexpected(ImageError{27, missing_encoder_message()});
    }

    AvifImage* avif_image = api.image_create(
        static_cast<uint32_t>(image.width()),
        static_cast<uint32_t>(image.height()),
        8,
        kAvifPixelFormatYuv420
    );
    if (!avif_image) {
        return std::unexpected(ImageError{26, "AVIF image allocation failed"});
    }

    AvifRGBImage rgb{};
    api.rgb_set_defaults(&rgb, avif_image);
    rgb.format = image.channels() == 4 ? kAvifRgbFormatRgba : kAvifRgbFormatRgb;
    rgb.depth = 8;
    rgb.max_threads = codec_threads();
    rgb.pixels = const_cast<uint8_t*>(image.data());
    rgb.row_bytes = static_cast<uint32_t>(image.width() * image.channels());

    const avifResult convert_result = api.rgb_to_yuv(avif_image, &rgb);
    if (convert_result != kAvifOk) {
        api.image_destroy(avif_image);
        return std::unexpected(
            ImageError{26, "AVIF RGB to YUV failed: " + result_text(api, convert_result)}
        );
    }

    AvifEncoder* encoder = api.encoder_create();
    if (!encoder) {
        api.image_destroy(avif_image);
        return std::unexpected(ImageError{26, "AVIF encoder allocation failed"});
    }
    auto* overlay = reinterpret_cast<AvifEncoderOverlay*>(encoder);
    overlay->quality = quality;
    overlay->quality_alpha = quality;
    overlay->speed = kAvifSpeedStill;
    // Without threads the encoder is single-threaded: a 4500² still takes ~13 s
    // where `avifenc -j N` (parallel tiles) takes ~2.4 s on the same machine.
    overlay->max_threads = codec_threads();

    AvifRWData output{nullptr, 0};
    const avifResult write_result = api.encoder_write(encoder, avif_image, &output);
    api.encoder_destroy(encoder);
    api.image_destroy(avif_image);

    if (write_result != kAvifOk || output.data == nullptr || output.size == 0) {
        if (output.data) {
            api.rw_data_free(&output);
        }
        if (write_result == kAvifNoCodecAvailable) {
            return std::unexpected(ImageError{27, missing_encoder_message()});
        }
        return std::unexpected(
            ImageError{26, "AVIF encode failed: " + result_text(api, write_result)}
        );
    }

    std::vector<uint8_t> encoded(output.data, output.data + output.size);
    api.rw_data_free(&output);
    return encoded;
}

namespace {

// Same acceptance rules as load_api(), minus the logging: completion queries
// run on every TAB press and their stdout must stay pure candidate lines.
// The handle deliberately stays open for the process lifetime, like load_api().
auto probe_api_quietly() -> const AvifApi& {
    static const AvifApi api = [] {
        LibHandle handle = try_open_avif();
        if (handle == nullptr) {
            return AvifApi{};
        }
        AvifApi bound = bind_symbols(handle);
        if (!bound.decode_ready() || !version_is_1x(bound)) {
            close_lib(handle);
            return AvifApi{};
        }
        return bound;
    }();
    return api;
}

}  // namespace

auto avif_decode_available() -> bool { return probe_api_quietly().decode_ready(); }
auto avif_encode_available() -> bool { return probe_api_quietly().encode_ready(); }

}  // namespace crossscale::avif_codec
