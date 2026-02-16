#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include "expected.hpp"

namespace crossscale {

enum class PixelFormat {
    RGB,
    RGBA,
    GRAY,
    GRAY_ALPHA
};

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
    
    static auto load_from_memory(const uint8_t* data, size_t size)
        -> expected<Image, ImageError>;
    
    auto save(const std::filesystem::path& path, int quality = 90) const
        -> expected<void, ImageError>;
    
    auto save_to_memory(const std::string& format = "png", int quality = 90) const
        -> expected<std::vector<uint8_t>, ImageError>;

    // Create empty image
    static auto create(int width, int height, int channels)
        -> Image;
    
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
    
    // Pixel access
    uint8_t& at(int x, int y, int c);
    uint8_t at(int x, int y, int c) const;
    
    // Get row pointer
    uint8_t* row(int y);
    const uint8_t* row(int y) const;
    
    // Convert format
    auto convert(int new_channels) const
        -> expected<Image, ImageError>;
    
    auto resize(int new_width, int new_height) const
        -> expected<Image, ImageError>;
    
    auto extract_region(int x, int y, int w, int h) const
        -> expected<Image, ImageError>;
    
    // Blend another image onto this one (for tile merging)
    void blend(const Image& other, int x, int y, int overlap = 0);

private:
    auto save_png(const std::filesystem::path& path) const -> expected<void, ImageError>;
    auto save_jpeg(const std::filesystem::path& path, int quality) const -> expected<void, ImageError>;
    auto save_webp(const std::filesystem::path& path, int quality) const -> expected<void, ImageError>;

    std::vector<uint8_t> data_;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 3;  // Default RGB
};

// Image loading utilities
namespace image_io {
    // Get image info without loading full image
    struct ImageInfo {
        int width;
        int height;
        int channels;
        std::string format;
        size_t file_size;
    };
    
    auto get_info(const std::filesystem::path& path)
        -> expected<ImageInfo, ImageError>;
    
    // Check if file is a supported image format
    bool is_supported(const std::filesystem::path& path);
    
    // Get supported formats
    std::vector<std::string> get_supported_formats();
}

} // namespace crossscale
