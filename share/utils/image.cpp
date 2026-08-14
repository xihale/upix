#include "image.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

extern "C" {
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <libdeflate.h>
}

namespace crossscale {

namespace {
    auto normalize_image_format(std::string format) -> std::string {
        std::string normalized;
        normalized.reserve(format.size());
        for (char ch : format) {
            if (ch == '.' || std::isspace(static_cast<unsigned char>(ch))) {
                continue;
            }
            normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return normalized;
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

    auto write_file(const std::filesystem::path& path, const uint8_t* data, size_t size) -> expected<void, ImageError> {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(ImageError{3, "Cannot open file for writing: " + path.string()});
        }
        if (!file.write(reinterpret_cast<const char*>(data), size)) {
            return std::unexpected(ImageError{4, "Failed to write file: " + path.string()});
        }
        return {};
    }

    auto png_compression_level_from_quality(int quality) -> int {
        const int clamped = std::clamp(quality, 1, 100);
        int level = ((100 - clamped) * 9 + 50) / 100;
        return std::clamp(level, 0, 9);
    }

    // libdeflate level for the PNG fast path: same numeric scale as the old
    // zlib mapping. Measured on 4500x4500 RGB super-resolution output, the
    // full PNG encode drops 639ms -> 245ms (2.6x) at default quality with
    // files ~8% smaller; higher libdeflate levels shrink further but fall
    // behind zlib-1 speed by level 6, so the identity mapping keeps the
    // quality knob honest. libdeflate has no "stored"/0 level; its minimum
    // is 1.
    auto png_libdeflate_level_from_quality(int quality) -> int {
        const int zlib_level = png_compression_level_from_quality(quality);
        return zlib_level == 0 ? 1 : std::clamp(zlib_level, 1, 9);
    }

    auto append_png_u32(std::vector<uint8_t>& out, uint32_t value) -> void {
        out.push_back(static_cast<uint8_t>(value >> 24));
        out.push_back(static_cast<uint8_t>(value >> 16));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value));
    }

    auto write_png_chunk(std::vector<uint8_t>& out, std::string_view type,
                         const uint8_t* data, size_t length) -> void {
        append_png_u32(out, static_cast<uint32_t>(length));
        const size_t crc_start = out.size();
        out.insert(out.end(), type.begin(), type.end());
        if (length > 0) {
            out.insert(out.end(), data, data + length);
        }
        append_png_u32(out, libdeflate_crc32(0, out.data() + crc_start, 4 + length));
    }

    // Hand-rolled PNG writer for the images this codebase produces: 8-bit,
    // 1-4 channels, non-interlaced (load strips 16-bit and expands palettes,
    // so every in-memory Image qualifies). Fixed AVG row filter (matches the
    // previous libpng configuration) + libdeflate zlib stream + libdeflate
    // CRC32. Returns false so the caller can fall back to the libpng writer
    // when libdeflate cannot allocate or the stream does not compress.
    auto save_png_libdeflate(const Image& image, std::vector<uint8_t>& out, int level) -> bool {
        const int width = image.width();
        const int height = image.height();
        const int channels = image.channels();
        if (width <= 0 || height <= 0 || channels < 1 || channels > 4) {
            return false;
        }

        static constexpr uint8_t color_type_by_channels[5] = {0, 0, 4, 2, 6};

        const size_t stride = static_cast<size_t>(width) * static_cast<size_t>(channels);

        // AVG-filter every row: dst = raw - floor((left + up) / 2), modulo 256.
        // Filter choice affects only encoded size, never decoded pixels.
        std::vector<uint8_t> filtered((stride + 1) * static_cast<size_t>(height));
        const uint8_t* src = image.data();
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = src + static_cast<size_t>(y) * stride;
            const uint8_t* prev = y > 0 ? row - stride : nullptr;
            uint8_t* dst = filtered.data() + static_cast<size_t>(y) * (stride + 1);
            dst[0] = 3;  // PNG filter type Average (0=none 1=sub 2=up 3=avg 4=paeth)
            for (size_t x = 0; x < stride; ++x) {
                const int left = x >= static_cast<size_t>(channels) ? row[x - channels] : 0;
                const int up = prev != nullptr ? prev[x] : 0;
                dst[1 + x] = static_cast<uint8_t>(row[x] - ((left + up) >> 1));
            }
        }

