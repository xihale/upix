#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include "expected.hpp"

namespace upix {

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

    // Load from file (PNG, JPEG, WebP; AVIF via system libavif if present)
    static auto load(const std::filesystem::path& path)
        -> expected<Image, ImageError>;

    // Raw encoded file bytes, no decode. For callers that sniff metadata
    // (sr_marker::probe) and decode later from the same buffer.
    static auto load_raw(const std::filesystem::path& path)
        -> expected<std::vector<uint8_t>, ImageError>;

    // Decode an in-memory encoded file (same formats as load()). The phone
    // path uses this to decode on the PC so the phone ships compressed bytes
    // instead of raw pixels.
    static auto load_from_memory(const std::string& format,
                                 const std::vector<uint8_t>& data)
        -> expected<Image, ImageError>;

    // `xmp`: optional XMP payload embedded as a metadata marker (PNG iTXt,
    // JPEG APP1, WebP "XMP " chunk, AVIF XMP item). Empty = no marker.
    auto save(const std::filesystem::path& path, int quality = 90,
              const std::string& xmp = {}) const
        -> expected<void, ImageError>;

    auto save_to_memory(const std::string& format = "png", int quality = 90,
                        const std::string& xmp = {}) const
        -> expected<std::vector<uint8_t>, ImageError>;

    // Get pixel data
    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }

    // Dimensions
    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    // Pixel bytes (w*h*c) — NOT data_.size(), which carries +16 upload slack.
    size_t size() const {
        return static_cast<size_t>(width_) * height_ * channels_;
    }

    // Check if valid
    bool valid() const { return !data_.empty() && width_ > 0 && height_ > 0; }

private:
    std::vector<uint8_t> data_;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 3;  // Default RGB
};

} // namespace upix
