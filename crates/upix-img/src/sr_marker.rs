//! SR marker (`urn:upix:sr:1`): XMP build/probe port of `share/utils/sr_marker.cpp`.

/// Marker carried in the XMP payload of every super-resolution output this
/// tool encodes. Inputs whose bytes contain a marker built with this URN are
/// this tool's own outputs fed back in, and are skipped by default.
pub const NAMESPACE_URN: &str = "urn:upix:sr:1";

/// Attributes sit inside the same tag as the namespace declaration, well
/// within this window after the URN occurrence.
const ATTRIBUTE_WINDOW_BYTES: usize = 512;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SrMarker {
    pub model: String,
    pub scale: i32,
    pub ts: i64,
}

/// XML attribute escaping (C++ `xml_escape`).
pub(crate) fn xml_escape(text: &str) -> String {
    let mut escaped = String::with_capacity(text.len());
    for ch in text.chars() {
        match ch {
            '&' => escaped.push_str("&amp;"),
            '<' => escaped.push_str("&lt;"),
            '>' => escaped.push_str("&gt;"),
            '"' => escaped.push_str("&quot;"),
            '\'' => escaped.push_str("&apos;"),
            _ => escaped.push(ch),
        }
    }
    escaped
}

/// C++ `xml_unescape`: only the five entities above are recognized; any
/// other `&`-sequence is copied verbatim. Works on raw bytes so hostile
/// (non-UTF-8) attribute values cannot cause UB; non-UTF-8 survivors become
/// replacement chars, which only ever affects foreign markers.
fn xml_unescape_bytes(text: &[u8]) -> String {
    let mut plain = String::with_capacity(text.len());
    let mut i = 0usize;
    while i < text.len() {
        if text[i] != b'&' {
            plain.push(text[i] as char);
            i += 1;
            continue;
        }
        // One-byte-at-a-time push on ASCII keeps this byte-faithful for
        // values we wrote (UTF-8 multibyte sequences contain no 0x26 '&'
        // continuation bytes, so they pass through untouched below).
        let rest = &text[i..];
        if rest.starts_with(b"&amp;") {
            plain.push('&');
            i += 5;
        } else if rest.starts_with(b"&lt;") {
            plain.push('<');
            i += 4;
        } else if rest.starts_with(b"&gt;") {
            plain.push('>');
            i += 4;
        } else if rest.starts_with(b"&quot;") {
            plain.push('"');
            i += 6;
        } else if rest.starts_with(b"&apos;") {
            plain.push('\'');
            i += 6;
        } else {
            plain.push(text[i] as char);
            i += 1;
        }
    }
    plain
}

/// Value of attribute `cs:name="..."` inside the window (double quotes only —
/// build_xmp is the only writer this needs to read back).
fn find_attribute<'a>(window: &'a [u8], name: &str) -> Option<&'a [u8]> {
    let mut key = Vec::with_capacity(name.len() + 5);
    key.extend_from_slice(b"cs:");
    key.extend_from_slice(name.as_bytes());
    key.push(b'=');
    key.push(b'"');
    let key_pos = find_sub(window, &key)?;
    let value_pos = key_pos + key.len();
    let end_pos = value_pos + memchr(&window[value_pos..], b'"')?;
    Some(&window[value_pos..end_pos])
}

/// First index of `needle` in `hay`, like `str::find` but byte-safe.
fn find_sub(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    (0..=hay.len() - needle.len()).find(|&i| &hay[i..i + needle.len()] == needle)
}

fn memchr(hay: &[u8], needle: u8) -> Option<usize> {
    hay.iter().position(|&b| b == needle)
}

/// C++ `parse_number` (std::from_chars): the whole string must parse, no
/// leading/trailing junk.
fn parse_i32(text: &[u8]) -> Option<i32> {
    std::str::from_utf8(text).ok()?.parse::<i32>().ok()
}

fn parse_i64(text: &[u8]) -> Option<i64> {
    std::str::from_utf8(text).ok()?.parse::<i64>().ok()
}

/// Minimal well-formed XMP packet: cs:model / cs:scale / cs:ts attributes on
/// an rdf:Description. `model` is XML-attribute escaped.
pub fn build_xmp(model: &str, scale: i32, ts: i64) -> String {
    let mut xmp = String::with_capacity(192 + model.len());
    xmp.push_str("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF ");
    xmp.push_str("xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">");
    xmp.push_str("<rdf:Description xmlns:cs=\"");
    xmp.push_str(NAMESPACE_URN);
    xmp.push_str("\" cs:model=\"");
    xmp.push_str(&xml_escape(model));
    xmp.push_str("\" cs:scale=\"");
    xmp.push_str(&scale.to_string());
    xmp.push_str("\" cs:ts=\"");
    xmp.push_str(&ts.to_string());
    xmp.push_str("\"/></rdf:RDF></x:xmpmeta>");
    xmp
}

/// Byte-level scan of an encoded image for a marker. XMP is stored
/// uncompressed in PNG iTXt chunks, JPEG APP1 segments, WebP "XMP " chunks
/// and AVIF mdat, so a plain needle search over the raw file bytes finds it
/// in any of the four formats without container parsing. Returns None for
/// anything else (including a truncated/foreign-marker edge: scale and ts
/// must parse).
pub fn probe(bytes: &[u8]) -> Option<SrMarker> {
    if bytes.len() < NAMESPACE_URN.len() {
        return None;
    }
    let urn = NAMESPACE_URN.as_bytes();
    let urn_pos = find_sub(bytes, urn)?;
    // Attributes live in the same tag right after the xmlns declaration.
    let window_end = (urn_pos + ATTRIBUTE_WINDOW_BYTES).min(bytes.len());
    let window = &bytes[urn_pos..window_end];

    let model = find_attribute(window, "model")
        .map(xml_unescape_bytes)
        .unwrap_or_default();
    let scale = find_attribute(window, "scale")?;
    let ts = find_attribute(window, "ts")?;
    Some(SrMarker {
        model,
        scale: parse_i32(scale)?,
        ts: parse_i64(ts)?,
    })
}
