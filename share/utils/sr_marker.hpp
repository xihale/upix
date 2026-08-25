#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace upix::sr_marker {

// Marker carried in the XMP payload of every super-resolution output this
// tool encodes. Inputs whose bytes contain a marker built with this URN are
// this tool's own outputs fed back in, and are skipped by default
// (`--force` on the CLI, a toggle in the app).
inline constexpr std::string_view kNamespaceUrn = "urn:upix:sr:1";

struct SrMarker {
    std::string model;
    int scale = 0;
    int64_t ts = 0;
};

// Minimal well-formed XMP packet: cs:model / cs:scale / cs:ts attributes on
// an rdf:Description. `model` is XML-attribute escaped.
auto build_xmp(std::string_view model, int scale, int64_t ts) -> std::string;

// Byte-level scan of an encoded image for a marker. XMP is stored
// uncompressed in PNG iTXt chunks, JPEG APP1 segments, WebP "XMP " chunks
// and AVIF mdat, so a plain needle search over the raw file bytes finds it
// in any of the four formats without container parsing — the same code runs
// on host and Android. Returns nullopt for anything else (including a
// truncated/foreign-marker edge: scale and ts must parse).
auto probe(const uint8_t* data, size_t size) -> std::optional<SrMarker>;

}  // namespace upix::sr_marker