        struct libdeflate_compressor* compressor = libdeflate_alloc_compressor(level);
        if (compressor == nullptr) {
            return false;
        }
        std::vector<uint8_t> compressed(libdeflate_zlib_compress_bound(compressor, filtered.size()));
        const size_t compressed_size = libdeflate_zlib_compress(
            compressor, filtered.data(), filtered.size(), compressed.data(), compressed.size());
        libdeflate_free_compressor(compressor);
        if (compressed_size == 0) {
            return false;
        }

        out.clear();
        out.reserve(compressed_size + 128);
        static constexpr uint8_t signature[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
        out.insert(out.end(), signature, signature + 8);

        uint8_t ihdr[13];
        const uint32_t be_width = static_cast<uint32_t>(width);
        const uint32_t be_height = static_cast<uint32_t>(height);
        ihdr[0] = static_cast<uint8_t>(be_width >> 24);
        ihdr[1] = static_cast<uint8_t>(be_width >> 16);
        ihdr[2] = static_cast<uint8_t>(be_width >> 8);
        ihdr[3] = static_cast<uint8_t>(be_width);
        ihdr[4] = static_cast<uint8_t>(be_height >> 24);
        ihdr[5] = static_cast<uint8_t>(be_height >> 16);
        ihdr[6] = static_cast<uint8_t>(be_height >> 8);
        ihdr[7] = static_cast<uint8_t>(be_height);
        ihdr[8] = 8;  // bit depth
        ihdr[9] = color_type_by_channels[channels];
        ihdr[10] = 0;  // compression: deflate
        ihdr[11] = 0;  // filter: adaptive (per-row bytes carry the actual type)
        ihdr[12] = 0;  // interlace: none
        write_png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
        write_png_chunk(out, "IDAT", compressed.data(), compressed_size);
        write_png_chunk(out, "IEND", nullptr, 0);
        return true;
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
        for (int y = 0; y < height; ++y) {
            row_pointers[y] = img.data() + y * width * channels;
        }
        
        png_read_image(png, row_pointers.data());
        png_read_end(png, nullptr);
        png_destroy_read_struct(&png, &info, nullptr);
        
        return img;
    }

    auto load_jpeg(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
        jpeg_decompress_struct cinfo;
        jpeg_error_mgr jerr;
        
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        
        jpeg_mem_src(&cinfo, const_cast<unsigned char*>(buffer.data()), buffer.size());
        
        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&cinfo);
            return std::unexpected(ImageError{6, "Invalid JPEG header"});
        }
        
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        
        int width = cinfo.output_width;
        int height = cinfo.output_height;
        int channels = cinfo.output_components;
        
        Image img(width, height, channels);
        
        while (cinfo.output_scanline < cinfo.output_height) {
            // Capture the row index BEFORE the call: jpeg_read_scanlines
            // increments output_scanline, so using it afterward writes every
            // row one position too far (buffer overflow past the image).
            const int row_index = cinfo.output_scanline;
            JSAMPLE* row = new JSAMPLE[width * channels];
            JSAMPARRAY buffer = &row;
            jpeg_read_scanlines(&cinfo, buffer, 1);
            std::memcpy(img.data() + row_index * width * channels, row, width * channels);
            delete[] row;
        }
        
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        
        return img;
    }

    auto load_webp(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
        int width, height;
        uint8_t* rgb = WebPDecodeRGB(buffer.data(), static_cast<int>(buffer.size()), &width, &height);
        if (!rgb) {
            return std::unexpected(ImageError{7, "WebP decode failed"});
        }
        
        Image img(width, height, 3);
        std::memcpy(img.data(), rgb, width * height * 3);
        WebPFree(rgb);
        return img;
    }

}

