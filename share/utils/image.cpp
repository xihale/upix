#include "image.hpp"
#include "image_avif.hpp"
#include "logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <print>
#include <ranges>
#include <span>
#include <thread>
#include <fstream>
#include <string_view>

extern "C" {
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <libdeflate.h>
}

namespace upix {

namespace {
    // Wrap the encoded WebP into the extended (VP8X) container and attach
    // the XMP payload as its "XMP " chunk. libwebpmux post-pass — the
    // Picture/encode API cannot carry metadata. Returns false (encoded
    // untouched) on mux errors; callers degrade to an unmarked file.
    auto webp_attach_xmp(std::vector<uint8_t>& encoded, const std::string& xmp) -> bool {
        WebPMux* mux = WebPMuxNew();
        if (mux == nullptr) {
            return false;
        }
        const WebPData image{encoded.data(), encoded.size()};
        const WebPData chunk{
            reinterpret_cast<const uint8_t*>(xmp.data()), xmp.size()};
        bool ok = WebPMuxSetImage(mux, &image, 1) == WEBP_MUX_OK &&
                  WebPMuxSetChunk(mux, "XMP ", &chunk, 1) == WEBP_MUX_OK;
        WebPData assembled{nullptr, 0};
        if (ok && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK) {
            encoded.assign(assembled.bytes, assembled.bytes + assembled.size);
        } else {
            ok = false;
        }
        WebPDataClear(&assembled);
        WebPMuxDelete(mux);
        return ok;
    }

    // WebPPicture writer that appends into a std::vector (WebPEncode calls it
    // once per encoded chunk; returning 0 aborts the encode).
    auto webp_vector_writer(const uint8_t* data, size_t size,
                            const WebPPicture* picture) -> int {
        auto* out = static_cast<std::vector<uint8_t>*>(picture->custom_ptr);
        out->insert(out->end(), data, data + size);
        return 1;
    }

    auto normalize_image_format(std::string_view format) -> std::string {
        auto normalized = format
            | std::views::filter([](unsigned char ch) {
                  return ch != '.' && !std::isspace(ch);
              })
            | std::views::transform([](unsigned char ch) {
                  return static_cast<char>(std::tolower(ch));
              });
        return {normalized.begin(), normalized.end()};
    }

