#pragma once

namespace upix {

// Per-format advanced encoder parameters. A default-constructed struct
// reproduces the historical behavior exactly: the -1 sentinels derive from
// output_quality (PNG) or fall back to the library default, and the explicit
// values match what the encoders hard-coded before this struct existed.
struct EncoderOptions {
    int png_level = -1;           // zlib-style 0-9; -1 = derive from output_quality
    int jpeg_subsampling = 420;   // 420 / 422 / 444
    int webp_method = -1;         // 0-6; -1 = libwebp default (4)
    bool webp_lossless = false;
    int avif_speed = 6;           // 0-10; 6 = still-image sweet spot
    int avif_subsampling = 420;   // 420 / 444
    int avif_threads = 0;         // 0 = hardware concurrency
};

} // namespace upix
