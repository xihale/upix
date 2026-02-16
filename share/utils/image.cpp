#include "image.hpp"
#include "logger.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

namespace crossscale {

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

    int width, height, channels;
    uint8_t* data = stbi_load(path.string().c_str(), &width, &height, &channels, 0);
    
    if (!data) {
        return ImageError{2, "Failed to load image: " + path.string()};
    }

    Image img;
    img.width_ = width;
    img.height_ = height;
    img.channels_ = channels;
    img.data_.assign(data, data + static_cast<size_t>(width) * height * channels);
    stbi_image_free(data);
    
    return img;
}

auto Image::load_from_memory(const uint8_t* data, size_t size)
    -> expected<Image, ImageError> {
    int width, height, channels;
    uint8_t* img_data = stbi_load_from_memory(data, static_cast<int>(size), 
                                                &width, &height, &channels, 0);
    
    if (!img_data) {
        return ImageError{3, "Failed to decode image from memory"};
    }

    Image img;
    img.width_ = width;
    img.height_ = height;
    img.channels_ = channels;
    img.data_.assign(img_data, img_data + static_cast<size_t>(width) * height * channels);
    stbi_image_free(img_data);
    
    return img;
}

auto Image::save(const std::filesystem::path& path, int quality) const
    -> expected<void, ImageError> {
    if (!valid()) {
        return ImageError{4, "Invalid image"};
    }

    std::string ext = path.extension().string();
    for (auto& c : ext) c = std::tolower(c);

    int result = 0;
    if (ext == ".png") {
        result = stbi_write_png(path.string().c_str(), width_, height_, channels_, 
                                data_.data(), width_ * channels_);
    } else if (ext == ".jpg" || ext == ".jpeg") {
        result = stbi_write_jpg(path.string().c_str(), width_, height_, channels_,
                                data_.data(), quality);
    } else if (ext == ".bmp") {
        result = stbi_write_bmp(path.string().c_str(), width_, height_, channels_, data_.data());
    } else {
        return ImageError{5, "Unsupported format: " + ext};
    }

    if (result == 0) {
        return ImageError{6, "Failed to write image: " + path.string()};
    }

    return {};
}

auto Image::create(int width, int height, int channels) -> Image {
    return Image(width, height, channels);
}

uint8_t& Image::at(int x, int y, int c) {
    return data_[(static_cast<size_t>(y) * width_ + x) * channels_ + c];
}

uint8_t Image::at(int x, int y, int c) const {
    return data_[(static_cast<size_t>(y) * width_ + x) * channels_ + c];
}

uint8_t* Image::row(int y) {
    return data_.data() + static_cast<size_t>(y) * width_ * channels_;
}

const uint8_t* Image::row(int y) const {
    return data_.data() + static_cast<size_t>(y) * width_ * channels_;
}

auto Image::convert(int new_channels) const
    -> expected<Image, ImageError> {
    if (!valid()) {
        return ImageError{4, "Invalid image"};
    }

    if (new_channels == channels_) {
        Image result(width_, height_, new_channels);
        std::copy_n(data_.data(), data_.size(), result.data_.data());
        return result;
    }

    Image result(width_, height_, new_channels);
    
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            if (channels_ == 1 && new_channels == 3) {
                uint8_t v = at(x, y, 0);
                result.at(x, y, 0) = v;
                result.at(x, y, 1) = v;
                result.at(x, y, 2) = v;
            } else if (channels_ == 3 && new_channels == 1) {
                result.at(x, y, 0) = static_cast<uint8_t>(
                    0.299f * at(x, y, 0) + 0.587f * at(x, y, 1) + 0.114f * at(x, y, 2));
            } else if (channels_ == 3 && new_channels == 4) {
                result.at(x, y, 0) = at(x, y, 0);
                result.at(x, y, 1) = at(x, y, 1);
                result.at(x, y, 2) = at(x, y, 2);
                result.at(x, y, 3) = 255;
            } else if (channels_ == 4 && new_channels == 3) {
                result.at(x, y, 0) = at(x, y, 0);
                result.at(x, y, 1) = at(x, y, 1);
                result.at(x, y, 2) = at(x, y, 2);
            } else {
                return ImageError{7, "Unsupported channel conversion"};
            }
        }
    }

    return result;
}

