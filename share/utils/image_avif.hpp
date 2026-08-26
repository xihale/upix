#pragma once

#include "image.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace upix::avif_codec {

// True when the buffer is an ISOBMFF file whose ftyp brands include avif/avis.
auto looks_like_avif(const uint8_t* data, size_t size) -> bool;

// Walks ISOBMFF boxes for the primary `ispe` width/height. {0,0} if not found.
auto sniff_dimensions(const uint8_t* data, size_t size) -> std::pair<uint32_t, uint32_t>;

// Decode/encode via dlopen of the system libavif (default build), or via a
// compile-time linked libavif when built with xmake --avif-encoder=y /
// --avif-decoder=y (UPIX_WITH_AVIF_ENCODER / UPIX_WITH_AVIF_DECODER).
// save() embeds `xmp` as the AVIF XMP item when non-empty and the libavif in
// use exports avifImageSetMetadataXMP (silently unmarked otherwise).
// `opts` carries the AVIF knobs (speed / subsampling / threads); defaults
// reproduce the historical behavior (speed 6, YUV420, all cores).
auto load(const std::vector<uint8_t>& buffer) -> expected<Image, ImageError>;
auto save(const Image& image, int quality, const std::string& xmp = {},
          const EncoderOptions& opts = {})
    -> expected<std::vector<uint8_t>, ImageError>;

// Quiet, cached probes: does the system libavif load with usable decode /
// encode symbols? Same acceptance rules as load()/save() but they never log,
// so stdout stays clean (shell completions consume it). Encode implies decode.
auto avif_decode_available() -> bool;
auto avif_encode_available() -> bool;

}  // namespace upix::avif_codec
