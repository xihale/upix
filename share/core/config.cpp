#include "config.hpp"
#include "utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <cctype>
#include "glaze/glaze.hpp"

namespace upix {

namespace {

// Locale-independent ASCII helpers matching C-locale isspace()/tolower()
// exactly (0x09-0x0D + 0x20, A-Z), so the normalization helpers stay constexpr.
constexpr auto ascii_tolower(unsigned char ch) -> char {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a')
                                    : static_cast<char>(ch);
}
constexpr auto ascii_isspace(unsigned char ch) -> bool {
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

constexpr auto normalize_output_extension(std::string_view format) -> std::string {
    if (format == "jpeg") {
        return "jpg";
    }
    return std::string(format);
}

} // namespace

// On-disk shape for ProcessingConfig. Kept separate so the in-memory struct
// can evolve (e.g. std::filesystem::path) without coupling to the JSON layout.
// Lives in a named (non-anonymous) namespace: glaze's reflection requires the
// type to have external linkage, which anonymous-namespace types lack.
namespace detail {
struct ConfigJson {
    int tile_size = kDefaultTileSize;
    int tile_overlap = kDefaultTileOverlap;
    int batch_size = kDefaultBatchSize;
    bool auto_tile_size = false;
    int min_tile_size = kDefaultMinTileSize;
    int max_tile_size = kDefaultMaxTileSize;
    bool use_gpu = true;
    std::string model_name;
    std::string output_format;
    int output_quality = kDefaultOutputQuality;
    EncoderOptions encoder;
};
} // namespace detail

auto normalize_output_format(std::string_view format) -> std::string {
    std::string normalized;
    normalized.reserve(format.size());
    for (char raw : format) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (raw == '.' || ascii_isspace(ch)) {
            continue;
        }
        normalized += ascii_tolower(ch);
    }
    return normalized;
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
    config.encoder = parsed.encoder;

    return config;
}

auto ProcessingConfig::validate() const -> std::vector<std::string> {
    std::vector<std::string> errors;

    if (tile_size < kTileSizeMin || tile_size > kTileSizeMax) {
        errors.push_back("tile_size must be between 16 and 2048");
    }

    if (tile_overlap < 0 || tile_overlap >= tile_size / 2) {
        errors.push_back("tile_overlap must be between 0 and tile_size/2");
    }

    if (batch_size < kBatchSizeMin || batch_size > kBatchSizeMax) {
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

    if (encoder.png_level < -1 || encoder.png_level > 9) {
        errors.push_back("encoder.png_level must be -1 (auto) or 0-9");
    }
    if (encoder.jpeg_subsampling != 420 && encoder.jpeg_subsampling != 422 &&
        encoder.jpeg_subsampling != 444) {
        errors.push_back("encoder.jpeg_subsampling must be 420, 422 or 444");
    }
    if (encoder.webp_method < -1 || encoder.webp_method > 6) {
        errors.push_back("encoder.webp_method must be -1 (auto) or 0-6");
    }
    if (encoder.avif_speed < 0 || encoder.avif_speed > 10) {
        errors.push_back("encoder.avif_speed must be between 0 and 10");
    }
    if (encoder.avif_subsampling != 420 && encoder.avif_subsampling != 444) {
        errors.push_back("encoder.avif_subsampling must be 420 or 444");
    }
    if (encoder.avif_threads < 0 || encoder.avif_threads > 64) {
        errors.push_back("encoder.avif_threads must be between 0 (auto) and 64");
    }

    return errors;
}

auto sanitize_model_for_filename(std::string_view model) -> std::string {
    std::string out;
    out.reserve(model.size());
    for (char c : model) {
        switch (c) {
            case '/': case '\\': case ' ': case '\t':
                out += '_';
                break;
            default:
                out += c;
        }
    }
    return out.empty() ? "model" : out;
}

} // namespace upix