Image::Image(int width, int height, int channels)
    : width_(width), height_(height), channels_(channels) {
    data_.resize(static_cast<size_t>(width) * height * channels);
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
    if (!std::filesystem::exists(path)) {
        return std::unexpected(ImageError{1, "File not found: " + path.string()});
    }
    
    auto file_data = read_file(path);
    if (!file_data.has_value()) {
        return std::unexpected(file_data.error());
    }
    
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    try {
        if (ext == ".png") {
            return load_png(file_data.value());
        } else if (ext == ".jpg" || ext == ".jpeg") {
            return load_jpeg(file_data.value());
        } else if (ext == ".webp") {
            return load_webp(file_data.value());
        } else {
            return std::unexpected(ImageError{8, "Unsupported image format: " + ext});
        }
    } catch (const std::exception& e) {
        return std::unexpected(ImageError{9, "Exception during load: " + std::string(e.what())});
    }
}

auto Image::save(const std::filesystem::path& path, int quality) const
    -> expected<void, ImageError> {
    auto encoded = save_to_memory(path.extension().string(), quality);
    if (!encoded.has_value()) {
        return std::unexpected(encoded.error());
    }
    return write_file(path, encoded->data(), encoded->size());
}

auto Image::save_to_memory(const std::string& format, int quality) const
    -> expected<std::vector<uint8_t>, ImageError> {
    const std::string normalized_format = normalize_image_format(format);
    const std::string output_format = normalized_format.empty() ? "png" : normalized_format;
    // volatile: this value must survive libpng/libjpeg setjmp/longjmp error unwinding.
    const volatile int clamped_quality = std::clamp(quality, 1, 100);

    try {
        if (output_format == "png") {
            // Fast path: libdeflate-backed writer for the 8-bit 1-4 channel
            // images this codebase produces (2.6x faster than libpng+zlib,
            // ~8% smaller files). Falls through to the libpng writer below
            // for anything it cannot handle.
            std::vector<uint8_t> encoded;
            if (save_png_libdeflate(
                    *this, encoded, png_libdeflate_level_from_quality(clamped_quality))) {
                return encoded;
            }
            const size_t estimated_size = static_cast<size_t>(width_) * height_ * channels_ / 2 + 4096;
            encoded.reserve(estimated_size);

            png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (!png) return std::unexpected(ImageError{20, "Failed to create PNG write struct"});

            png_infop info = png_create_info_struct(png);
            if (!info) {
                png_destroy_write_struct(&png, nullptr);
                return std::unexpected(ImageError{20, "Failed to create PNG info struct"});
            }

            if (setjmp(png_jmpbuf(png))) {
                png_destroy_write_struct(&png, &info);
                return std::unexpected(ImageError{20, "Error during PNG memory writing"});
            }

            png_set_write_fn(
                png,
                &encoded,
                [](png_structp png_ptr, png_bytep data, png_size_t length) {
                    auto* out = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png_ptr));
                    const size_t size = out->size();
                    if (size + length > out->capacity()) {
                        out->reserve(std::max(size + length, size * 2));
                    }
                    out->insert(out->end(), data, data + length);
                },
                nullptr
            );

            png_byte color_type;
            if (channels_ == 1) color_type = PNG_COLOR_TYPE_GRAY;
            else if (channels_ == 2) color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
            else if (channels_ == 3) color_type = PNG_COLOR_TYPE_RGB;
            else if (channels_ == 4) color_type = PNG_COLOR_TYPE_RGBA;
            else {
                png_destroy_write_struct(&png, &info);
                return std::unexpected(ImageError{14, "Unsupported channel count for PNG: " + std::to_string(channels_)});
            }

            png_set_compression_level(png, png_compression_level_from_quality(clamped_quality));
            png_set_compression_strategy(png, PNG_Z_DEFAULT_STRATEGY);
            // Fixed row filter: libpng's default adaptive heuristic evaluates all
            // five filters per row, which measured ~32% of PNG encode time, while
            // a fixed AVG filter stays size-neutral (within +/-1% of adaptive) on
            // super-resolution output. Filtering choice never affects decoded pixels.
            png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_AVG);
            png_set_IHDR(
                png, info, width_, height_, 8, color_type, PNG_INTERLACE_NONE,
                PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
            );

            png_write_info(png, info);
            std::vector<png_bytep> row_pointers(height_);
            for (int y = 0; y < height_; ++y) {
                row_pointers[y] = const_cast<png_bytep>(&data_[static_cast<size_t>(y) * width_ * channels_]);
            }
            png_write_image(png, row_pointers.data());
            png_write_end(png, nullptr);
            png_destroy_write_struct(&png, &info);
            return encoded;
        }

        if (output_format == "jpg" || output_format == "jpeg") {
            if (channels_ != 1 && channels_ != 3) {
                return std::unexpected(ImageError{15, "JPEG only supports 1 or 3 channels"});
            }

            jpeg_compress_struct cinfo;
            jpeg_error_mgr jerr;
            unsigned char* out_buffer = nullptr;
            size_t out_size = 0;

            cinfo.err = jpeg_std_error(&jerr);
            jpeg_create_compress(&cinfo);
            jpeg_mem_dest(&cinfo, &out_buffer, &out_size);

            cinfo.image_width = width_;
            cinfo.image_height = height_;
            cinfo.input_components = channels_;
            cinfo.in_color_space = channels_ == 1 ? JCS_GRAYSCALE : JCS_RGB;

            jpeg_set_defaults(&cinfo);
            jpeg_set_quality(&cinfo, clamped_quality, TRUE);
            jpeg_start_compress(&cinfo, TRUE);

            std::vector<JSAMPROW> row_pointers(height_);
            const size_t row_stride = static_cast<size_t>(width_) * channels_;
            for (int y = 0; y < height_; ++y) {
                row_pointers[y] = const_cast<JSAMPROW>(&data_[static_cast<size_t>(y) * row_stride]);
            }
            while (cinfo.next_scanline < cinfo.image_height) {
                JDIMENSION num_lines = cinfo.image_height - cinfo.next_scanline;
                jpeg_write_scanlines(&cinfo, &row_pointers[cinfo.next_scanline], num_lines);
            }

            jpeg_finish_compress(&cinfo);
            jpeg_destroy_compress(&cinfo);

            if (!out_buffer || out_size == 0) {
                std::free(out_buffer);
                return std::unexpected(ImageError{21, "JPEG memory encode failed"});
            }

            std::vector<uint8_t> encoded(out_buffer, out_buffer + out_size);
            std::free(out_buffer);
            return encoded;
        }

        if (output_format == "webp") {
            if (channels_ != 3 && channels_ != 4) {
                return std::unexpected(ImageError{18, "WebP save requires 3 or 4 channels"});
            }

            uint8_t* out_buffer = nullptr;
            const int stride = width_ * channels_;
            size_t out_size = 0;

            if (channels_ == 3) {
                out_size = WebPEncodeRGB(data(), width_, height_, stride, static_cast<float>(clamped_quality), &out_buffer);
            } else {
                out_size = WebPEncodeRGBA(data(), width_, height_, stride, static_cast<float>(clamped_quality), &out_buffer);
            }

            if (out_size == 0 || out_buffer == nullptr) {
                return std::unexpected(ImageError{19, "WebP encode failed"});
            }

            std::vector<uint8_t> encoded(out_buffer, out_buffer + out_size);
            WebPFree(out_buffer);
            return encoded;
        }

        return std::unexpected(ImageError{22, "Unsupported save format: " + output_format});
    } catch (const std::exception& e) {
        return std::unexpected(ImageError{23, "Exception during memory save: " + std::string(e.what())});
    }
}

} // namespace crossscale
