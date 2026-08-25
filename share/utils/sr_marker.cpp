#include "sr_marker.hpp"

#include <charconv>

namespace upix::sr_marker {

namespace {

// XMP is tiny; the attributes sit inside the same tag as the namespace
// declaration, well within this window after the URN occurrence.
constexpr size_t kAttributeWindowBytes = 512;

auto xml_escape(std::string_view text) -> std::string {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += ch;
        }
    }
    return escaped;
}

auto xml_unescape(std::string_view text) -> std::string {
    std::string plain;
    plain.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '&') {
            plain += text[i++];
            continue;
        }
        if (text.compare(i, 5, "&amp;") == 0) { plain += '&'; i += 5; }
        else if (text.compare(i, 4, "&lt;") == 0) { plain += '<'; i += 4; }
        else if (text.compare(i, 4, "&gt;") == 0) { plain += '>'; i += 4; }
        else if (text.compare(i, 6, "&quot;") == 0) { plain += '"'; i += 6; }
        else if (text.compare(i, 6, "&apos;") == 0) { plain += '\''; i += 6; }
        else { plain += text[i++]; }
    }
    return plain;
}

// Value of attribute `name="..."` inside the window (double quotes only —
// build_xmp is the only writer this needs to read back).
auto find_attribute(std::string_view window, std::string_view name)
    -> std::optional<std::string_view> {
    std::string key;
    key.reserve(name.size() + 2);
    key += "cs:";
    key += name;
    key += "=\"";
    const size_t key_pos = window.find(key);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t value_pos = key_pos + key.size();
    const size_t end_pos = window.find('"', value_pos);
    if (end_pos == std::string_view::npos) {
        return std::nullopt;
    }
    return window.substr(value_pos, end_pos - value_pos);
}

template <typename T>
auto parse_number(std::string_view text, T& out) -> bool {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

}  // namespace

auto build_xmp(std::string_view model, int scale, int64_t ts) -> std::string {
    std::string xmp;
    xmp.reserve(192 + model.size());
    xmp += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
           "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
           "<rdf:Description xmlns:cs=\"";
    xmp += kNamespaceUrn;
    xmp += "\" cs:model=\"";
    xmp += xml_escape(model);
    xmp += "\" cs:scale=\"";
    xmp += std::to_string(scale);
    xmp += "\" cs:ts=\"";
    xmp += std::to_string(ts);
    xmp += "\"/></rdf:RDF></x:xmpmeta>";
    return xmp;
}

auto probe(const uint8_t* data, size_t size) -> std::optional<SrMarker> {
    if (data == nullptr || size < kNamespaceUrn.size()) {
        return std::nullopt;
    }
    const auto bytes = std::string_view(
        reinterpret_cast<const char*>(data), size);
    const size_t urn_pos = bytes.find(kNamespaceUrn);
    if (urn_pos == std::string_view::npos) {
        return std::nullopt;
    }
    // Attributes live in the same tag right after the xmlns declaration.
    const size_t window_len = std::min(kAttributeWindowBytes, bytes.size() - urn_pos);
    const auto window = bytes.substr(urn_pos, window_len);

    SrMarker marker;
    if (const auto model = find_attribute(window, "model")) {
        marker.model = xml_unescape(*model);
    }
    const auto scale = find_attribute(window, "scale");
    const auto ts = find_attribute(window, "ts");
    if (!scale || !ts) {
        return std::nullopt;
    }
    if (!parse_number(*scale, marker.scale) || !parse_number(*ts, marker.ts)) {
        return std::nullopt;
    }
    return marker;
}

}  // namespace upix::sr_marker