    auto read_file(const std::filesystem::path& path) -> expected<std::vector<uint8_t>, ImageError> {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected(ImageError{1, "Cannot open file: " + path.string()});
        }
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return std::unexpected(ImageError{2, "Failed to read file: " + path.string()});
        }
        return buffer;
    }

    auto write_file(const std::filesystem::path& path, std::span<const uint8_t> data) -> expected<void, ImageError> {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(ImageError{3, "Cannot open file for writing: " + path.string()});
        }
        if (!file.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size()))) {
            return std::unexpected(ImageError{4, "Failed to write file: " + path.string()});
        }
        return {};
    }

    auto png_compression_level_from_quality(int quality) -> int {
        const int clamped = std::clamp(quality, 1, 100);
        int level = ((100 - clamped) * 9 + 50) / 100;
        return std::clamp(level, 0, 9);
    }

    // PNG color type for the 1-4 channel 8-bit images this codebase produces
    // (indexed by channel count; entry 0 unused).
    constexpr uint8_t kPngColorTypeByChannels[5] = {0, 0, 4, 2, 6};

    // The libdeflate fast path below materializes the AVG-filtered frame plus
    // a worst-case compress_bound output — ~2x raw bytes on top of the image
    // itself (measured: a 14000x14400 output peaks ~1.1GB extra RSS). Past
    // this raw-size threshold, stream through libpng instead: identical
    // pixels, same fixed AVG filter, O(rows) extra memory, at the cost of the
    // ~2.6x slower encode (639ms vs 245ms at 4500^2) — negligible next to
    // multi-second inference at these dimensions.
    constexpr size_t kPngStreamingThresholdBytes = 128u << 20;

    // libdeflate level for the PNG fast path: same numeric scale as the old
    // zlib mapping. Measured on 4500x4500 RGB super-resolution output, the
    // full PNG encode drops 639ms -> 245ms (2.6x) at default quality with
    // files ~8% smaller; higher libdeflate levels shrink further but fall
    // behind zlib-1 speed by level 6, so the identity mapping keeps the
    // quality knob honest. libdeflate has no "stored"/0 level; its minimum
    // is 1. An explicit EncoderOptions.png_level (0-9) overrides the
    // quality-derived mapping.
    auto png_zlib_level(int quality, const EncoderOptions& opts) -> int {
        if (opts.png_level >= 0) {
            return std::clamp(opts.png_level, 0, 9);
        }
        return png_compression_level_from_quality(quality);
    }

    auto png_libdeflate_level(int quality, const EncoderOptions& opts) -> int {
        const int zlib_level = png_zlib_level(quality, opts);
        return zlib_level == 0 ? 1 : std::clamp(zlib_level, 1, 9);
    }

    auto append_png_u32(std::vector<uint8_t>& out, uint32_t value) -> void {
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

    // Big-endian u32 into a fixed byte buffer (PNG IHDR fields).
    constexpr auto put_be_u32(std::array<uint8_t, 13>& out, size_t offset,
                              uint32_t value) -> void {
        for (const int i : std::views::iota(0, 4)) {
            out[offset + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
        }
    }

    auto write_png_chunk(std::vector<uint8_t>& out, std::string_view type,
                         std::span<const uint8_t> data) -> void {
        append_png_u32(out, static_cast<uint32_t>(data.size()));
        const size_t crc_start = out.size();
        out.insert(out.end(), type.begin(), type.end());
        out.insert(out.end(), data.begin(), data.end());
        append_png_u32(out, libdeflate_crc32(0, out.data() + crc_start,
                                             4 + data.size()));
    }

    // iTXt payload carrying an XMP packet: the standard
    // "XML:com.adobe.xmp" keyword, uncompressed, empty language tag and
    // translated keyword, then the UTF-8 XML. Shared shape with the
    // streaming-libpng path so both writers emit identical metadata.
    auto build_png_xmp_itxt(std::string_view xmp) -> std::vector<uint8_t> {
        static constexpr std::string_view kXmpKeyword = "XML:com.adobe.xmp";
        std::vector<uint8_t> payload;
        payload.reserve(kXmpKeyword.size() + 4 + xmp.size());
        payload.insert(payload.end(), kXmpKeyword.begin(), kXmpKeyword.end());
        payload.push_back(0);  // keyword terminator
        payload.push_back(0);  // compression flag: uncompressed
        payload.push_back(0);  // compression method: none
        payload.push_back(0);  // language tag: empty
        payload.push_back(0);  // translated keyword: empty
        payload.insert(payload.end(), xmp.begin(), xmp.end());
        return payload;
    }

    // Hand-rolled PNG writer for the images this codebase produces: 8-bit,
    // 1-4 channels, non-interlaced (load strips 16-bit and expands palettes,
    // so every in-memory Image qualifies). Fixed AVG row filter (matches the
    // previous libpng configuration) + libdeflate zlib stream + libdeflate
    // CRC32. Returns false only on invalid dimensions or compressor
    // allocation/compress failure (OOM-class).
    auto save_png_libdeflate(const Image& image, std::vector<uint8_t>& out, int level,
                             const std::string& xmp) -> bool {
        const int width = image.width();
        const int height = image.height();
        const int channels = image.channels();
        if (width <= 0 || height <= 0 || channels < 1 || channels > 4) {
            return false;
        }

        const size_t stride = static_cast<size_t>(width) * static_cast<size_t>(channels);

        // AVG-filter every row: dst = raw - floor((left + up) / 2), modulo 256.
        // Filter choice affects only encoded size, never decoded pixels.
        // Rows are independent (each reads two *raw* source rows), so large
        // images filter in parallel; the buffers are written in full below,
        // so they are allocated uninitialized to skip a ~600 MB memset each.
        const auto filter_start = std::chrono::steady_clock::now();
        const auto filtered_bytes = (stride + 1) * static_cast<size_t>(height);
        auto filtered = std::make_unique_for_overwrite<uint8_t[]>(filtered_bytes);
        const uint8_t* src = image.data();
        auto filter_rows = [&](int y_begin, int y_end) {
            for (int y = y_begin; y < y_end; ++y) {
                const uint8_t* row = src + static_cast<size_t>(y) * stride;
                const uint8_t* prev = y > 0 ? row - stride : nullptr;
                uint8_t* dst = filtered.get() + static_cast<size_t>(y) * (stride + 1);
                dst[0] = 3;  // PNG filter type Average (0=none 1=sub 2=up 3=avg 4=paeth)
                for (size_t x = 0; x < stride; ++x) {
                    const int left = x >= static_cast<size_t>(channels) ? row[x - channels] : 0;
                    const int up = prev != nullptr ? prev[x] : 0;
                    dst[1 + x] = static_cast<uint8_t>(row[x] - ((left + up) >> 1));
                }
            }
        };
        {
            const auto hw = std::thread::hardware_concurrency();
            const size_t parallel_threshold = 16u << 20;  // filter cost ~bandwidth-bound
            if (hw > 1 && filtered_bytes >= parallel_threshold) {
                const int threads = static_cast<int>(std::min<size_t>(hw, 8u));
                std::vector<std::thread> pool;
                pool.reserve(threads - 1);
                for (int t = 1; t < threads; ++t) {
                    const int y_begin = static_cast<int>(1LL * height * t / threads);
                    const int y_end = static_cast<int>(1LL * height * (t + 1) / threads);
                    pool.emplace_back(filter_rows, y_begin, y_end);
                }
                filter_rows(0, static_cast<int>(1LL * height / threads));
                for (auto& th : pool) {
                    th.join();
                }
            } else {
                filter_rows(0, height);
            }
        }

        struct libdeflate_compressor* compressor = libdeflate_alloc_compressor(level);
        if (compressor == nullptr) {
            return false;
        }
        const auto filter_done = std::chrono::steady_clock::now();
        const auto bound = libdeflate_zlib_compress_bound(compressor, filtered_bytes);
        auto compressed = std::make_unique_for_overwrite<uint8_t[]>(bound);
        const size_t compressed_size = libdeflate_zlib_compress(
            compressor, filtered.get(), filtered_bytes, compressed.get(), bound);
        libdeflate_free_compressor(compressor);
        const auto compress_done = std::chrono::steady_clock::now();
        if (std::getenv("UPIX_PIPELINE_TIMER") != nullptr) {
            std::print(stderr, "[PERF] png save {}x{}: filter={}ms compress={}ms\n",
                       width, height,
                       static_cast<size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           filter_done - filter_start).count()),
                       static_cast<size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           compress_done - filter_done).count()));
        }
        if (compressed_size == 0) {
            return false;
        }

        out.clear();
        out.reserve(compressed_size + 128);
        static constexpr uint8_t signature[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
        out.insert(out.end(), signature, signature + 8);

        std::array<uint8_t, 13> ihdr{};
        put_be_u32(ihdr, 0, static_cast<uint32_t>(width));
        put_be_u32(ihdr, 4, static_cast<uint32_t>(height));
        ihdr[8] = 8;  // bit depth
        ihdr[9] = kPngColorTypeByChannels[channels];
        ihdr[10] = 0;  // compression: deflate
        ihdr[11] = 0;  // filter: adaptive (per-row bytes carry the actual type)
        ihdr[12] = 0;  // interlace: none
        write_png_chunk(out, "IHDR", ihdr);
        if (!xmp.empty()) {
            const auto itxt = build_png_xmp_itxt(xmp);
            write_png_chunk(out, "iTXt", itxt);
        }
        write_png_chunk(out, "IDAT", {compressed.get(), compressed_size});
        write_png_chunk(out, "IEND", {});
        return true;
    }

    // Streaming PNG writer for very large images (see
    // kPngStreamingThresholdBytes): libpng encodes row-by-row straight to the
    // output file, so the only extra memory is the row-pointer table. Same
    // fixed AVG filter as the libdeflate path; decoded pixels are identical,
    // only the compressed byte stream differs.
    auto png_write_to_stream(png_structp png, png_bytep data, png_size_t length) -> void {
        auto* file = static_cast<std::ofstream*>(png_get_io_ptr(png));
        file->write(reinterpret_cast<const char*>(data), length);
        if (!*file) {
            png_error(png, "PNG write failed");
        }
    }

    auto png_flush_stream(png_structp png) -> void {
        static_cast<std::ofstream*>(png_get_io_ptr(png))->flush();
    }

    auto save_png_streaming(const Image& image, const std::filesystem::path& path, int level,
                            const std::string& xmp) -> expected<void, ImageError> {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(ImageError{3, "Cannot open file for writing: " + path.string()});
        }

        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) {
            return std::unexpected(ImageError{5, "Failed to create PNG write struct"});
        }
        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_write_struct(&png, nullptr);
            return std::unexpected(ImageError{5, "Failed to create PNG info struct"});
        }

        // volatile: this local must survive the libpng setjmp/longjmp unwinding.
        volatile bool encode_failed = false;
        if (setjmp(png_jmpbuf(png))) {
            encode_failed = true;
        }

        if (!encode_failed) {
            png_set_write_fn(png, &file, png_write_to_stream, png_flush_stream);
            // Match the fast path's effective level floor (libdeflate has no 0).
            png_set_compression_level(png, level < 1 ? 1 : level);
            png_set_filter(png, 0, PNG_FILTER_AVG);

            const int channels = image.channels();
            png_set_IHDR(png, info,
                         static_cast<png_uint_32>(image.width()),
                         static_cast<png_uint_32>(image.height()),
                         8, kPngColorTypeByChannels[channels],
                         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            if (!xmp.empty()) {
                png_text text{};
                text.compression = PNG_ITXT_COMPRESSION_NONE;
                text.key = const_cast<png_charp>("XML:com.adobe.xmp");
                // png_text::text is non-const; the packet outlives the call.
                text.text = const_cast<png_charp>(xmp.c_str());
                text.itxt_length = xmp.size();
                png_set_text(png, info, &text, 1);
            }
            png_write_info(png, info);

            std::vector<png_bytep> rows(static_cast<size_t>(image.height()));
            const size_t stride = static_cast<size_t>(image.width()) * channels;
            std::ranges::transform(std::views::iota(0, image.height()), rows.begin(),
                [&](int y) {
                    return const_cast<png_bytep>(image.data() +
                                                 static_cast<size_t>(y) * stride);
                });
            png_write_image(png, rows.data());
            png_write_end(png, info);
            png_destroy_write_struct(&png, &info);
        } else {
            png_destroy_write_struct(&png, &info);
        }

        file.close();
        if (encode_failed || !file) {
            return std::unexpected(ImageError{4, "Failed to write file: " + path.string()});
        }
        return {};
    }

    auto load_png(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
        struct PngReadContext {
            const uint8_t* data;
            size_t size;
            size_t offset;
        };
        
        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) return std::unexpected(ImageError{5, "Failed to create PNG read struct"});
        
        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            return std::unexpected(ImageError{5, "Failed to create PNG info struct"});
        }
        
        if (setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            return std::unexpected(ImageError{5, "Error during PNG reading"});
        }
        
        PngReadContext ctx{buffer.data(), buffer.size(), 0};
        png_set_read_fn(png, &ctx,
                        [](png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
                            PngReadContext* ctx = static_cast<PngReadContext*>(png_get_io_ptr(png_ptr));
                            if (ctx->offset + byteCountToRead > ctx->size) {
                                png_error(png_ptr, "Read beyond buffer");
                            }
                            std::memcpy(outBytes, ctx->data + ctx->offset, byteCountToRead);
                            ctx->offset += byteCountToRead;
                        });
        
        png_read_info(png, info);
        int width = png_get_image_width(png, info);
        int height = png_get_image_height(png, info);
        png_byte color_type = png_get_color_type(png, info);
        png_byte bit_depth = png_get_bit_depth(png, info);

        if (bit_depth == 16) {
            png_set_strip_16(png);
        } else if (bit_depth < 8) {
            png_set_packing(png);
        }

        if (color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_palette_to_rgb(png);
        }

        if (png_get_valid(png, info, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png);
        }

        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png);
        }

        png_read_update_info(png, info);

        int channels = png_get_channels(png, info);
        if (channels <= 0) {
            png_destroy_read_struct(&png, &info, nullptr);
            return std::unexpected(ImageError{5, "Invalid channel count from PNG"});
        }

        Image img(width, height, channels);
        std::vector<png_bytep> row_pointers(height);
        const size_t stride = static_cast<size_t>(width) * channels;
        std::ranges::transform(std::views::iota(0, height), row_pointers.begin(),
            [&](int y) { return img.data() + static_cast<size_t>(y) * stride; });
        
        png_read_image(png, row_pointers.data());
        png_read_end(png, nullptr);
        png_destroy_read_struct(&png, &info, nullptr);
        
        return img;
    }

    // libjpeg's default error_exit calls exit() — on ANY thread that is fatal
    // to the whole process (a corrupt remote submit would take the entire
    // coordinator down mid-batch). Route hard errors through a C++ exception
    // instead; unwinding stops at the try in load_jpeg below, whose guard
    // releases the decompress state. libjpeg frames are plain C (nothing to
    // clean up), so table-based unwinding crosses them safely.
    struct JpegErrorHook {
        jpeg_error_mgr pub{};
        std::string message;
    };
    struct DecompressGuard {
        jpeg_decompress_struct* p;
        ~DecompressGuard() { jpeg_destroy_decompress(p); }
    };
    struct CompressGuard {
        jpeg_compress_struct* p;
        ~CompressGuard() { jpeg_destroy_compress(p); }
    };

    auto load_jpeg(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
        jpeg_decompress_struct cinfo{};
        JpegErrorHook hook{};

        cinfo.err = &hook.pub;
        jpeg_std_error(&hook.pub);
        hook.pub.error_exit = [](j_common_ptr cinfo) {
            auto* self = reinterpret_cast<JpegErrorHook*>(cinfo->err);
            char buf[JMSG_LENGTH_MAX] = {};
            (*cinfo->err->format_message)(cinfo, buf);
            self->message = buf;
            throw std::runtime_error(buf);
        };
        jpeg_create_decompress(&cinfo);
        DecompressGuard guard{&cinfo};

        try {
            jpeg_mem_src(&cinfo, const_cast<unsigned char*>(buffer.data()),
                         buffer.size());

            if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
                return std::unexpected(ImageError{6, "Invalid JPEG header"});
            }

            cinfo.out_color_space = JCS_RGB;
            jpeg_start_decompress(&cinfo);

            int width = cinfo.output_width;
            int height = cinfo.output_height;
            int channels = cinfo.output_components;

            Image img(width, height, channels);

            while (cinfo.output_scanline < cinfo.output_height) {
                // jpeg_read_scanlines increments output_scanline, so capture the
                // row index before the call and decode straight into the image
                // buffer — no per-row temporary.
                const int row_index = cinfo.output_scanline;
                JSAMPROW row = img.data() +
                               static_cast<size_t>(row_index) * width * channels;
                JSAMPARRAY scanlines = &row;
                jpeg_read_scanlines(&cinfo, scanlines, 1);
            }

            jpeg_finish_decompress(&cinfo);

            return img;
        } catch (const std::exception& e) {
            const std::string detail =
                !hook.message.empty() ? hook.message : e.what();
            return std::unexpected(
                ImageError{6, "JPEG decode failed: " + detail});
        }
    }

    auto load_webp(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
        int width = 0, height = 0;
        if (!WebPGetInfo(buffer.data(), buffer.size(), &width, &height) || !width || !height) {
            return std::unexpected(ImageError{7, "Invalid WebP header"});
        }

        // Decoder config instead of the one-shot WebPDecodeRGB: enables
        // multithreaded decode and decodes straight into the Image buffer
        // (the simple API forces an extra full-size copy).
        WebPDecoderConfig cfg;
        if (!WebPInitDecoderConfig(&cfg)) {
            return std::unexpected(ImageError{7, "WebP decoder init failed"});
        }
        cfg.options.use_threads = static_cast<int>(std::thread::hardware_concurrency());

        Image img(width, height, 3);
        cfg.output.colorspace = MODE_RGB;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = img.data();
        cfg.output.u.RGBA.stride = width * 3;
        cfg.output.u.RGBA.size = static_cast<size_t>(width) * height * 3;

        if (WebPDecode(buffer.data(), buffer.size(), &cfg) != VP8_STATUS_OK) {
            return std::unexpected(ImageError{7, "WebP decode failed"});
        }
        return img;
    }

}