auto Image::resize(int new_width, int new_height) const
    -> expected<Image, ImageError> {
    if (!valid()) {
        return ImageError{4, "Invalid image"};
    }

    Image result(new_width, new_height, channels_);
    
    float x_ratio = static_cast<float>(width_) / new_width;
    float y_ratio = static_cast<float>(height_) / new_height;

    for (int y = 0; y < new_height; ++y) {
        for (int x = 0; x < new_width; ++x) {
            float src_x = x * x_ratio;
            float src_y = y * y_ratio;
            
            int x0 = static_cast<int>(src_x);
            int y0 = static_cast<int>(src_y);
            int x1 = std::min(x0 + 1, width_ - 1);
            int y1 = std::min(y0 + 1, height_ - 1);
            
            float x_frac = src_x - x0;
            float y_frac = src_y - y0;

            for (int c = 0; c < channels_; ++c) {
                float v00 = at(x0, y0, c);
                float v10 = at(x1, y0, c);
                float v01 = at(x0, y1, c);
                float v11 = at(x1, y1, c);
                
                float v = v00 * (1 - x_frac) * (1 - y_frac) +
                          v10 * x_frac * (1 - y_frac) +
                          v01 * (1 - x_frac) * y_frac +
                          v11 * x_frac * y_frac;
                
                result.at(x, y, c) = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
            }
        }
    }

    return result;
}

auto Image::extract_region(int x, int y, int w, int h) const
    -> expected<Image, ImageError> {
    if (!valid()) {
        return ImageError{4, "Invalid image"};
    }
    
    if (x < 0 || y < 0 || x + w > width_ || y + h > height_) {
        return ImageError{8, "Region out of bounds"};
    }

    Image result(w, h, channels_);
    
    for (int r = 0; r < h; ++r) {
        std::copy_n(
            this->row(y + r) + x * channels_,
            static_cast<size_t>(w) * channels_,
            result.row(r)
        );
    }

    return result;
}

void Image::blend(const Image& other, int x, int y, int overlap) {
    if (!valid() || !other.valid() || channels_ != other.channels_) {
        return;
    }

    for (int oy = 0; oy < other.height_; ++oy) {
        int dst_y = y + oy;
        if (dst_y < 0 || dst_y >= height_) continue;

        for (int ox = 0; ox < other.width_; ++ox) {
            int dst_x = x + ox;
            if (dst_x < 0 || dst_x >= width_) continue;

            float weight = 1.0f;
            if (overlap > 0) {
                float wx = 1.0f, wy = 1.0f;
                if (ox < overlap) wx = static_cast<float>(ox) / overlap;
                if (oy < overlap) wy = static_cast<float>(oy) / overlap;
                if (other.width_ - ox - 1 < overlap) 
                    wx = static_cast<float>(other.width_ - ox - 1) / overlap;
                if (other.height_ - oy - 1 < overlap) 
                    wy = static_cast<float>(other.height_ - oy - 1) / overlap;
                weight = wx * wy;
            }

            for (int c = 0; c < channels_; ++c) {
                float src = other.at(ox, oy, c);
                float dst = at(dst_x, dst_y, c);
                at(dst_x, dst_y, c) = static_cast<uint8_t>(
                    dst * (1 - weight) + src * weight);
            }
        }
    }
}

namespace image_io {

auto get_info(const std::filesystem::path& path)
    -> expected<ImageInfo, ImageError> {
    if (!std::filesystem::exists(path)) {
        return ImageError{1, "File not found"};
    }

    int width, height, channels;
    int result = stbi_info(path.string().c_str(), &width, &height, &channels);
    
    if (result == 0) {
        return ImageError{2, "Failed to get image info"};
    }

    ImageInfo info;
    info.width = width;
    info.height = height;
    info.channels = channels;
    info.file_size = std::filesystem::file_size(path);
    
    std::string ext = path.extension().string();
    for (auto& c : ext) c = std::tolower(c);
    info.format = ext;

    return info;
}

bool is_supported(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (auto& c : ext) c = std::tolower(c);
    
    static const std::vector<std::string> supported = {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tga"};
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

std::vector<std::string> get_supported_formats() {
    return {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tga"};
}

} // namespace image_io

} // namespace crossscale
