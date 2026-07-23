#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include "expected.hpp"

namespace crossscale {

struct ImageError {
    int code;
    std::string message;
};

class Image {
public:
    Image() = default;
    Image(int width, int height, int channels);

    // Move operations
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    // No copy (expensive)
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    ~Image() = default;

    // Load from file (supports PNG, JPEG, WebP, BMP)
    static auto load(const std::filesystem::path& path)
        -> expected<Image, ImageError>;

    auto save(const std::filesystem::path& path, int quality = 90) const
        -> expected<void, ImageError>;

    auto save_to_memory(const std::string& format = "png", int quality = 90) const
        -> expected<std::vector<uint8_t>, ImageError>;

    // Get pixel data
    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }

    // Dimensions
    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    size_t size() const { return data_.size(); }

    // Check if valid
    bool valid() const { return !data_.empty() && width_ > 0 && height_ > 0; }

private:
    std::vector<uint8_t> data_;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 3;  // Default RGB
};

} // namespace crossscale
