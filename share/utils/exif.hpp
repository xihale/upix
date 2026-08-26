#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace upix {

class Image;

// EXIF orientation (JPEG APP1 tag 0x0112): parse + bake-in helpers.
//
// The PC decoders (libjpeg/libpng/libwebp/libavif) hand back the *stored*
// pixel grid; for portrait shots with orientation 5-8 that grid is rotated
// relative to what the camera saw. Outputs are re-encoded from pixels and
// therefore carry no EXIF, so the correct fix is to bake the transform into
// the decoded pixels — the equivalent of PIL exif_transpose / libvips
// autorotate. (Other metadata — capture date, GPS — is lost on output either
// way; that is pre-existing behavior, not this module's concern.)
namespace exif {

// Scan a JPEG byte stream for the APP1 "Exif\0\0" segment and read the
// IFD0 Orientation tag. Returns 1-8; returns 1 (no-op semantics) for
// non-JPEG input, missing/unreadable EXIF, or any parse error — callers
// treat 1 as "nothing to do". Only the header region (up to SOS) is read;
// hand-rolled with strict bounds checks, safe on truncated/hostile input.
int parse_jpeg_exif_orientation(const uint8_t* data, size_t len);

// Apply the orientation transform in place to 8-bit interleaved pixels
// (1/3/4 channels). `pixels` must hold at least w*h*channels bytes; extra
// tail bytes (e.g. Image's +16 upload slack) are preserved. Orientations
// 2-4 are geometry-preserving flips; 5-8 transpose and swap w/h (both are
// updated on success). Returns true when a transform was applied; false
// (pixels untouched) for orientation 1 or invalid arguments.
bool apply_exif_orientation_u8(std::vector<uint8_t>& pixels, int& w, int& h,
                               int channels, int orientation);

// Same transform for a decoded upix::Image: flips run in place, the
// transposing orientations rebuild the frame (one pixel copy) so the +16
// upload slack invariant survives. Returns true when the image changed.
bool autorotate_image(Image& image, int orientation);

}  // namespace exif
}  // namespace upix
