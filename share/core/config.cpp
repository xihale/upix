#include "config.hpp"
#include "utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "glaze/glaze.hpp"

namespace crossscale {

namespace {

auto normalize_output_extension(const std::string& format) -> std::string {
    if (format == "jpeg") {
        return "jpg";
    }
    return format;
}

} // namespace

// On-disk shape for ProcessingConfig. Kept separate so the in-memory struct
// can evolve (e.g. std::filesystem::path) without coupling to the JSON layout.
// Lives in a named (non-anonymous) namespace: glaze's reflection requires the
// type to have external linkage, which anonymous-namespace types lack.
namespace detail {
struct ConfigJson {
    int tile_size = 256;
    int tile_overlap = 16;
    int batch_size = 1;
    bool auto_tile_size = false;
    int min_tile_size = 64;
    int max_tile_size = 512;
    bool use_gpu = true;
    std::string model_name;
    std::string output_format;
    int output_quality = 90;
};
} // namespace detail

auto normalize_output_format(std::string format) -> std::string {
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

bool is_supported_output_format(const std::string& format) {
    static constexpr const char* kSupportedFormats[] = {
        "png",
        "jpg",
        "jpeg",
        "webp",
        "avif"
    };
    for (const char* candidate : kSupportedFormats) {
        if (format == candidate) {
            return true;
        }
    }
    return false;
}

auto resolve_output_path(
    const std::filesystem::path& requested_path,
    const ProcessingConfig& config
) -> std::filesystem::path {
    const std::string normalized_format = normalize_output_format(config.output_format);
    if (normalized_format.empty()) {
        return requested_path;
    }

    std::filesystem::path resolved = requested_path;
    const std::string extension = normalize_output_extension(normalized_format);
    resolved.replace_extension("." + extension);
    return resolved;
}

auto ProcessingConfig::load(const std::filesystem::path& path)
    -> expected<ProcessingConfig, std::string> {

    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected("Cannot open config file: " + path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();

    detail::ConfigJson parsed;
    if (auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, json); err) {
        return std::unexpected("Failed to parse config JSON: " + glz::format_error(err, json));
    }

    ProcessingConfig config;
    config.tile_size = parsed.tile_size;
    config.tile_overlap = parsed.tile_overlap;
    config.batch_size = parsed.batch_size;
    config.auto_tile_size = parsed.auto_tile_size;
    config.min_tile_size = parsed.min_tile_size;
    config.max_tile_size = parsed.max_tile_size;
    config.use_gpu = parsed.use_gpu;
    config.model_name = parsed.model_name;
    config.output_quality = parsed.output_quality;
    config.output_format = normalize_output_format(parsed.output_format);

    return config;
}

auto ProcessingConfig::validate() const -> std::vector<std::string> {
    std::vector<std::string> errors;

    if (tile_size < 16 || tile_size > 2048) {
        errors.push_back("tile_size must be between 16 and 2048");
    }

    if (tile_overlap < 0 || tile_overlap >= tile_size / 2) {
        errors.push_back("tile_overlap must be between 0 and tile_size/2");
    }

    if (batch_size < 1 || batch_size > 64) {
        errors.push_back("batch_size must be between 1 and 64");
    }

    if (min_tile_size > max_tile_size) {
        errors.push_back("min_tile_size cannot exceed max_tile_size");
    }

    if (tile_size < min_tile_size || tile_size > max_tile_size) {
        errors.push_back("tile_size must be within [min_tile_size, max_tile_size]");
    }

    if (output_quality < 1 || output_quality > 100) {
        errors.push_back("output_quality must be between 1 and 100");
    }
    const std::string normalized_format = normalize_output_format(output_format);
    if (!normalized_format.empty() && !is_supported_output_format(normalized_format)) {
        errors.push_back("output_format must be one of: png, jpg, jpeg, webp, avif");
    }

    return errors;
}

} // namespace crossscale
