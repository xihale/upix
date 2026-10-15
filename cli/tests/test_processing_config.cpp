#include "args.hpp"
#include "common.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int fail(const char* message) {
    std::cerr << "FAIL processing config precedence: " << message << '\n';
    return 1;
}

}  // namespace

int main() {
    const auto config_path = std::filesystem::temp_directory_path() /
        ("upix-config-precedence-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
    {
        std::ofstream config_file(config_path);
        config_file << R"({
            "tile_size": 64,
            "tile_overlap": 0,
            "batch_size": 3,
            "auto_tile_size": false,
            "min_tile_size": 64,
            "max_tile_size": 512,
            "output_format": "webp",
            "output_quality": 77
        })";
        if (!config_file) return fail("could not write temporary config file");
    }

    args::Process args;
    auto defaults = resolve_processing_config(args);
    if (!defaults.recommend_tile_size ||
        defaults.config.tile_size != upix::kDefaultTileSize ||
        defaults.config.tile_overlap != upix::kDefaultTileOverlap ||
        defaults.config.batch_size != upix::kDefaultBatchSize) {
        std::filesystem::remove(config_path);
        return fail("no-config defaults or recommendation changed");
    }

    args.config_file = config_path.string();
    auto from_file = resolve_processing_config(args);
    if (from_file.recommend_tile_size || from_file.config.tile_size != 64 ||
        from_file.config.tile_overlap != 0 || from_file.config.batch_size != 3 ||
        from_file.config.output_format != "webp" ||
        from_file.config.output_quality != 77) {
        std::filesystem::remove(config_path);
        return fail("JSON values were not retained without CLI overrides");
    }

    args.tile_size = 128;
    args.tile_size_explicit = true;
    args.tile_overlap = 12;
    args.tile_overlap_explicit = true;
    args.batch_size = 4;
    args.batch_size_explicit = true;
    args.auto_tile = true;
    args.auto_tile_explicit = true;
    args.min_tile = 128;
    args.min_tile_explicit = true;
    args.max_tile = 256;
    args.max_tile_explicit = true;
    args.output_format = "jpg";
    auto overridden = resolve_processing_config(args);
    if (overridden.recommend_tile_size || overridden.config.tile_size != 128 ||
        overridden.config.tile_overlap != 12 || overridden.config.batch_size != 4 ||
        !overridden.config.auto_tile_size || overridden.config.min_tile_size != 128 ||
        overridden.config.max_tile_size != 256 ||
        overridden.config.output_format != "jpg" ||
        overridden.config.output_quality != 77) {
        std::filesystem::remove(config_path);
        return fail("explicit CLI values did not override JSON while preserving other keys");
    }

    args::Process pinned;
    pinned.tile_size = 192;
    pinned.tile_size_explicit = true;
    auto explicit_tile = resolve_processing_config(pinned);
    if (explicit_tile.recommend_tile_size || explicit_tile.config.tile_size != 192) {
        std::filesystem::remove(config_path);
        return fail("explicit tile size did not suppress the derived recommendation");
    }

    std::filesystem::remove(config_path);
    return 0;
}
