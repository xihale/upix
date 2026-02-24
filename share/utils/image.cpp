#include "image.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include <webp/encode.h>
}

namespace crossscale {

namespace {
    auto read_file(const std::filesystem::path& path) -> expected<std::vector<uint8_t>, ImageError> {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return ImageError{1, "Cannot open file: " + path.string()};
        }
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return ImageError{2, "Failed to read file: " + path.string()};
        }
        return buffer;
    }

    auto write_file(const std::filesystem::path& path, const uint8_t* data, size_t size) -> expected<void, ImageError> {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return ImageError{3, "Cannot open file for writing: " + path.string()};
        }
        if (!file.write(reinterpret_cast<const char*>(data), size)) {
            return ImageError{4, "Failed to write file: " + path.string()};
        }
        return {};
    }

    auto png_compression_level_from_quality(int quality) -> int {
        const int clamped = std::clamp(quality, 1, 100);
        int level = ((100 - clamped) * 9 + 50) / 100;
        return std::clamp(level, 0, 9);
    }

    auto load_png(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError> {
        struct PngReadContext {
            const uint8_t* data;
            size_t size;
            size_t offset;
        };
        
        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) return ImageError{5, "Failed to create PNG read struct"};
        
        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            return ImageError{5, "Failed to create PNG info struct"};
        }
        
        if (setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            return ImageError{5, "Error during PNG reading"};
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

        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_RGB) {
            png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
        }

        png_read_update_info(png, info);

        int channels = png_get_channels(png, info);
        if (channels <= 0) {
            png_destroy_read_struct(&png, &info, nullptr);
            return ImageError{5, "Invalid channel count from PNG"};
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
            return ImageError{6, "Invalid JPEG header"};
        }
        
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        
        int width = cinfo.output_width;
        int height = cinfo.output_height;
        int channels = cinfo.output_components;
        
        Image img(width, height, channels);
        
        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPLE* row = new JSAMPLE[width * channels];
            JSAMPARRAY buffer = &row;
            jpeg_read_scanlines(&cinfo, buffer, 1);
            std::memcpy(img.data() + cinfo.output_scanline * width * channels, row, width * channels);
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
            return ImageError{7, "WebP decode failed"};
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
        return ImageError{1, "File not found: " + path.string()};
    }
    
    auto file_data = read_file(path);
    if (!file_data.has_value()) {
        return file_data.error();
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
            return ImageError{8, "Unsupported image format: " + ext};
        }
    } catch (const std::exception& e) {
        return ImageError{9, "Exception during load: " + std::string(e.what())};
    }
}

auto Image::load_from_memory(const uint8_t* data, size_t size)
    -> expected<Image, ImageError> {
    return ImageError{3, "Not implemented"};
}

auto Image::save(const std::filesystem::path& path, int quality) const
    -> expected<void, ImageError> {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    try {
        if (ext == ".png") {
            return save_png(path, quality);
        } else if (ext == ".jpg" || ext == ".jpeg") {
            return save_jpeg(path, quality);
        } else if (ext == ".webp") {
            return save_webp(path, quality);
        } else {
            return ImageError{10, "Unsupported save format: " + ext};
        }
    } catch (const std::exception& e) {
        return ImageError{11, "Exception during save: " + std::string(e.what())};
    }
}

auto Image::save_png(const std::filesystem::path& path, int quality) const
    -> expected<void, ImageError> {
    std::vector<uint8_t> buffer;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return ImageError{12, "Failed to create PNG write struct"};
    
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return ImageError{12, "Failed to create PNG info struct"};
    }
    
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return ImageError{12, "Error during PNG writing"};
    }
    
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) {
        png_destroy_write_struct(&png, &info);
        return ImageError{13, "Cannot open file for writing: " + path.string()};
    }
    
    png_init_io(png, fp);
    png_set_compression_level(png, png_compression_level_from_quality(quality));
    png_set_compression_strategy(png, PNG_Z_DEFAULT_STRATEGY);
    
    png_byte color_type;
    if (channels_ == 1) color_type = PNG_COLOR_TYPE_GRAY;
    else if (channels_ == 2) color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
    else if (channels_ == 3) color_type = PNG_COLOR_TYPE_RGB;
    else if (channels_ == 4) color_type = PNG_COLOR_TYPE_RGBA;
    else {
        fclose(fp);
        png_destroy_write_struct(&png, &info);
        return ImageError{14, "Unsupported channel count for PNG: " + std::to_string(channels_)};
    }
    
    png_set_IHDR(png, info, width_, height_, 8, color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    
    png_write_info(png, info);
    
    for (int y = 0; y < height_; ++y) {
        png_bytep row = const_cast<png_bytep>(&data_[static_cast<size_t>(y) * width_ * channels_]);
        png_write_row(png, row);
    }
    
    png_write_end(png, nullptr);
    fclose(fp);
    png_destroy_write_struct(&png, &info);
    return {};
}

auto Image::save_jpeg(const std::filesystem::path& path, int quality) const
    -> expected<void, ImageError> {
    if (channels_ != 1 && channels_ != 3) {
        return ImageError{15, "JPEG only supports 1 or 3 channels"};
    }
    
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) {
        return ImageError{16, "Cannot open file for writing: " + path.string()};
    }
    
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    
    cinfo.image_width = width_;
    cinfo.image_height = height_;
    cinfo.input_components = channels_;
    cinfo.in_color_space = channels_ == 1 ? JCS_GRAYSCALE : JCS_RGB;
    
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    
    JSAMPROW row_pointer[1];
    const size_t row_stride = width_ * channels_;
    
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = const_cast<JSAMPLE*>(&data_[static_cast<size_t>(cinfo.next_scanline) * row_stride]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return {};
}

auto Image::save_webp(const std::filesystem::path& path, int quality) const
    -> expected<void, ImageError> {
    if (channels_ != 3 && channels_ != 4) {
        return ImageError{18, "WebP save requires 3 or 4 channels"};
    }
    
    uint8_t* output = nullptr;
    size_t size = 0;
    int stride = width_ * channels_;
    
    if (channels_ == 3) {
        size = WebPEncodeRGB(data(), width_, height_, stride, static_cast<float>(quality), &output);
    } else {
        size = WebPEncodeRGBA(data(), width_, height_, stride, static_cast<float>(quality), &output);
    }
    
    if (size == 0 || output == nullptr) {
        return ImageError{19, "WebP encode failed"};
    }
    
    auto write_result = write_file(path, output, size);
    WebPFree(output);
    
    if (!write_result.has_value()) {
        return write_result.error();
    }
    
    return {};
}

auto Image::extract_region(int x, int y, int w, int h) const
    -> expected<Image, ImageError> {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > width_ || y + h > height_) {
        return ImageError{5, "Region out of bounds"};
    }
    
    Image region(w, h, channels_);
    for (int row = 0; row < h; ++row) {
        std::copy(data_.begin() + ((y + row) * width_ + x) * channels_,
                  data_.begin() + ((y + row) * width_ + x + w) * channels_,
                  region.data_.begin() + row * w * channels_);
    }
    
    return region;
}

uint8_t& Image::at(int x, int y, int c) {
    return data_[(y * width_ + x) * channels_ + c];
}

uint8_t Image::at(int x, int y, int c) const {
    return data_[(y * width_ + x) * channels_ + c];
}

} // namespace crossscale