Image::Image(int width, int height, int channels)
    : width_(width), height_(height), channels_(channels) {
    // +16 slack: the Vulkan upload path (ncnn record_clone on a 1-D u8 Mat)
    // copies rounded-up-to-16 bytes, which reads past a exactly-sized buffer.
    // size() stays w*h*c; the tail is never part of the image.
    data_.resize(static_cast<size_t>(width) * height * channels + 16);
}

Image::Image(Image&& other) noexcept
    : data_(std::move(other.data_))
    , width_(other.width_)
    , height_(other.height_)
    , channels_(other.channels_) {
    other.width_ = 0;
    other.height_ = 0;
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        data_ = std::move(other.data_);
        width_ = other.width_;
        height_ = other.height_;
        channels_ = other.channels_;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

auto Image::load(const std::filesystem::path& path)
    -> expected<Image, ImageError> {
    auto file_data = load_raw(path);
    if (!file_data.has_value()) {
        return std::unexpected(file_data.error());
    }

    std::string ext = path.extension().string();
    return load_from_memory(ext, file_data.value());
}

auto Image::load_raw(const std::filesystem::path& path)
    -> expected<std::vector<uint8_t>, ImageError> {
    if (!std::filesystem::exists(path)) {
        return std::unexpected(ImageError{1, "File not found: " + path.string()});
    }
    return read_file(path);
}

auto Image::load_from_memory(const std::string& format,
                             const std::vector<uint8_t>& data)
    -> expected<Image, ImageError> {
    try {
        const std::string fmt = normalize_image_format(format);
        if (fmt == "png") {
            return load_png(data);
        }
        if (fmt == "jpg" || fmt == "jpeg") {
            return load_jpeg(data);
        }
        if (fmt == "webp") {
            return load_webp(data);
        }
        if (fmt == "avif" || avif_codec::looks_like_avif(
                                        data.data(), data.size())) {
            return avif_codec::load(data);
        }
        return std::unexpected(ImageError{8, "Unsupported image format: " + format});
    } catch (const std::exception& e) {
        return std::unexpected(ImageError{9, "Exception during load: " + std::string(e.what())});
    }
}

auto Image::save(const std::filesystem::path& path, int quality,
                 const std::string& xmp, const EncoderOptions& opts) const
    -> expected<void, ImageError> {
    // Very large PNG outputs go straight to disk row-by-row instead of
    // materializing the whole compressed file (plus the fast path's ~2x raw
    // transient) in RAM.
    const std::string fmt = normalize_image_format(path.extension().string());
    if ((fmt.empty() || fmt == "png") && size() >= kPngStreamingThresholdBytes) {
        return save_png_streaming(*this, path,
                                  png_libdeflate_level(quality, opts), xmp);
    }

    auto encoded = save_to_memory(path.extension().string(), quality, xmp, opts);
    if (!encoded.has_value()) {
        return std::unexpected(encoded.error());
    }
    return write_file(path, *encoded);
}

auto Image::save_to_memory(const std::string& format, int quality,
                           const std::string& xmp, const EncoderOptions& opts) const
    -> expected<std::vector<uint8_t>, ImageError> {
    const std::string normalized_format = normalize_image_format(format);
    const std::string output_format = normalized_format.empty() ? "png" : normalized_format;
    // volatile: this value must survive libpng/libjpeg setjmp/longjmp error unwinding.
    const volatile int clamped_quality = std::clamp(quality, 1, 100);

    try {
        if (output_format == "png") {
            // libdeflate-backed writer for the 8-bit 1-4 channel images this
            // codebase produces (2.6x faster than libpng+zlib, ~8% smaller
            // files). Failures are compressor-alloc/zero-size-compress cases
            // (OOM-class); return the error instead of falling back.
            std::vector<uint8_t> encoded;
            if (!save_png_libdeflate(
                    *this, encoded, png_libdeflate_level(clamped_quality, opts), xmp)) {
                return std::unexpected(ImageError{20, "PNG encode failed (libdeflate)"});
            }
            return encoded;
        }

        if (output_format == "jpg" || output_format == "jpeg") {
            if (channels_ != 1 && channels_ != 3) {
                return std::unexpected(ImageError{15, "JPEG only supports 1 or 3 channels"});
            }

            jpeg_compress_struct cinfo{};
            JpegErrorHook hook{};
            unsigned char* out_buffer = nullptr;
            size_t out_size = 0;

            // Same exit()-default landmine as the decode side: an encoder
            // error must come back as ImageError, not kill the process.
            cinfo.err = &hook.pub;
            jpeg_std_error(&hook.pub);
            hook.pub.error_exit = [](j_common_ptr cinfo) {
                auto* self = reinterpret_cast<JpegErrorHook*>(cinfo->err);
                char buf[JMSG_LENGTH_MAX] = {};
                (*cinfo->err->format_message)(cinfo, buf);
                self->message = buf;
                throw std::runtime_error(buf);
            };
            jpeg_create_compress(&cinfo);
            CompressGuard guard{&cinfo};
            jpeg_mem_dest(&cinfo, &out_buffer, &out_size);

            cinfo.image_width = width_;
            cinfo.image_height = height_;
            cinfo.input_components = channels_;
            cinfo.in_color_space = channels_ == 1 ? JCS_GRAYSCALE : JCS_RGB;

            jpeg_set_defaults(&cinfo);
            // Chroma subsampling via sampling factors (libjpeg default is
            // Y=2x2, Cb/Cr=1x1 = 4:2:0). 422/444 widen the chroma resolution
            // at the cost of file size; values outside {420,422,444} degrade
            // to the 4:2:0 default (validate() rejects them at config level).
            if (channels_ == 3) {
                int subsampling = opts.jpeg_subsampling;
                if (subsampling != 422 && subsampling != 444) {
                    subsampling = 420;
                }
                cinfo.comp_info[0].h_samp_factor = subsampling == 444 ? 1 : 2;
                cinfo.comp_info[0].v_samp_factor = subsampling == 420 ? 2 : 1;
                for (int c = 1; c < 3; ++c) {
                    cinfo.comp_info[c].h_samp_factor = 1;
                    cinfo.comp_info[c].v_samp_factor = 1;
                }
            }
            jpeg_set_quality(&cinfo, clamped_quality, TRUE);
            jpeg_start_compress(&cinfo, TRUE);

            if (!xmp.empty()) {
                // APP1 with the standard XMP namespace URI prefix.
                static constexpr std::string_view kJpegXmpNamespace =
                    "http://ns.adobe.com/xap/1.0/\0";
                std::vector<uint8_t> app1;
                app1.reserve(kJpegXmpNamespace.size() + xmp.size());
                app1.insert(app1.end(), kJpegXmpNamespace.begin(),
                            kJpegXmpNamespace.end());  // includes the NUL
                app1.insert(app1.end(), xmp.begin(), xmp.end());
                jpeg_write_marker(&cinfo, JPEG_APP0 + 1, app1.data(),
                                  static_cast<unsigned int>(app1.size()));
            }

            std::vector<JSAMPROW> row_pointers(height_);
            const size_t row_stride = static_cast<size_t>(width_) * channels_;
            for (const int y : std::views::iota(0, height_)) {
                row_pointers[y] = const_cast<JSAMPROW>(&data_[static_cast<size_t>(y) * row_stride]);
            }

            try {
                while (cinfo.next_scanline < cinfo.image_height) {
                    JDIMENSION num_lines = cinfo.image_height - cinfo.next_scanline;
                    jpeg_write_scanlines(&cinfo, &row_pointers[cinfo.next_scanline], num_lines);
                }

                jpeg_finish_compress(&cinfo);
            } catch (const std::exception&) {
                std::free(out_buffer);
                return std::unexpected(ImageError{
                    21, "JPEG encode failed: " + hook.message});
            }
            // Guard released the compressor; only the destination buffer
            // remains caller-owned.
            if (!out_buffer || out_size == 0) {
                std::free(out_buffer);
                return std::unexpected(ImageError{21, "JPEG memory encode failed"});
            }

            std::vector<uint8_t> encoded(out_buffer, out_buffer + out_size);
            std::free(out_buffer);
            return encoded;
        }

        if (output_format == "avif") {
            return avif_codec::save(*this, clamped_quality, xmp, opts);
        }

        if (output_format == "webp") {
            if (channels_ != 3 && channels_ != 4) {
                return std::unexpected(ImageError{18, "WebP save requires 3 or 4 channels"});
            }

            // Picture API instead of the one-shot WebPEncodeRGB*: the simple
            // API is single-threaded; use_threads parallelizes entropy coding
            // across tiles like `cwebp -threads`.
            WebPConfig cfg;
            if (!WebPConfigInit(&cfg)) {
                return std::unexpected(ImageError{19, "WebP encoder init failed"});
            }
            cfg.quality = static_cast<float>(clamped_quality);
            cfg.lossless = opts.webp_lossless ? 1 : 0;
            cfg.thread_level = 1;
            if (opts.webp_method >= 0) {
                // Trade encode time for file size (libwebp default is 4).
                cfg.method = std::clamp(opts.webp_method, 0, 6);
            }

            const int stride = width_ * channels_;
            WebPPicture pic;
            if (!WebPPictureInit(&pic)) {
                return std::unexpected(ImageError{19, "WebP picture init failed"});
            }
            pic.width = width_;
            pic.height = height_;
            pic.writer = webp_vector_writer;

            std::vector<uint8_t> encoded;
            bool ok = false;
            if (channels_ == 3) {
                ok = WebPPictureImportRGB(&pic, data(), stride) != 0;
            } else {
                ok = WebPPictureImportRGBA(&pic, data(), stride) != 0;
            }
            if (ok) {
                pic.custom_ptr = &encoded;
                ok = WebPEncode(&cfg, &pic) != 0 && !encoded.empty();
            }
            WebPPictureFree(&pic);

            if (!ok) {
                return std::unexpected(ImageError{19, "WebP encode failed"});
            }
            if (!xmp.empty() && !webp_attach_xmp(encoded, xmp)) {
                LOG_WARNING("Image", "WebP XMP mux failed; writing without marker");
            }
            return encoded;
        }

        return std::unexpected(ImageError{22, "Unsupported save format: " + output_format});
    } catch (const std::exception& e) {
        return std::unexpected(ImageError{23, "Exception during memory save: " + std::string(e.what())});
    }
}

// ── Channel planar conversion (shared: CLI inputs + coordinator tasks) ──────
auto convert_image_channels(const Image& source, int target_channels)
    -> expected<Image, ImageError> {
    const int src_channels = source.channels();
    if (src_channels < 1 || src_channels > 4 || target_channels < 1 ||
        target_channels > 4) {
        return std::unexpected(ImageError{30, "unsupported channel count"});
    }

    Image converted(source.width(), source.height(), target_channels);
    const size_t pixel_count =
        static_cast<size_t>(source.width()) * source.height();
    const uint8_t* src = source.data();
    uint8_t* dst = converted.data();

    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t* src_px = src + i * static_cast<size_t>(src_channels);
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;

        switch (src_channels) {
            case 1:
                r = g = b = src_px[0];
                break;
            case 2:
                r = g = b = src_px[0];
                a = src_px[1];
                break;
            case 3:
                r = src_px[0];
                g = src_px[1];
                b = src_px[2];
                break;
            case 4:
                r = src_px[0];
                g = src_px[1];
                b = src_px[2];
                a = src_px[3];
                break;
            default:
                return std::unexpected(ImageError{31, "bad channel count"});
        }

        const uint8_t gray = static_cast<uint8_t>(
            (77u * static_cast<unsigned int>(r) +
             150u * static_cast<unsigned int>(g) +
             29u * static_cast<unsigned int>(b) +
             128u) >> 8
        );

        uint8_t* dst_px = dst + i * static_cast<size_t>(target_channels);
        switch (target_channels) {
            case 1:
                dst_px[0] = gray;
                break;
            case 2:
                dst_px[0] = gray;
                dst_px[1] = a;
                break;
            case 3:
                // Dropping alpha copies RGB straight through (historical CLI
                // behavior kept byte-identical on re-runs).
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                break;
            case 4:
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                dst_px[3] = a;
                break;
        }
    }
    return converted;
}

auto normalize_image_channels(Image& image, int expected_channels,
                              std::string_view what) -> bool {
    if (expected_channels <= 0 || image.channels() == expected_channels) {
        return true;
    }
    auto converted = convert_image_channels(image, expected_channels);
    if (!converted.has_value()) {
        LOG_ERROR("Engine",
                  "Input channel count {} incompatible with model input "
                  "channels {}: {}",
                  image.channels(), expected_channels, what);
        return false;
    }
    LOG_INFO("Engine",
             "converted input channels {} -> {} for {}",
             image.channels(), expected_channels, what);
    image = std::move(converted.value());
    return image.channels() == expected_channels;
}

} // namespace upix
