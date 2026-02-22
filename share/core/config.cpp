#include "config.hpp"
#include "utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace crossscale {

// 简单的 JSON 解析辅助函数
static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";

    pos = json.find("\"", pos);
    if (pos == std::string::npos) return "";

    size_t end = json.find("\"", pos + 1);
    if (end == std::string::npos) return "";

    return json.substr(pos + 1, end - pos - 1);
}

static int extract_json_int(const std::string& json, const std::string& key, int default_val = 0) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return default_val;

    pos = json.find(":", pos);
    if (pos == std::string::npos) return default_val;

    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) ++pos;

    std::string num_str;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-')) {
        num_str += json[pos++];
    }

    return num_str.empty() ? default_val : std::stoi(num_str);
}

static bool extract_json_bool(const std::string& json, const std::string& key, bool default_val = false) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return default_val;

    pos = json.find(":", pos);
    if (pos == std::string::npos) return default_val;

    if (json.find("true", pos) != std::string::npos) return true;
    if (json.find("false", pos) != std::string::npos) return false;

    return default_val;
}

auto ProcessingConfig::load(const std::filesystem::path& path)
    -> expected<ProcessingConfig, std::string> {

    std::ifstream file(path);
    if (!file.is_open()) {
        return "Cannot open config file: " + path.string();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();

    ProcessingConfig config;
    config.tile_size = extract_json_int(json, "tile_size", 256);
    config.tile_overlap = extract_json_int(json, "tile_overlap", 16);
    config.batch_size = extract_json_int(json, "batch_size", 1);
    config.auto_tile_size = extract_json_bool(json, "auto_tile_size", false);
    config.min_tile_size = extract_json_int(json, "min_tile_size", 64);
    config.max_tile_size = extract_json_int(json, "max_tile_size", 512);
    config.max_memory_mb = extract_json_int(json, "max_memory_mb", 0);
    config.num_threads = extract_json_int(json, "num_threads", 0);
    config.use_gpu = extract_json_bool(json, "use_gpu", true);
    config.model_name = extract_json_string(json, "model_name");
    config.output_quality = extract_json_int(json, "output_quality", 90);
    config.output_format = extract_json_string(json, "output_format");

    return config;
}

auto ProcessingConfig::save(const std::filesystem::path& path) const
    -> expected<void, std::string> {

    std::ofstream file(path);
    if (!file.is_open()) {
        return "Cannot create config file: " + path.string();
    }

    file << "{\n"
         << "  \"tile_size\": " << tile_size << ",\n"
         << "  \"tile_overlap\": " << tile_overlap << ",\n"
         << "  \"batch_size\": " << batch_size << ",\n"
         << "  \"auto_tile_size\": " << (auto_tile_size ? "true" : "false") << ",\n"
         << "  \"min_tile_size\": " << min_tile_size << ",\n"
         << "  \"max_tile_size\": " << max_tile_size << ",\n"
         << "  \"max_memory_mb\": " << max_memory_mb << ",\n"
         << "  \"num_threads\": " << num_threads << ",\n"
         << "  \"use_gpu\": " << (use_gpu ? "true" : "false") << ",\n"
         << "  \"model_name\": \"" << model_name << "\",\n"
         << "  \"output_quality\": " << output_quality << ",\n"
         << "  \"output_format\": \"" << output_format << "\"\n"
         << "}\n";

    return {};
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

    return errors;
}

} // namespace crossscale
