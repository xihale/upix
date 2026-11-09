#pragma once

#include <cstdint>

namespace upix {

struct TileRegion {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t overlap;
};

struct TileConfig {
    int tile_size = 256;
    int overlap = 16;
    int batch_size = 1;
};

struct ProcessingProgress {
    int total_tiles = 0;
    int completed_tiles = 0;
    uint32_t current_tile_x = 0;
    uint32_t current_tile_y = 0;

    float percentage() const {
        if (total_tiles == 0) return 0.0f;
        return static_cast<float>(completed_tiles) / total_tiles * 100.0f;
    }
};

} // namespace upix
