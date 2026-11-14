//! Image container codecs (PNG, JPEG), EXIF orientation parse & bake-in,
//! `urn:upix:sr:1` XMP marker probe/build, WebP `VP8X`/`XMP ` muxer, and
//! SHA-256 hex digest — pure Zig, zero libc, safe for both host and
//! `wasm32-freestanding`.

const std = @import("std");
const math = std.math;
const mem = std.mem;
const Allocator = mem.Allocator;
const flate = std.compress.flate;

pub const NAMESPACE_URN: []const u8 = "urn:upix:sr:1";
pub const JPEG_XMP_NAMESPACE: []const u8 = "http://ns.adobe.com/xap/1.0/\x00";
const ATTRIBUTE_WINDOW_BYTES: usize = 512;

pub const CodecError = error{
    OutOfMemory,
    InvalidDimensions,
    BufferTooSmall,
    UnsupportedChannels,
    InvalidPng,
    InvalidJpeg,
    InvalidWebp,
    CompressionFailed,
    DecompressionFailed,
    XmpTooLarge,
};

pub const SrMarker = struct {
    model: []u8,
    scale: i32,
    ts: i64,

    pub fn deinit(self: *SrMarker, allocator: Allocator) void {
        allocator.free(self.model);
    }
};

pub const DecodedImage = struct {
    width: u32,
    height: u32,
    channels: u32,
    data: []u8,

    pub fn deinit(self: *DecodedImage, allocator: Allocator) void {
        allocator.free(self.data);
    }
};

// ============================================================================
// SHA-256 Hex Digest
// ============================================================================

const HEX_LOWER = "0123456789abcdef";

/// Compute lowercase 64-character SHA-256 hex digest into a stack buffer.
pub fn sha256HexBuf(bytes: []const u8) [64]u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    var out: [64]u8 = undefined;
    for (digest, 0..) |b, i| {
        out[i * 2] = HEX_LOWER[b >> 4];
        out[i * 2 + 1] = HEX_LOWER[b & 0x0F];
    }
    return out;
}

/// Compute lowercase 64-character SHA-256 hex digest as an allocated slice.
pub fn sha256HexAlloc(allocator: Allocator, bytes: []const u8) Allocator.Error![]u8 {
    const buf = sha256HexBuf(bytes);
    return allocator.dupe(u8, &buf);
}

// ============================================================================
// `urn:upix:sr:1` XMP Marker Build & Probe
// ============================================================================

fn appendXmlEscaped(list: *std.ArrayList(u8), allocator: Allocator, text: []const u8) Allocator.Error!void {
    for (text) |ch| {
        switch (ch) {
            '&' => try list.appendSlice(allocator, "&amp;"),
            '<' => try list.appendSlice(allocator, "&lt;"),
            '>' => try list.appendSlice(allocator, "&gt;"),
            '"' => try list.appendSlice(allocator, "&quot;"),
            '\'' => try list.appendSlice(allocator, "&apos;"),
            else => try list.append(allocator, ch),
        }
    }
}

fn xmlUnescapeAlloc(allocator: Allocator, text: []const u8) Allocator.Error![]u8 {
    var raw: std.ArrayList(u8) = .empty;
    errdefer raw.deinit(allocator);
    try raw.ensureTotalCapacity(allocator, text.len);

    var i: usize = 0;
    while (i < text.len) {
        if (text[i] != '&') {
            try raw.append(allocator, text[i]);
            i += 1;
            continue;
        }
        const rest = text[i..];
        if (mem.startsWith(u8, rest, "&amp;")) {
            try raw.append(allocator, '&');
            i += 5;
        } else if (mem.startsWith(u8, rest, "&lt;")) {
            try raw.append(allocator, '<');
            i += 4;
        } else if (mem.startsWith(u8, rest, "&gt;")) {
            try raw.append(allocator, '>');
            i += 4;
        } else if (mem.startsWith(u8, rest, "&quot;")) {
            try raw.append(allocator, '"');
            i += 6;
        } else if (mem.startsWith(u8, rest, "&apos;")) {
            try raw.append(allocator, '\'');
            i += 6;
        } else {
            try raw.append(allocator, text[i]);
            i += 1;
        }
    }
    return raw.toOwnedSlice(allocator);
}

fn findXmlAttribute(window: []const u8, name: []const u8) ?[]const u8 {
    var key_buf: [64]u8 = undefined;
    const needed = 3 + name.len + 2; // "cs:" + name + "=\""
    if (needed > key_buf.len) return null;
    @memcpy(key_buf[0..3], "cs:");
    @memcpy(key_buf[3 .. 3 + name.len], name);
    key_buf[3 + name.len] = '=';
    key_buf[4 + name.len] = '"';
    const key = key_buf[0..needed];

    const key_pos = mem.indexOf(u8, window, key) orelse return null;
    const val_pos = key_pos + key.len;
    const end_rel = mem.indexOfScalarPos(u8, window, val_pos, '"') orelse return null;
    return window[val_pos..end_rel];
}

/// Build `urn:upix:sr:1` XMP packet identical to `upix_img::sr_marker::build_xmp`.
pub fn buildSrXmp(allocator: Allocator, model: []const u8, scale: i32, ts: i64) Allocator.Error![]u8 {
    var xmp: std.ArrayList(u8) = .empty;
    errdefer xmp.deinit(allocator);
    try xmp.ensureTotalCapacity(allocator, 192 + model.len);

    try xmp.appendSlice(allocator, "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF ");
    try xmp.appendSlice(allocator, "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">");
    try xmp.appendSlice(allocator, "<rdf:Description xmlns:cs=\"");
    try xmp.appendSlice(allocator, NAMESPACE_URN);
    try xmp.appendSlice(allocator, "\" cs:model=\"");
    try appendXmlEscaped(&xmp, allocator, model);
    try xmp.appendSlice(allocator, "\" cs:scale=\"");
    var num_buf: [32]u8 = undefined;
    const scale_str = std.fmt.bufPrint(&num_buf, "{d}", .{scale}) catch unreachable;
    try xmp.appendSlice(allocator, scale_str);
    try xmp.appendSlice(allocator, "\" cs:ts=\"");
    const ts_str = std.fmt.bufPrint(&num_buf, "{d}", .{ts}) catch unreachable;
    try xmp.appendSlice(allocator, ts_str);
    try xmp.appendSlice(allocator, "\"/></rdf:RDF></x:xmpmeta>");
    return xmp.toOwnedSlice(allocator);
}

/// Substring scan for `urn:upix:sr:1` across PNG, JPEG, WebP, and AVIF containers.
pub fn probeSrMarker(allocator: Allocator, bytes: []const u8) Allocator.Error!?SrMarker {
    if (bytes.len < NAMESPACE_URN.len) return null;
    const urn_pos = mem.indexOf(u8, bytes, NAMESPACE_URN) orelse return null;
    const window_end = @min(urn_pos + ATTRIBUTE_WINDOW_BYTES, bytes.len);
    const window = bytes[urn_pos..window_end];

    const scale_bytes = findXmlAttribute(window, "scale") orelse return null;
    const scale = std.fmt.parseInt(i32, scale_bytes, 10) catch return null;
    const ts_bytes = findXmlAttribute(window, "ts") orelse return null;
    const ts = std.fmt.parseInt(i64, ts_bytes, 10) catch return null;

    const model = if (findXmlAttribute(window, "model")) |m|
        try xmlUnescapeAlloc(allocator, m)
    else
        try allocator.dupe(u8, "");

    return SrMarker{
        .model = model,
        .scale = scale,
        .ts = ts,
    };
}

/// Probe `urn:upix:sr:1` marker and serialize as JSON (`null` or `{"model":...,"scale":...,"ts":...}`).
pub fn probeSrMarkerJson(allocator: Allocator, bytes: []const u8) Allocator.Error![]u8 {
    var maybe = try probeSrMarker(allocator, bytes);
    if (maybe == null) {
        return allocator.dupe(u8, "null");
    }
    defer maybe.?.deinit(allocator);
    const m = maybe.?;

    var aw: std.Io.Writer.Allocating = .init(allocator);
    errdefer aw.deinit();
    std.json.Stringify.value(.{
        .model = m.model,
        .scale = m.scale,
        .ts = m.ts,
    }, .{}, &aw.writer) catch return error.OutOfMemory;
    return aw.toOwnedSlice();
}

// ============================================================================
// EXIF Orientation Parser & Pixel Autorotation
// ============================================================================

fn readU16(p: []const u8, little: bool) u16 {
    return if (little) mem.readInt(u16, p[0..2], .little) else mem.readInt(u16, p[0..2], .big);
}

fn readU32(p: []const u8, little: bool) u32 {
    return if (little) mem.readInt(u32, p[0..4], .little) else mem.readInt(u32, p[0..4], .big);
}

fn orientationFromTiff(tiff: []const u8) u8 {
    if (tiff.len < 8) return 1;
    const little: bool = if (tiff[0] == 'I' and tiff[1] == 'I')
        true
    else if (tiff[0] == 'M' and tiff[1] == 'M')
        false
    else
        return 1;

    if (readU16(tiff[2..4], little) != 42) return 1;
    const ifd_off: usize = readU32(tiff[4..8], little);
    if (ifd_off + 2 > tiff.len) return 1;
    const entries: usize = readU16(tiff[ifd_off .. ifd_off + 2], little);
    const dir_bytes = std.math.mul(usize, entries, 12) catch return 1;
    if (ifd_off + 2 + dir_bytes > tiff.len) return 1;

    var i: usize = 0;
    while (i < entries) : (i += 1) {
        const e = tiff[ifd_off + 2 + 12 * i ..];
        if (readU16(e[0..2], little) == 0x0112 and
            readU16(e[2..4], little) == 3 and
            readU32(e[4..8], little) >= 1)
        {
            const val = readU16(e[8..10], little);
            return if (val >= 1 and val <= 8) @intCast(val) else 1;
        }
    }
    return 1;
}

/// Parse JPEG EXIF IFD0 tag 0x0112 orientation (returns 1..8, or 1 if absent/invalid).
pub fn parseJpegExifOrientation(data: []const u8) u8 {
    if (data.len < 4 or data[0] != 0xFF or data[1] != 0xD8) return 1;
    var pos: usize = 2;
    while (pos + 4 <= data.len) {
        if (data[pos] != 0xFF) return 1;
        var m = pos + 1;
        while (m < data.len and data[m] == 0xFF) : (m += 1) {}
        if (m >= data.len) return 1;
        const marker = data[m];
        if (marker == 0x01 or (marker >= 0xD0 and marker <= 0xD8)) {
            pos = m + 1;
            continue;
        }
        if (marker == 0xDA or marker == 0xD9) return 1;
        if (data.len - m < 3) return 1;
        const seg_len: usize = mem.readInt(u16, data[m + 1 ..][0..2], .big);
        if (seg_len < 2) return 1;
        const payload = seg_len - 2;
        const seg_start = m + 3;
        if (payload > data.len - seg_start) return 1;
        if (marker == 0xE1 and payload >= 6 and mem.eql(u8, data[seg_start .. seg_start + 6], "Exif\x00\x00")) {
            return orientationFromTiff(data[seg_start + 6 .. seg_start + payload]);
        }
        pos = seg_start + payload;
    }
    return 1;
}

fn autorotatePixels(
    allocator: Allocator,
    pixels: *[]u8,
    width: *u32,
    height: *u32,
    c: usize,
    orientation: u8,
) Allocator.Error!void {
    const w: usize = width.*;
    const h: usize = height.*;
    const p = pixels.*;
    switch (orientation) {
        2 => {
            const stride = w * c;
            for (0..h) |y| {
                const row = p[y * stride .. (y + 1) * stride];
                for (0..w / 2) |x| {
                    for (0..c) |k| {
                        mem.swap(u8, &row[x * c + k], &row[(w - 1 - x) * c + k]);
                    }
                }
            }
        },
        3 => {
            const n = w * h;
            for (0..n / 2) |i| {
                for (0..c) |k| {
                    mem.swap(u8, &p[i * c + k], &p[(n - 1 - i) * c + k]);
                }
            }
        },
        4 => {
            const stride = w * c;
            const tmp = try allocator.alloc(u8, stride);
            defer allocator.free(tmp);
            for (0..h / 2) |y| {
                const a = y * stride;
                const b = (h - 1 - y) * stride;
                @memcpy(tmp, p[a .. a + stride]);
                @memcpy(p[a .. a + stride], p[b .. b + stride]);
                @memcpy(p[b .. b + stride], tmp);
            }
        },
        5, 6, 7, 8 => {
            const ow = h;
            const oh = w;
            const dst = try allocator.alloc(u8, ow * oh * c);
            for (0..oh) |y| {
                for (0..ow) |x| {
                    const sx: usize = switch (orientation) {
                        5, 6 => y,
                        7, 8 => w - 1 - y,
                        else => unreachable,
                    };
                    const sy: usize = switch (orientation) {
                        5, 8 => x,
                        6, 7 => h - 1 - x,
                        else => unreachable,
                    };
                    const s_off = (sy * w + sx) * c;
                    const d_off = (y * ow + x) * c;
                    @memcpy(dst[d_off .. d_off + c], p[s_off .. s_off + c]);
                }
            }
            allocator.free(p);
            pixels.* = dst;
            width.* = @intCast(ow);
            height.* = @intCast(oh);
        },
        else => {},
    }
}

// ============================================================================
// WebP RIFF `"XMP "` Chunk Muxer (RFC 9649 §2.5)
// ============================================================================

fn webpBitstreamDims(ctype: []const u8, p: []const u8) ?struct { u32, u32 } {
    if (mem.eql(u8, ctype, "VP8L")) {
        if (p.len < 5 or p[0] != 0x2F) return null;
        const bits = mem.readInt(u32, p[1..5], .little);
        return .{ (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1 };
    }
    if (mem.eql(u8, ctype, "VP8 ")) {
        if (p.len < 10 or p[3] != 0x9D or p[4] != 0x01 or p[5] != 0x2A) return null;
        const w: u32 = (@as(u32, p[6]) | (@as(u32, p[7]) << 8)) & 0x3FFF;
        const h: u32 = (@as(u32, p[8]) | (@as(u32, p[9]) << 8)) & 0x3FFF;
        return .{ w, h };
    }
    return null;
}

/// Attach or replace an `"XMP "` metadata chunk inside an encoded WebP RIFF container.
pub fn webpAttachXmp(allocator: Allocator, webp_bytes: []const u8, xmp: []const u8) CodecError![]u8 {
    if (xmp.len == 0) {
        return allocator.dupe(u8, webp_bytes);
    }
    if (webp_bytes.len < 20 or !mem.eql(u8, webp_bytes[0..4], "RIFF") or !mem.eql(u8, webp_bytes[8..12], "WEBP")) {
        return error.InvalidWebp;
    }

    const riff_size: usize = mem.readInt(u32, webp_bytes[4..8], .little);
    const body_end = @min(riff_size +| 8, webp_bytes.len);

    var offset: usize = 12;
    var vp8x_flags: u8 = 0;
    var vp8x_dims: ?struct { u32, u32 } = null;
    var has_alpha = false;
    var iccp_chunk: ?[]const u8 = null;
    var exif_chunk: ?[]const u8 = null;
    var image_chunks: std.ArrayList([]const u8) = .empty;
    defer image_chunks.deinit(allocator);
    var first_image_ctype: ?[]const u8 = null;
    var first_image_payload: ?[]const u8 = null;

    while (offset + 8 <= body_end) {
        const ctype = webp_bytes[offset .. offset + 4];
        const csize: usize = mem.readInt(u32, webp_bytes[offset + 4 ..][0..4], .little);
        const payload_start = offset + 8;
        const payload_end = std.math.add(usize, payload_start, csize) catch break;
        if (payload_end > body_end) break;
        const padded_end = @min(payload_end + (csize & 1), body_end);
        const full_chunk = webp_bytes[offset..padded_end];
        const payload = webp_bytes[payload_start..payload_end];

        if (mem.eql(u8, ctype, "VP8X") and payload.len >= 10) {
            vp8x_flags = payload[0];
            const w1: u32 = @as(u32, payload[4]) | (@as(u32, payload[5]) << 8) | (@as(u32, payload[6]) << 16);
            const h1: u32 = @as(u32, payload[7]) | (@as(u32, payload[8]) << 8) | (@as(u32, payload[9]) << 16);
            vp8x_dims = .{ w1 + 1, h1 + 1 };
        } else if (mem.eql(u8, ctype, "ICCP")) {
            iccp_chunk = full_chunk;
        } else if (mem.eql(u8, ctype, "EXIF")) {
            exif_chunk = full_chunk;
        } else if (mem.eql(u8, ctype, "ALPH")) {
            has_alpha = true;
            try image_chunks.append(allocator, full_chunk);
        } else if (mem.eql(u8, ctype, "VP8 ") or mem.eql(u8, ctype, "VP8L")) {
            if (mem.eql(u8, ctype, "VP8L") and payload.len >= 5 and ((payload[4] >> 4) & 1) == 1) {
                has_alpha = true;
            }
            if (first_image_ctype == null) {
                first_image_ctype = ctype;
                first_image_payload = payload;
            }
            try image_chunks.append(allocator, full_chunk);
        } else if (mem.eql(u8, ctype, "XMP ")) {
            // Replaced below.
        } else {
            try image_chunks.append(allocator, full_chunk);
        }
        offset = padded_end;
    }

    const dims = vp8x_dims orelse blk: {
        const ct = first_image_ctype orelse return error.InvalidWebp;
        const pl = first_image_payload orelse return error.InvalidWebp;
        break :blk webpBitstreamDims(ct, pl) orelse return error.InvalidWebp;
    };
    const canvas_w = dims[0];
    const canvas_h = dims[1];
    if (canvas_w == 0 or canvas_h == 0 or canvas_w > (1 << 24) or canvas_h > (1 << 24)) {
        return error.InvalidDimensions;
    }

    var flags: u8 = vp8x_flags | 0x04;
    if (has_alpha) flags |= 0x10;
    if (iccp_chunk != null) flags |= 0x20;
    if (exif_chunk != null) flags |= 0x08;

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    try out.ensureTotalCapacity(allocator, webp_bytes.len + xmp.len + 32);

    try out.appendSlice(allocator, "RIFF");
    try out.appendSlice(allocator, &[_]u8{ 0, 0, 0, 0 });
    try out.appendSlice(allocator, "WEBP");

    // VP8X header chunk
    try out.appendSlice(allocator, "VP8X");
    var le32: [4]u8 = undefined;
    mem.writeInt(u32, &le32, 10, .little);
    try out.appendSlice(allocator, &le32);
    try out.append(allocator, flags);
    try out.appendSlice(allocator, &[_]u8{ 0, 0, 0 });
    const w1 = canvas_w - 1;
    const h1 = canvas_h - 1;
    try out.appendSlice(allocator, &[_]u8{
        @intCast(w1 & 0xFF),
        @intCast((w1 >> 8) & 0xFF),
        @intCast((w1 >> 16) & 0xFF),
    });
    try out.appendSlice(allocator, &[_]u8{
        @intCast(h1 & 0xFF),
        @intCast((h1 >> 8) & 0xFF),
        @intCast((h1 >> 16) & 0xFF),
    });

    if (iccp_chunk) |iccp| try out.appendSlice(allocator, iccp);
    for (image_chunks.items) |chunk| try out.appendSlice(allocator, chunk);
    if (exif_chunk) |exif| try out.appendSlice(allocator, exif);

    try out.appendSlice(allocator, "XMP ");
    mem.writeInt(u32, &le32, @intCast(xmp.len), .little);
    try out.appendSlice(allocator, &le32);
    try out.appendSlice(allocator, xmp);
    if ((xmp.len & 1) == 1) try out.append(allocator, 0);

    const final_riff_size: u32 = @intCast(out.items.len - 8);
    mem.writeInt(u32, out.items[4..8], final_riff_size, .little);
    return out.toOwnedSlice(allocator);
}

// ============================================================================
// PNG Encoder & Decoder (Pure Zig, std.compress.flate)
// ============================================================================

const PNG_SIG = [_]u8{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

fn writePngChunk(out: *std.ArrayList(u8), allocator: Allocator, ctype: *const [4]u8, data: []const u8) Allocator.Error!void {
    var buf: [4]u8 = undefined;
    mem.writeInt(u32, &buf, @intCast(data.len), .big);
    try out.appendSlice(allocator, &buf);
    try out.appendSlice(allocator, ctype);
    try out.appendSlice(allocator, data);
    var crc = std.hash.Crc32.init();
    crc.update(ctype);
    crc.update(data);
    mem.writeInt(u32, &buf, crc.final(), .big);
    try out.appendSlice(allocator, &buf);
}

fn paethPredictor(a: u8, b: u8, c: u8) u8 {
    const ia: i32 = a;
    const ib: i32 = b;
    const ic: i32 = c;
    const p: i32 = ia + ib - ic;
    const pa: i32 = @intCast(@abs(p - ia));
    const pb: i32 = @intCast(@abs(p - ib));
    const pc: i32 = @intCast(@abs(p - ic));
    if (pa <= pb and pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/// Encode 8-bit pixels (1/2/3/4 ch) to PNG, optionally embedding `iTXt` `"XML:com.adobe.xmp"`.
pub fn encodePng(
    allocator: Allocator,
    pixels: []const u8,
    width: u32,
    height: u32,
    channels: u32,
    quality: i32,
    png_level: i32,
    xmp: []const u8,
) CodecError![]u8 {
    if (width == 0 or height == 0) return error.InvalidDimensions;
    const c: usize = channels;
    if (c < 1 or c > 4) return error.UnsupportedChannels;
    const expected = math.mul(usize, math.mul(usize, width, height) catch return error.InvalidDimensions, c) catch return error.InvalidDimensions;
    if (pixels.len < expected) return error.BufferTooSmall;

    const color_type: u8 = switch (c) {
        1 => 0,
        2 => 4,
        3 => 2,
        4 => 6,
        else => unreachable,
    };

    const level: i32 = if (png_level >= 0)
        @min(png_level, 9)
    else blk: {
        const q = math.clamp(quality, 1, 100);
        break :blk math.clamp(@divTrunc((100 - q) * 9 + 50, 100), 0, 9);
    };
    const comp_opts: flate.Compress.Options = switch (level) {
        0...1 => .fastest,
        8...9 => .best,
        else => .default,
    };

    const row_bytes = @as(usize, width) * c;
    const raw_len = (@as(usize, height)) * (1 + row_bytes);
    const raw = try allocator.alloc(u8, raw_len);
    defer allocator.free(raw);

    // Filter each scanline using adaptive min-abs-sum across None/Sub/Up/Avg/Paeth.
    const cand = try allocator.alloc(u8, row_bytes * 5);
    defer allocator.free(cand);

    for (0..height) |y| {
        const src = pixels[y * row_bytes .. (y + 1) * row_bytes];
        const dst = raw[y * (1 + row_bytes) .. (y + 1) * (1 + row_bytes)];

        const prev: ?[]const u8 = if (y > 0) pixels[(y - 1) * row_bytes .. y * row_bytes] else null;
        var sums: [5]usize = .{ 0, 0, 0, 0, 0 };

        for (0..row_bytes) |i| {
            const x = src[i];
            const a: u8 = if (i >= c) src[i - c] else 0;
            const b: u8 = if (prev) |pr| pr[i] else 0;
            const cp: u8 = if (prev != null and i >= c) prev.?[i - c] else 0;

            const f0 = x;
            const f1 = x -% a;
            const f2 = x -% b;
            const f3 = x -% @as(u8, @intCast((@as(u16, a) + @as(u16, b)) >> 1));
            const f4 = x -% paethPredictor(a, b, cp);

            cand[0 * row_bytes + i] = f0;
            cand[1 * row_bytes + i] = f1;
            cand[2 * row_bytes + i] = f2;
            cand[3 * row_bytes + i] = f3;
            cand[4 * row_bytes + i] = f4;

            sums[0] += @as(u8, @intCast(@abs(@as(i8, @bitCast(f0)))));
            sums[1] += @as(u8, @intCast(@abs(@as(i8, @bitCast(f1)))));
            sums[2] += @as(u8, @intCast(@abs(@as(i8, @bitCast(f2)))));
            sums[3] += @as(u8, @intCast(@abs(@as(i8, @bitCast(f3)))));
            sums[4] += @as(u8, @intCast(@abs(@as(i8, @bitCast(f4)))));
        }

        var best_f: usize = 0;
        for (1..5) |f| {
            if (sums[f] < sums[best_f]) best_f = f;
        }
        dst[0] = @intCast(best_f);
        @memcpy(dst[1 .. 1 + row_bytes], cand[best_f * row_bytes .. (best_f + 1) * row_bytes]);
    }

    // Zlib-compress filtered scanlines (heap-alloc Compress & window buffer so wasm32 stack stays tiny).
    var idat_aw: std.Io.Writer.Allocating = try .initCapacity(allocator, @max(4096, expected / 2));
    defer idat_aw.deinit();

    const comp_buf = try allocator.alloc(u8, flate.max_window_len);
    defer allocator.free(comp_buf);

    if (level == 0) {
        const raw_comp = try allocator.create(flate.Compress.Raw);
        defer allocator.destroy(raw_comp);
        raw_comp.* = flate.Compress.Raw.init(&idat_aw.writer, comp_buf, .zlib) catch return error.CompressionFailed;
        raw_comp.writer.writeAll(raw) catch return error.CompressionFailed;
        raw_comp.finish() catch return error.CompressionFailed;
    } else {
        const comp = try allocator.create(flate.Compress);
        defer allocator.destroy(comp);
        comp.* = flate.Compress.init(&idat_aw.writer, comp_buf, .zlib, comp_opts) catch return error.CompressionFailed;
        comp.writer.writeAll(raw) catch return error.CompressionFailed;
        comp.finish() catch return error.CompressionFailed;
    }
    const idat_bytes = idat_aw.written();

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    try out.ensureTotalCapacity(allocator, 64 + xmp.len + idat_bytes.len);

    try out.appendSlice(allocator, &PNG_SIG);

    var ihdr: [13]u8 = undefined;
    mem.writeInt(u32, ihdr[0..4], width, .big);
    mem.writeInt(u32, ihdr[4..8], height, .big);
    ihdr[8] = 8; // bit depth
    ihdr[9] = color_type;
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    try writePngChunk(&out, allocator, "IHDR", &ihdr);

    if (xmp.len > 0) {
        const kw = "XML:com.adobe.xmp";
        const itxt_len = kw.len + 1 + 1 + 1 + 1 + 1 + xmp.len;
        const itxt = try allocator.alloc(u8, itxt_len);
        defer allocator.free(itxt);
        var p: usize = 0;
        @memcpy(itxt[p .. p + kw.len], kw);
        p += kw.len;
        itxt[p] = 0; // NUL after keyword
        p += 1;
        itxt[p] = 0; // compression flag = 0
        p += 1;
        itxt[p] = 0; // compression method = 0
        p += 1;
        itxt[p] = 0; // language tag (empty)
        p += 1;
        itxt[p] = 0; // translated keyword (empty)
        p += 1;
        @memcpy(itxt[p .. p + xmp.len], xmp);
        try writePngChunk(&out, allocator, "iTXt", itxt);
    }

    try writePngChunk(&out, allocator, "IDAT", idat_bytes);
    try writePngChunk(&out, allocator, "IEND", &.{});

    return out.toOwnedSlice(allocator);
}

fn pngUnfilterScanline(row: []u8, prev: ?[]const u8, filter_type: u8, bpp: usize) CodecError!void {
    switch (filter_type) {
        0 => {},
        1 => {
            for (bpp..row.len) |i| row[i] +%= row[i - bpp];
        },
        2 => {
            if (prev) |pr| {
                for (0..row.len) |i| row[i] +%= pr[i];
            }
        },
        3 => {
            for (0..row.len) |i| {
                const a: u16 = if (i >= bpp) row[i - bpp] else 0;
                const b: u16 = if (prev) |pr| pr[i] else 0;
                row[i] +%= @intCast((a + b) >> 1);
            }
        },
        4 => {
            for (0..row.len) |i| {
                const a: u8 = if (i >= bpp) row[i - bpp] else 0;
                const b: u8 = if (prev) |pr| pr[i] else 0;
                const c: u8 = if (prev != null and i >= bpp) prev.?[i - bpp] else 0;
                row[i] +%= paethPredictor(a, b, c);
            }
        },
        else => return error.InvalidPng,
    }
}

fn pngDecodePass(
    raw: []const u8,
    raw_pos: *usize,
    pass_w: usize,
    pass_h: usize,
    color_type: u8,
    bit_depth: u8,
    palette: []const [3]u8,
    trns_alpha: []const u8,
    trns_gray: ?u16,
    trns_rgb: ?[3]u16,
    dst: []u8,
    dst_w: usize,
    dst_c: usize,
    x_origin: usize,
    y_origin: usize,
    x_step: usize,
    y_step: usize,
    allocator: Allocator,
) CodecError!void {
    if (pass_w == 0 or pass_h == 0) return;
    const raw_samples: usize = switch (color_type) {
        0, 3 => 1,
        2 => 3,
        4 => 2,
        6 => 4,
        else => return error.InvalidPng,
    };
    const bits_per_pixel = raw_samples * bit_depth;
    const row_bytes = (pass_w * bits_per_pixel + 7) / 8;
    const bpp = @max(1, (bits_per_pixel + 7) / 8);

    const prev_row = try allocator.alloc(u8, row_bytes);
    defer allocator.free(prev_row);
    const cur_row = try allocator.alloc(u8, row_bytes);
    defer allocator.free(cur_row);

    for (0..pass_h) |py| {
        if (raw_pos.* + 1 + row_bytes > raw.len) return error.InvalidPng;
        const ftype = raw[raw_pos.*];
        @memcpy(cur_row, raw[raw_pos.* + 1 .. raw_pos.* + 1 + row_bytes]);
        raw_pos.* += 1 + row_bytes;

        try pngUnfilterScanline(cur_row, if (py > 0) prev_row else null, ftype, bpp);
        @memcpy(prev_row, cur_row);

        const dy = y_origin + py * y_step;
        for (0..pass_w) |px| {
            const dx = x_origin + px * x_step;
            const out_off = (dy * dst_w + dx) * dst_c;

            switch (color_type) {
                0 => {
                    var g8: u8 = undefined;
                    var is_trans = false;
                    if (bit_depth == 16) {
                        const s16 = mem.readInt(u16, cur_row[px * 2 ..][0..2], .big);
                        g8 = @intCast(s16 >> 8);
                        if (trns_gray) |tg| is_trans = (s16 == tg);
                    } else if (bit_depth == 8) {
                        g8 = cur_row[px];
                        if (trns_gray) |tg| is_trans = (@as(u16, g8) == tg);
                    } else {
                        const ppb = 8 / bit_depth;
                        const shift: u3 = @intCast((ppb - 1 - (px % ppb)) * bit_depth);
                        const mask: u8 = (@as(u8, 1) << @intCast(bit_depth)) - 1;
                        const raw_v = (cur_row[px / ppb] >> shift) & mask;
                        if (trns_gray) |tg| is_trans = (@as(u16, raw_v) == tg);
                        g8 = switch (bit_depth) {
                            1 => raw_v * 255,
                            2 => raw_v * 85,
                            4 => raw_v * 17,
                            else => unreachable,
                        };
                    }
                    dst[out_off] = g8;
                    dst[out_off + 1] = g8;
                    dst[out_off + 2] = g8;
                    if (dst_c == 4) dst[out_off + 3] = if (is_trans) 0 else 255;
                },
                2 => {
                    if (bit_depth == 16) {
                        const r16 = mem.readInt(u16, cur_row[px * 6 ..][0..2], .big);
                        const g16 = mem.readInt(u16, cur_row[px * 6 + 2 ..][0..2], .big);
                        const b16 = mem.readInt(u16, cur_row[px * 6 + 4 ..][0..2], .big);
                        dst[out_off] = @intCast(r16 >> 8);
                        dst[out_off + 1] = @intCast(g16 >> 8);
                        dst[out_off + 2] = @intCast(b16 >> 8);
                        if (dst_c == 4) {
                            const is_trans = if (trns_rgb) |tr| (r16 == tr[0] and g16 == tr[1] and b16 == tr[2]) else false;
                            dst[out_off + 3] = if (is_trans) 0 else 255;
                        }
                    } else {
                        const r = cur_row[px * 3];
                        const g = cur_row[px * 3 + 1];
                        const b = cur_row[px * 3 + 2];
                        dst[out_off] = r;
                        dst[out_off + 1] = g;
                        dst[out_off + 2] = b;
                        if (dst_c == 4) {
                            const is_trans = if (trns_rgb) |tr| (r == tr[0] and g == tr[1] and b == tr[2]) else false;
                            dst[out_off + 3] = if (is_trans) 0 else 255;
                        }
                    }
                },
                3 => {
                    const idx: usize = if (bit_depth == 8) cur_row[px] else blk: {
                        const ppb = 8 / bit_depth;
                        const shift: u3 = @intCast((ppb - 1 - (px % ppb)) * bit_depth);
                        const mask: u8 = (@as(u8, 1) << @intCast(bit_depth)) - 1;
                        break :blk (cur_row[px / ppb] >> shift) & mask;
                    };
                    if (idx >= palette.len) return error.InvalidPng;
                    dst[out_off] = palette[idx][0];
                    dst[out_off + 1] = palette[idx][1];
                    dst[out_off + 2] = palette[idx][2];
                    if (dst_c == 4) {
                        dst[out_off + 3] = if (idx < trns_alpha.len) trns_alpha[idx] else 255;
                    }
                },
                4 => {
                    if (bit_depth == 16) {
                        const g8 = cur_row[px * 4];
                        const a8 = cur_row[px * 4 + 2];
                        dst[out_off] = g8;
                        dst[out_off + 1] = g8;
                        dst[out_off + 2] = g8;
                        dst[out_off + 3] = a8;
                    } else {
                        const g8 = cur_row[px * 2];
                        const a8 = cur_row[px * 2 + 1];
                        dst[out_off] = g8;
                        dst[out_off + 1] = g8;
                        dst[out_off + 2] = g8;
                        dst[out_off + 3] = a8;
                    }
                },
                6 => {
                    if (bit_depth == 16) {
                        dst[out_off] = cur_row[px * 8];
                        dst[out_off + 1] = cur_row[px * 8 + 2];
                        dst[out_off + 2] = cur_row[px * 8 + 4];
                        dst[out_off + 3] = cur_row[px * 8 + 6];
                    } else {
                        dst[out_off] = cur_row[px * 4];
                        dst[out_off + 1] = cur_row[px * 4 + 1];
                        dst[out_off + 2] = cur_row[px * 4 + 2];
                        dst[out_off + 3] = cur_row[px * 4 + 3];
                    }
                },
                else => return error.InvalidPng,
            }
        }
    }
}

/// Decode PNG bytes to 8-bit RGB (3ch) or RGBA (4ch).
/// Grayscale expands to RGB; Grayscale+Alpha expands to RGBA; 16-bit strips to 8-bit.
pub fn decodePng(allocator: Allocator, buffer: []const u8) CodecError!DecodedImage {
    if (buffer.len < 8 + 25 or !mem.eql(u8, buffer[0..8], &PNG_SIG)) {
        return error.InvalidPng;
    }

    var pos: usize = 8;
    var have_ihdr = false;
    var width: u32 = 0;
    var height: u32 = 0;
    var bit_depth: u8 = 0;
    var color_type: u8 = 0;
    var interlace: u8 = 0;

    var palette: [256][3]u8 = undefined;
    var palette_len: usize = 0;
    var trns_alpha: [256]u8 = undefined;
    var trns_alpha_len: usize = 0;
    var trns_gray: ?u16 = null;
    var trns_rgb: ?[3]u16 = null;

    var idat_data: std.ArrayList(u8) = .empty;
    defer idat_data.deinit(allocator);

    while (pos + 12 <= buffer.len) {
        const clen: usize = mem.readInt(u32, buffer[pos..][0..4], .big);
        const ctype = buffer[pos + 4 .. pos + 8];
        const data_start = pos + 8;
        const data_end = std.math.add(usize, data_start, clen) catch return error.InvalidPng;
        if (data_end + 4 > buffer.len) return error.InvalidPng;
        const cdata = buffer[data_start..data_end];
        pos = data_end + 4;

        if (mem.eql(u8, ctype, "IHDR")) {
            if (clen != 13 or have_ihdr) return error.InvalidPng;
            width = mem.readInt(u32, cdata[0..4], .big);
            height = mem.readInt(u32, cdata[4..8], .big);
            bit_depth = cdata[8];
            color_type = cdata[9];
            if (cdata[10] != 0 or cdata[11] != 0) return error.InvalidPng;
            interlace = cdata[12];
            if (width == 0 or height == 0 or interlace > 1) return error.InvalidPng;
            const valid_combo = switch (color_type) {
                0 => bit_depth == 1 or bit_depth == 2 or bit_depth == 4 or bit_depth == 8 or bit_depth == 16,
                2, 4, 6 => bit_depth == 8 or bit_depth == 16,
                3 => bit_depth == 1 or bit_depth == 2 or bit_depth == 4 or bit_depth == 8,
                else => false,
            };
            if (!valid_combo) return error.InvalidPng;
            have_ihdr = true;
        } else if (mem.eql(u8, ctype, "PLTE")) {
            if (clen == 0 or clen % 3 != 0 or clen / 3 > 256) return error.InvalidPng;
            palette_len = clen / 3;
            for (0..palette_len) |i| {
                palette[i] = .{ cdata[i * 3], cdata[i * 3 + 1], cdata[i * 3 + 2] };
            }
        } else if (mem.eql(u8, ctype, "tRNS")) {
            switch (color_type) {
                3 => {
                    trns_alpha_len = @min(clen, 256);
                    @memcpy(trns_alpha[0..trns_alpha_len], cdata[0..trns_alpha_len]);
                },
                0 => if (clen >= 2) {
                    trns_gray = mem.readInt(u16, cdata[0..2], .big);
                },
                2 => if (clen >= 6) {
                    trns_rgb = .{
                        mem.readInt(u16, cdata[0..2], .big),
                        mem.readInt(u16, cdata[2..4], .big),
                        mem.readInt(u16, cdata[4..6], .big),
                    };
                },
                else => {},
            }
        } else if (mem.eql(u8, ctype, "IDAT")) {
            try idat_data.appendSlice(allocator, cdata);
        } else if (mem.eql(u8, ctype, "IEND")) {
            break;
        }
    }

    if (!have_ihdr or idat_data.items.len == 0) return error.InvalidPng;
    if (color_type == 3 and palette_len == 0) return error.InvalidPng;

    var in_reader: std.Io.Reader = .fixed(idat_data.items);
    var raw_aw: std.Io.Writer.Allocating = .init(allocator);
    defer raw_aw.deinit();

    var decomp: flate.Decompress = .init(&in_reader, .zlib, &.{});
    _ = decomp.reader.streamRemaining(&raw_aw.writer) catch return error.DecompressionFailed;
    const raw = raw_aw.written();

    const has_trns_alpha = (trns_alpha_len > 0) or (trns_gray != null) or (trns_rgb != null);
    const out_channels: u32 = if (color_type == 4 or color_type == 6 or has_trns_alpha) 4 else 3;
    const total_bytes = math.mul(usize, math.mul(usize, width, height) catch return error.InvalidDimensions, out_channels) catch return error.InvalidDimensions;
    const out_data = try allocator.alloc(u8, total_bytes);
    errdefer allocator.free(out_data);

    var raw_pos: usize = 0;
    if (interlace == 0) {
        try pngDecodePass(
            raw,
            &raw_pos,
            width,
            height,
            color_type,
            bit_depth,
            palette[0..palette_len],
            trns_alpha[0..trns_alpha_len],
            trns_gray,
            trns_rgb,
            out_data,
            width,
            out_channels,
            0,
            0,
            1,
            1,
            allocator,
        );
    } else {
        const adam7_x0 = [7]usize{ 0, 4, 0, 2, 0, 1, 0 };
        const adam7_y0 = [7]usize{ 0, 0, 4, 0, 2, 0, 1 };
        const adam7_xs = [7]usize{ 8, 8, 4, 4, 2, 2, 1 };
        const adam7_ys = [7]usize{ 8, 8, 8, 4, 4, 2, 2 };
        for (0..7) |pass| {
            const x0 = adam7_x0[pass];
            const y0 = adam7_y0[pass];
            const xs = adam7_xs[pass];
            const ys = adam7_ys[pass];
            const pw = if (width > x0) (width - x0 + xs - 1) / xs else 0;
            const ph = if (height > y0) (height - y0 + ys - 1) / ys else 0;
            try pngDecodePass(
                raw,
                &raw_pos,
                pw,
                ph,
                color_type,
                bit_depth,
                palette[0..palette_len],
                trns_alpha[0..trns_alpha_len],
                trns_gray,
                trns_rgb,
                out_data,
                width,
                out_channels,
                x0,
                y0,
                xs,
                ys,
                allocator,
            );
        }
    }

    return DecodedImage{
        .width = width,
        .height = height,
        .channels = out_channels,
        .data = out_data,
    };
}

// ============================================================================
// JPEG Baseline Encoder & Baseline/Progressive Decoder (Pure Zig)
// ============================================================================

const ZIGZAG = [64]u8{
    0,  1,  8,  16, 9,  2,  3,  10,
    17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// Standard IJG / ITU-T T.81 Annex K quantization tables (natural order)
const STD_LUMINANCE_QUANT = [64]u8{
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};

const STD_CHROMINANCE_QUANT = [64]u8{
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

// ITU-T T.81 Annex K.3 Standard Huffman tables
const DC_LUMA_BITS = [16]u8{ 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
const DC_LUMA_VALS = [_]u8{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

const DC_CHROMA_BITS = [16]u8{ 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
const DC_CHROMA_VALS = [_]u8{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

const AC_LUMA_BITS = [16]u8{ 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
const AC_LUMA_VALS = [_]u8{
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

const AC_CHROMA_BITS = [16]u8{ 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
const AC_CHROMA_VALS = [_]u8{
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

const DCT_COS: [8][8]f32 = blk: {
    @setEvalBranchQuota(10000);
    var table: [8][8]f32 = undefined;
    const inv_sqrt2: f64 = 1.0 / @sqrt(2.0);
    for (0..8) |k| {
        const ck: f64 = if (k == 0) inv_sqrt2 else 1.0;
        for (0..8) |x| {
            const angle = (@as(f64, @floatFromInt(2 * x + 1)) * @as(f64, @floatFromInt(k)) * math.pi) / 16.0;
            table[k][x] = @floatCast(0.5 * ck * @cos(angle));
        }
    }
    break :blk table;
};

fn fdct8x8(block: *const [64]f32, out: *[64]f32) void {
    var tmp: [64]f32 = undefined;
    for (0..8) |y| {
        for (0..8) |u| {
            var sum: f32 = 0;
            for (0..8) |x| {
                sum += block[y * 8 + x] * DCT_COS[u][x];
            }
            tmp[y * 8 + u] = sum;
        }
    }
    for (0..8) |v| {
        for (0..8) |u| {
            var sum: f32 = 0;
            for (0..8) |y| {
                sum += tmp[y * 8 + u] * DCT_COS[v][y];
            }
            out[v * 8 + u] = sum;
        }
    }
}

fn idct8x8(coeffs: *const [64]f32, out: *[64]u8) void {
    var tmp: [64]f32 = undefined;
    for (0..8) |y| {
        for (0..8) |u| {
            var sum: f32 = 0;
            for (0..8) |v| {
                sum += coeffs[v * 8 + u] * DCT_COS[v][y];
            }
            tmp[y * 8 + u] = sum;
        }
    }
    for (0..8) |y| {
        for (0..8) |x| {
            var sum: f32 = 128.5;
            for (0..8) |u| {
                sum += tmp[y * 8 + u] * DCT_COS[u][x];
            }
            const iv: i32 = @intFromFloat(@floor(sum));
            out[y * 8 + x] = @intCast(math.clamp(iv, 0, 255));
        }
    }
}

const HuffEncTable = struct {
    codes: [256]u16,
    sizes: [256]u8,

    fn build(bits: *const [16]u8, vals: []const u8) HuffEncTable {
        var t = HuffEncTable{
            .codes = @splat(0),
            .sizes = @splat(0),
        };
        var code: u16 = 0;
        var idx: usize = 0;
        for (0..16) |b| {
            const count = bits[b];
            for (0..count) |_| {
                const sym = vals[idx];
                t.codes[sym] = code;
                t.sizes[sym] = @intCast(b + 1);
                code += 1;
                idx += 1;
            }
            code <<= 1;
        }
        return t;
    }
};

const DC_LUMA_ENC = HuffEncTable.build(&DC_LUMA_BITS, &DC_LUMA_VALS);
const DC_CHROMA_ENC = HuffEncTable.build(&DC_CHROMA_BITS, &DC_CHROMA_VALS);
const AC_LUMA_ENC = HuffEncTable.build(&AC_LUMA_BITS, &AC_LUMA_VALS);
const AC_CHROMA_ENC = HuffEncTable.build(&AC_CHROMA_BITS, &AC_CHROMA_VALS);

const JpegBitWriter = struct {
    out: *std.ArrayList(u8),
    allocator: Allocator,
    buf: u32 = 0,
    bits: u5 = 0,

    fn putBits(self: *JpegBitWriter, code: u32, n: u5) Allocator.Error!void {
        if (n == 0) return;
        self.buf = (self.buf << n) | (code & ((@as(u32, 1) << n) - 1));
        self.bits += n;
        while (self.bits >= 8) {
            self.bits -= 8;
            const byte: u8 = @intCast((self.buf >> self.bits) & 0xFF);
            try self.out.append(self.allocator, byte);
            if (byte == 0xFF) {
                try self.out.append(self.allocator, 0x00);
            }
        }
    }

    fn flush(self: *JpegBitWriter) Allocator.Error!void {
        if (self.bits > 0) {
            const pad: u5 = 8 - self.bits;
            const byte: u8 = @intCast(((self.buf << pad) | ((@as(u32, 1) << pad) - 1)) & 0xFF);
            try self.out.append(self.allocator, byte);
            if (byte == 0xFF) {
                try self.out.append(self.allocator, 0x00);
            }
            self.buf = 0;
            self.bits = 0;
        }
    }
};

fn bitLength(v: i32) u5 {
    var abs_v: u32 = @intCast(@abs(v));
    var n: u5 = 0;
    while (abs_v > 0) : (abs_v >>= 1) n += 1;
    return n;
}

fn encodeDctBlock(
    bw: *JpegBitWriter,
    block: *const [64]f32,
    quant: *const [64]u8,
    prev_dc: *i32,
    dc_tbl: *const HuffEncTable,
    ac_tbl: *const HuffEncTable,
) Allocator.Error!void {
    var dct: [64]f32 = undefined;
    fdct8x8(block, &dct);

    var zz: [64]i32 = undefined;
    for (0..64) |i| {
        const idx = ZIGZAG[i];
        const q: f32 = @floatFromInt(quant[idx]);
        zz[i] = @intFromFloat(@round(dct[idx] / q));
    }

    // DC coefficient
    const dc_diff = zz[0] - prev_dc.*;
    prev_dc.* = zz[0];
    if (dc_diff == 0) {
        try bw.putBits(dc_tbl.codes[0], @intCast(dc_tbl.sizes[0]));
    } else {
        const ssss = bitLength(dc_diff);
        try bw.putBits(dc_tbl.codes[ssss], @intCast(dc_tbl.sizes[ssss]));
        const bits: u32 = if (dc_diff > 0) @intCast(dc_diff) else @intCast(dc_diff + (@as(i32, 1) << ssss) - 1);
        try bw.putBits(bits, ssss);
    }

    // AC coefficients
    var last_nz: usize = 0;
    for (1..64) |i| {
        if (zz[i] != 0) last_nz = i;
    }
    var i: usize = 1;
    while (i <= last_nz) : (i += 1) {
        var zero_run: usize = 0;
        while (zz[i] == 0) : (i += 1) {
            zero_run += 1;
        }
        while (zero_run >= 16) {
            try bw.putBits(ac_tbl.codes[0xF0], @intCast(ac_tbl.sizes[0xF0]));
            zero_run -= 16;
        }
        const val = zz[i];
        const ssss = bitLength(val);
        const sym: u8 = @intCast((zero_run << 4) | ssss);
        try bw.putBits(ac_tbl.codes[sym], @intCast(ac_tbl.sizes[sym]));
        const bits: u32 = if (val > 0) @intCast(val) else @intCast(val + (@as(i32, 1) << ssss) - 1);
        try bw.putBits(bits, ssss);
    }
    if (last_nz < 63) {
        try bw.putBits(ac_tbl.codes[0x00], @intCast(ac_tbl.sizes[0x00]));
    }
}

fn buildScaledQuant(base: *const [64]u8, quality: i32) [64]u8 {
    const q = math.clamp(quality, 1, 100);
    const scale: i32 = if (q < 50) @divTrunc(5000, q) else 200 - q * 2;
    var out: [64]u8 = undefined;
    for (0..64) |i| {
        const v = @divTrunc(@as(i32, base[i]) * scale + 50, 100);
        out[i] = @intCast(math.clamp(v, 1, 255));
    }
    return out;
}

fn writeJpegDht(out: *std.ArrayList(u8), allocator: Allocator, class_id: u8, bits: *const [16]u8, vals: []const u8) Allocator.Error!void {
    const len: u16 = @intCast(2 + 1 + 16 + vals.len);
    try out.appendSlice(allocator, &[_]u8{ 0xFF, 0xC4, @intCast(len >> 8), @intCast(len & 0xFF), class_id });
    try out.appendSlice(allocator, bits);
    try out.appendSlice(allocator, vals);
}

fn writeJpegDqt(out: *std.ArrayList(u8), allocator: Allocator, id: u8, table: *const [64]u8) Allocator.Error!void {
    try out.appendSlice(allocator, &[_]u8{ 0xFF, 0xDB, 0x00, 67, id });
    var zz: [64]u8 = undefined;
    for (0..64) |i| zz[i] = table[ZIGZAG[i]];
    try out.appendSlice(allocator, &zz);
}

/// Encode 8-bit pixels (1/3/4 ch; 4ch premultiplied over black -> RGB) to baseline JPEG.
/// `subsampling`: 444 (1x1), 422 (2x1), or 420 (2x2, default).
pub fn encodeJpeg(
    allocator: Allocator,
    pixels: []const u8,
    width: u32,
    height: u32,
    channels: u32,
    quality: i32,
    subsampling: i32,
    xmp: []const u8,
) CodecError![]u8 {
    if (width == 0 or height == 0 or width > 65535 or height > 65535) {
        return error.InvalidDimensions;
    }
    if (channels != 1 and channels != 3 and channels != 4) {
        return error.UnsupportedChannels;
    }
    const w: usize = width;
    const h: usize = height;
    const expected = w * h * channels;
    if (pixels.len < expected) return error.BufferTooSmall;

    const eff_channels: usize = if (channels == 4) 3 else channels;
    const rgb: []const u8 = if (channels == 4) blk: {
        const tmp = try allocator.alloc(u8, w * h * 3);
        for (0..w * h) |i| {
            const a: u32 = pixels[i * 4 + 3];
            tmp[i * 3] = @intCast((@as(u32, pixels[i * 4]) * a + 127) / 255);
            tmp[i * 3 + 1] = @intCast((@as(u32, pixels[i * 4 + 1]) * a + 127) / 255);
            tmp[i * 3 + 2] = @intCast((@as(u32, pixels[i * 4 + 2]) * a + 127) / 255);
        }
        break :blk tmp;
    } else pixels[0 .. w * h * eff_channels];
    defer if (channels == 4) allocator.free(rgb);

    const h_samp: usize = if (eff_channels == 1 or subsampling == 444) 1 else 2;
    const v_samp: usize = if (eff_channels == 1 or subsampling == 444 or subsampling == 422) 1 else 2;

    const luma_q = buildScaledQuant(&STD_LUMINANCE_QUANT, quality);
    const chroma_q = buildScaledQuant(&STD_CHROMINANCE_QUANT, quality);

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    try out.ensureTotalCapacity(allocator, @max(4096, expected / 4) + xmp.len);

    // SOI
    try out.appendSlice(allocator, &[_]u8{ 0xFF, 0xD8 });

    // APP0 JFIF
    try out.appendSlice(allocator, &[_]u8{
        0xFF, 0xE0, 0x00, 16, 'J', 'F', 'I', 'F', 0x00, 1, 1, 0, 0, 1, 0, 1, 0, 0,
    });

    // APP1 XMP
    if (xmp.len > 0) {
        const payload_len = JPEG_XMP_NAMESPACE.len + xmp.len;
        if (payload_len <= 65533) {
            const seg_len: u16 = @intCast(payload_len + 2);
            try out.appendSlice(allocator, &[_]u8{ 0xFF, 0xE1, @intCast(seg_len >> 8), @intCast(seg_len & 0xFF) });
            try out.appendSlice(allocator, JPEG_XMP_NAMESPACE);
            try out.appendSlice(allocator, xmp);
        }
    }

    // DQT
    try writeJpegDqt(&out, allocator, 0, &luma_q);
    if (eff_channels == 3) {
        try writeJpegDqt(&out, allocator, 1, &chroma_q);
    }

    // SOF0
    const sof_len: u16 = @intCast(8 + eff_channels * 3);
    try out.appendSlice(allocator, &[_]u8{
        0xFF,
        0xC0,
        @intCast(sof_len >> 8),
        @intCast(sof_len & 0xFF),
        8,
        @intCast(h >> 8),
        @intCast(h & 0xFF),
        @intCast(w >> 8),
        @intCast(w & 0xFF),
        @intCast(eff_channels),
    });
    try out.appendSlice(allocator, &[_]u8{ 1, @intCast((h_samp << 4) | v_samp), 0 });
    if (eff_channels == 3) {
        try out.appendSlice(allocator, &[_]u8{ 2, 0x11, 1 });
        try out.appendSlice(allocator, &[_]u8{ 3, 0x11, 1 });
    }

    // DHT
    try writeJpegDht(&out, allocator, 0x00, &DC_LUMA_BITS, &DC_LUMA_VALS);
    try writeJpegDht(&out, allocator, 0x10, &AC_LUMA_BITS, &AC_LUMA_VALS);
    if (eff_channels == 3) {
        try writeJpegDht(&out, allocator, 0x01, &DC_CHROMA_BITS, &DC_CHROMA_VALS);
        try writeJpegDht(&out, allocator, 0x11, &AC_CHROMA_BITS, &AC_CHROMA_VALS);
    }

    // SOS
    const sos_len: u16 = @intCast(6 + eff_channels * 2);
    try out.appendSlice(allocator, &[_]u8{
        0xFF,
        0xDA,
        @intCast(sos_len >> 8),
        @intCast(sos_len & 0xFF),
        @intCast(eff_channels),
        1,
        0x00,
    });
    if (eff_channels == 3) {
        try out.appendSlice(allocator, &[_]u8{ 2, 0x11, 3, 0x11 });
    }
    try out.appendSlice(allocator, &[_]u8{ 0, 63, 0 });

    // Encode MCUs
    var bw = JpegBitWriter{ .out = &out, .allocator = allocator };
    const mcu_w = h_samp * 8;
    const mcu_h = v_samp * 8;
    const mcu_cols = (w + mcu_w - 1) / mcu_w;
    const mcu_rows = (h + mcu_h - 1) / mcu_h;

    var prev_dc: [3]i32 = .{ 0, 0, 0 };
    var y_block: [64]f32 = undefined;
    var cb_block: [64]f32 = undefined;
    var cr_block: [64]f32 = undefined;

    for (0..mcu_rows) |my| {
        for (0..mcu_cols) |mx| {
            const x0 = mx * mcu_w;
            const y0 = my * mcu_h;

            if (eff_channels == 1) {
                for (0..8) |by| {
                    const sy = @min(y0 + by, h - 1);
                    for (0..8) |bx| {
                        const sx = @min(x0 + bx, w - 1);
                        y_block[by * 8 + bx] = @as(f32, @floatFromInt(rgb[sy * w + sx])) - 128.0;
                    }
                }
                try encodeDctBlock(&bw, &y_block, &luma_q, &prev_dc[0], &DC_LUMA_ENC, &AC_LUMA_ENC);
            } else {
                // Y blocks
                for (0..v_samp) |vy| {
                    for (0..h_samp) |hx| {
                        const bx0 = x0 + hx * 8;
                        const by0 = y0 + vy * 8;
                        for (0..8) |by| {
                            const sy = @min(by0 + by, h - 1);
                            for (0..8) |bx| {
                                const sx = @min(bx0 + bx, w - 1);
                                const idx = (sy * w + sx) * 3;
                                const r: f32 = @floatFromInt(rgb[idx]);
                                const g: f32 = @floatFromInt(rgb[idx + 1]);
                                const b: f32 = @floatFromInt(rgb[idx + 2]);
                                y_block[by * 8 + bx] = (0.299 * r + 0.587 * g + 0.114 * b) - 128.0;
                            }
                        }
                        try encodeDctBlock(&bw, &y_block, &luma_q, &prev_dc[0], &DC_LUMA_ENC, &AC_LUMA_ENC);
                    }
                }

                // Cb & Cr downsampled blocks (box filter)
                for (0..8) |cy| {
                    for (0..8) |cx| {
                        var sum_cb: f32 = 0;
                        var sum_cr: f32 = 0;
                        for (0..v_samp) |dy| {
                            const sy = @min(y0 + cy * v_samp + dy, h - 1);
                            for (0..h_samp) |dx| {
                                const sx = @min(x0 + cx * h_samp + dx, w - 1);
                                const idx = (sy * w + sx) * 3;
                                const r: f32 = @floatFromInt(rgb[idx]);
                                const g: f32 = @floatFromInt(rgb[idx + 1]);
                                const b: f32 = @floatFromInt(rgb[idx + 2]);
                                sum_cb += -0.168736 * r - 0.331264 * g + 0.5 * b;
                                sum_cr += 0.5 * r - 0.418688 * g - 0.081312 * b;
                            }
                        }
                        const denom: f32 = @floatFromInt(h_samp * v_samp);
                        cb_block[cy * 8 + cx] = sum_cb / denom;
                        cr_block[cy * 8 + cx] = sum_cr / denom;
                    }
                }
                try encodeDctBlock(&bw, &cb_block, &chroma_q, &prev_dc[1], &DC_CHROMA_ENC, &AC_CHROMA_ENC);
                try encodeDctBlock(&bw, &cr_block, &chroma_q, &prev_dc[2], &DC_CHROMA_ENC, &AC_CHROMA_ENC);
            }
        }
    }

    try bw.flush();
    // EOI
    try out.appendSlice(allocator, &[_]u8{ 0xFF, 0xD9 });
    return out.toOwnedSlice(allocator);
}

// ---------------- JPEG Decoder (Baseline SOF0/SOF1 & Progressive SOF2) ----------------

const HuffDecTable = struct {
    defined: bool = false,
    max_code: [17]i32 = @splat(-1),
    val_offset: [17]i32 = @splat(0),
    vals: [256]u8 = @splat(0),
    fast_sym: [256]u8 = @splat(0),
    fast_bits: [256]u8 = @splat(0),

    fn build(bits: *const [16]u8, vals_slice: []const u8) HuffDecTable {
        var t = HuffDecTable{ .defined = true };
        const n = @min(vals_slice.len, 256);
        @memcpy(t.vals[0..n], vals_slice[0..n]);

        var code: i32 = 0;
        var k: i32 = 0;
        for (1..17) |len| {
            const cnt = bits[len - 1];
            if (cnt == 0) {
                t.max_code[len] = -1;
            } else {
                t.val_offset[len] = k - code;
                for (0..cnt) |_| {
                    if (len <= 8) {
                        const shift: u3 = @intCast(8 - len);
                        const base: usize = @intCast(code << shift);
                        const span: usize = @as(usize, 1) << shift;
                        for (0..span) |j| {
                            t.fast_sym[base + j] = t.vals[@intCast(k)];
                            t.fast_bits[base + j] = @intCast(len);
                        }
                    }
                    code += 1;
                    k += 1;
                }
                t.max_code[len] = code - 1;
            }
            code <<= 1;
        }
        return t;
    }
};

const JpegBitReader = struct {
    data: []const u8,
    pos: usize,
    buf: u32 = 0,
    bits: u6 = 0,
    marker: u8 = 0,

    fn fill(self: *JpegBitReader, need: u6) void {
        while (self.bits < need) {
            if (self.marker != 0 or self.pos >= self.data.len) {
                self.buf = (self.buf << 8);
                self.bits += 8;
                continue;
            }
            const b = self.data[self.pos];
            if (b == 0xFF) {
                if (self.pos + 1 < self.data.len) {
                    const next = self.data[self.pos + 1];
                    if (next == 0x00) {
                        self.pos += 2;
                        self.buf = (self.buf << 8) | 0xFF;
                        self.bits += 8;
                    } else if (next == 0xFF) {
                        self.pos += 1;
                    } else {
                        self.marker = next;
                        self.pos += 2;
                        self.buf = (self.buf << 8);
                        self.bits += 8;
                    }
                } else {
                    self.pos += 1;
                    self.buf = (self.buf << 8);
                    self.bits += 8;
                }
            } else {
                self.pos += 1;
                self.buf = (self.buf << 8) | b;
                self.bits += 8;
            }
        }
    }

    fn readBits(self: *JpegBitReader, n: u5) u32 {
        if (n == 0) return 0;
        self.fill(n);
        self.bits -= n;
        const shift: u5 = @intCast(self.bits);
        return (self.buf >> shift) & ((@as(u32, 1) << n) - 1);
    }

    fn readSigned(self: *JpegBitReader, n: u5) i32 {
        if (n == 0) return 0;
        const v: i32 = @intCast(self.readBits(n));
        const half: i32 = @as(i32, 1) << (n - 1);
        return if (v < half) v - ((@as(i32, 1) << n) - 1) else v;
    }

    fn decodeHuff(self: *JpegBitReader, tbl: *const HuffDecTable) CodecError!u8 {
        if (!tbl.defined) return error.InvalidJpeg;
        self.fill(16);
        const top8: usize = @intCast((self.buf >> @intCast(self.bits - 8)) & 0xFF);
        const fb = tbl.fast_bits[top8];
        if (fb != 0) {
            self.bits -= @intCast(fb);
            return tbl.fast_sym[top8];
        }
        var code: i32 = @intCast(top8);
        var len: usize = 8;
        self.bits -= 8;
        while (len < 16) {
            len += 1;
            self.fill(1);
            self.bits -= 1;
            const bit: i32 = @intCast((self.buf >> @intCast(self.bits)) & 1);
            code = (code << 1) | bit;
            if (code <= tbl.max_code[len]) {
                const idx: usize = @intCast(code + tbl.val_offset[len]);
                return tbl.vals[idx];
            }
        }
        return error.InvalidJpeg;
    }

    fn resetForRestart(self: *JpegBitReader) void {
        if (self.marker == 0) {
            // Skip any stuffed padding to reach next marker if aligned
            while (self.pos + 1 < self.data.len) {
                if (self.data[self.pos] == 0xFF and self.data[self.pos + 1] != 0x00 and self.data[self.pos + 1] != 0xFF) {
                    self.marker = self.data[self.pos + 1];
                    self.pos += 2;
                    break;
                }
                self.pos += 1;
            }
        }
        self.buf = 0;
        self.bits = 0;
        self.marker = 0;
    }
};

const JpegComp = struct {
    id: u8 = 0,
    h_samp: u8 = 1,
    v_samp: u8 = 1,
    q_id: u8 = 0,
    dc_tbl: u8 = 0,
    ac_tbl: u8 = 0,
    dc_pred: i32 = 0,
    bw: usize = 0,
    bh: usize = 0,
    coeffs: []i16 = &.{},
};

/// Decode JPEG bytes to 8-bit RGB (3ch) with EXIF orientation auto-rotate baked in.
pub fn decodeJpeg(allocator: Allocator, buffer: []const u8) CodecError!DecodedImage {
    if (buffer.len < 4 or buffer[0] != 0xFF or buffer[1] != 0xD8) {
        return error.InvalidJpeg;
    }

    var quants: [4][64]u16 = undefined;
    var quant_defined: [4]bool = .{ false, false, false, false };
    var dc_huff: [4]HuffDecTable = .{ .{}, .{}, .{}, .{} };
    var ac_huff: [4]HuffDecTable = .{ .{}, .{}, .{}, .{} };

    var width: u32 = 0;
    var height: u32 = 0;
    var num_comp: usize = 0;
    var progressive = false;
    var have_sof = false;
    var restart_interval: u16 = 0;
    var comps: [4]JpegComp = .{ .{}, .{}, .{}, .{} };
    defer for (0..4) |i| {
        if (comps[i].coeffs.len > 0) allocator.free(comps[i].coeffs);
    };

    var max_h: usize = 1;
    var max_v: usize = 1;
    var pos: usize = 2;

    while (pos < buffer.len) {
        if (buffer[pos] != 0xFF) {
            pos += 1;
            continue;
        }
        while (pos < buffer.len and buffer[pos] == 0xFF) : (pos += 1) {}
        if (pos >= buffer.len) break;
        const marker = buffer[pos];
        pos += 1;

        if (marker == 0x00 or marker == 0x01 or (marker >= 0xD0 and marker <= 0xD8)) continue;
        if (marker == 0xD9) break;
        if (pos + 2 > buffer.len) return error.InvalidJpeg;
        const seg_len: usize = mem.readInt(u16, buffer[pos..][0..2], .big);
        if (seg_len < 2 or pos + seg_len > buffer.len) return error.InvalidJpeg;
        const seg = buffer[pos + 2 .. pos + seg_len];
        pos += seg_len;

        if (marker == 0xDB) {
            // DQT
            var p: usize = 0;
            while (p < seg.len) {
                const pq_tq = seg[p];
                const prec = pq_tq >> 4;
                const tq = pq_tq & 0x0F;
                if (tq >= 4) return error.InvalidJpeg;
                p += 1;
                if (prec == 0) {
                    if (p + 64 > seg.len) return error.InvalidJpeg;
                    for (0..64) |i| quants[tq][ZIGZAG[i]] = seg[p + i];
                    p += 64;
                } else {
                    if (p + 128 > seg.len) return error.InvalidJpeg;
                    for (0..64) |i| quants[tq][ZIGZAG[i]] = mem.readInt(u16, seg[p + i * 2 ..][0..2], .big);
                    p += 128;
                }
                quant_defined[tq] = true;
            }
        } else if (marker == 0xC4) {
            // DHT
            var p: usize = 0;
            while (p < seg.len) {
                if (p + 17 > seg.len) return error.InvalidJpeg;
                const tc_th = seg[p];
                const tc = tc_th >> 4;
                const th = tc_th & 0x0F;
                if (th >= 4 or tc > 1) return error.InvalidJpeg;
                const bits = seg[p + 1 ..][0..16];
                p += 17;
                var total: usize = 0;
                for (bits) |b| total += b;
                if (p + total > seg.len or total > 256) return error.InvalidJpeg;
                const vals = seg[p .. p + total];
                p += total;
                if (tc == 0) {
                    dc_huff[th] = HuffDecTable.build(bits, vals);
                } else {
                    ac_huff[th] = HuffDecTable.build(bits, vals);
                }
            }
        } else if (marker == 0xDD) {
            // DRI
            if (seg.len >= 2) restart_interval = mem.readInt(u16, seg[0..2], .big);
        } else if (marker == 0xC0 or marker == 0xC1 or marker == 0xC2) {
            // SOF0 / SOF1 / SOF2
            if (have_sof or seg.len < 6) return error.InvalidJpeg;
            progressive = (marker == 0xC2);
            if (seg[0] != 8) return error.InvalidJpeg;
            height = mem.readInt(u16, seg[1..3], .big);
            width = mem.readInt(u16, seg[3..5], .big);
            num_comp = seg[5];
            if (width == 0 or height == 0 or (num_comp != 1 and num_comp != 3)) return error.InvalidJpeg;
            if (seg.len < 6 + num_comp * 3) return error.InvalidJpeg;

            max_h = 1;
            max_v = 1;
            for (0..num_comp) |ci| {
                const hv = seg[6 + ci * 3 + 1];
                const hs = hv >> 4;
                const vs = hv & 0x0F;
                if (hs == 0 or vs == 0 or hs > 4 or vs > 4) return error.InvalidJpeg;
                comps[ci] = .{
                    .id = seg[6 + ci * 3],
                    .h_samp = hs,
                    .v_samp = vs,
                    .q_id = seg[6 + ci * 3 + 2],
                };
                if (comps[ci].q_id >= 4) return error.InvalidJpeg;
                max_h = @max(max_h, hs);
                max_v = @max(max_v, vs);
            }

            const mcu_w = max_h * 8;
            const mcu_h = max_v * 8;
            const mcu_cols = (width + mcu_w - 1) / mcu_w;
            const mcu_rows = (height + mcu_h - 1) / mcu_h;
            for (0..num_comp) |ci| {
                comps[ci].bw = mcu_cols * comps[ci].h_samp;
                comps[ci].bh = mcu_rows * comps[ci].v_samp;
                const n_blocks = comps[ci].bw * comps[ci].bh;
                comps[ci].coeffs = try allocator.alloc(i16, n_blocks * 64);
                @memset(comps[ci].coeffs, 0);
            }
            have_sof = true;
        } else if (marker == 0xDA) {
            // SOS
            if (!have_sof or seg.len < 1) return error.InvalidJpeg;
            const ns: usize = seg[0];
            if (ns == 0 or ns > num_comp or seg.len < 1 + ns * 2 + 3) return error.InvalidJpeg;

            var scan_comps: [4]usize = undefined;
            for (0..ns) |si| {
                const cid = seg[1 + si * 2];
                const td_ta = seg[2 + si * 2];
                const ci = for (0..num_comp) |k| {
                    if (comps[k].id == cid) break k;
                } else return error.InvalidJpeg;
                comps[ci].dc_tbl = td_ta >> 4;
                comps[ci].ac_tbl = td_ta & 0x0F;
                if (comps[ci].dc_tbl >= 4 or comps[ci].ac_tbl >= 4) return error.InvalidJpeg;
                scan_comps[si] = ci;
            }
            const ss: usize = seg[1 + ns * 2];
            const se: usize = seg[2 + ns * 2];
            const ah: u5 = @intCast(seg[3 + ns * 2] >> 4);
            const al: u5 = @intCast(seg[3 + ns * 2] & 0x0F);
            if (ss > se or se >= 64) return error.InvalidJpeg;

            var br = JpegBitReader{ .data = buffer, .pos = pos };
            var mcu_count: usize = 0;
            var eob_run: usize = 0;
            for (0..num_comp) |ci| comps[ci].dc_pred = 0;

            if (!progressive or ns > 1) {
                const mcu_w = max_h * 8;
                const mcu_h = max_v * 8;
                const mcu_cols = (width + mcu_w - 1) / mcu_w;
                const mcu_rows = (height + mcu_h - 1) / mcu_h;

                for (0..mcu_rows) |my| {
                    for (0..mcu_cols) |mx| {
                        if (restart_interval > 0 and mcu_count > 0 and (mcu_count % restart_interval) == 0) {
                            br.resetForRestart();
                            for (0..num_comp) |ci| comps[ci].dc_pred = 0;
                            eob_run = 0;
                        }
                        mcu_count += 1;

                        for (0..ns) |si| {
                            const ci = scan_comps[si];
                            const comp = &comps[ci];
                            const hs: usize = if (ns == 1) 1 else comp.h_samp;
                            const vs: usize = if (ns == 1) 1 else comp.v_samp;
                            for (0..vs) |vy| {
                                for (0..hs) |hx| {
                                    const bx = mx * comp.h_samp + hx;
                                    const by = my * comp.v_samp + vy;
                                    const blk = comp.coeffs[(by * comp.bw + bx) * 64 ..][0..64];
                                    try decodeScanBlock(&br, blk, &comp.dc_pred, &dc_huff[comp.dc_tbl], &ac_huff[comp.ac_tbl], ss, se, ah, al, &eob_run);
                                }
                            }
                        }
                    }
                }
            } else {
                // Non-interleaved single-component progressive scan (block row by col)
                const ci = scan_comps[0];
                const comp = &comps[ci];
                const bw_px = (width * comp.h_samp + max_h - 1) / max_h;
                const bh_px = (height * comp.v_samp + max_v - 1) / max_v;
                const scan_bw = (bw_px + 7) / 8;
                const scan_bh = (bh_px + 7) / 8;

                for (0..scan_bh) |by| {
                    for (0..scan_bw) |bx| {
                        if (restart_interval > 0 and mcu_count > 0 and (mcu_count % restart_interval) == 0) {
                            br.resetForRestart();
                            comp.dc_pred = 0;
                            eob_run = 0;
                        }
                        mcu_count += 1;
                        const blk = comp.coeffs[(by * comp.bw + bx) * 64 ..][0..64];
                        try decodeScanBlock(&br, blk, &comp.dc_pred, &dc_huff[comp.dc_tbl], &ac_huff[comp.ac_tbl], ss, se, ah, al, &eob_run);
                    }
                }
            }

            // Advance outer parser `pos` to end of entropy-coded segment
            if (br.marker != 0) {
                pos = br.pos - 2;
            } else {
                pos = br.pos;
            }
        }
    }

    if (!have_sof) return error.InvalidJpeg;
    for (0..num_comp) |ci| {
        if (!quant_defined[comps[ci].q_id]) return error.InvalidJpeg;
    }

    // Reconstruct spatial planes per component
    const planes = try allocator.alloc([]u8, num_comp);
    defer {
        for (0..num_comp) |ci| {
            if (planes[ci].len > 0) allocator.free(planes[ci]);
        }
        allocator.free(planes);
    }
    for (0..num_comp) |ci| planes[ci] = &.{};

    for (0..num_comp) |ci| {
        const comp = &comps[ci];
        const pw = comp.bw * 8;
        const ph = comp.bh * 8;
        const plane = try allocator.alloc(u8, pw * ph);
        planes[ci] = plane;
        const qt = &quants[comp.q_id];

        var fcoeffs: [64]f32 = undefined;
        var spatial: [64]u8 = undefined;
        for (0..comp.bh) |by| {
            for (0..comp.bw) |bx| {
                const blk = comp.coeffs[(by * comp.bw + bx) * 64 ..][0..64];
                for (0..64) |i| {
                    const idx = ZIGZAG[i];
                    fcoeffs[idx] = @as(f32, @floatFromInt(blk[i])) * @as(f32, @floatFromInt(qt[idx]));
                }
                idct8x8(&fcoeffs, &spatial);
                for (0..8) |sy| {
                    const dst_off = (by * 8 + sy) * pw + bx * 8;
                    @memcpy(plane[dst_off .. dst_off + 8], spatial[sy * 8 ..][0..8]);
                }
            }
        }
    }

    const w: usize = width;
    const h: usize = height;
    var rgb = try allocator.alloc(u8, w * h * 3);
    errdefer allocator.free(rgb);

    if (num_comp == 1) {
        const pw = comps[0].bw * 8;
        for (0..h) |y| {
            for (0..w) |x| {
                const g = planes[0][y * pw + x];
                const idx = (y * w + x) * 3;
                rgb[idx] = g;
                rgb[idx + 1] = g;
                rgb[idx + 2] = g;
            }
        }
    } else {
        const pw0 = comps[0].bw * 8;
        const pw1 = comps[1].bw * 8;
        const pw2 = comps[2].bw * 8;
        for (0..h) |y| {
            const y0 = (y * comps[0].v_samp) / max_v;
            const y1 = (y * comps[1].v_samp) / max_v;
            const y2 = (y * comps[2].v_samp) / max_v;
            for (0..w) |x| {
                const x0 = (x * comps[0].h_samp) / max_h;
                const x1 = (x * comps[1].h_samp) / max_h;
                const x2 = (x * comps[2].h_samp) / max_h;

                const yy: f32 = @floatFromInt(planes[0][y0 * pw0 + x0]);
                const cb: f32 = @as(f32, @floatFromInt(planes[1][y1 * pw1 + x1])) - 128.0;
                const cr: f32 = @as(f32, @floatFromInt(planes[2][y2 * pw2 + x2])) - 128.0;

                const r: i32 = @intFromFloat(@round(yy + 1.402 * cr));
                const g: i32 = @intFromFloat(@round(yy - 0.344136 * cb - 0.714136 * cr));
                const b: i32 = @intFromFloat(@round(yy + 1.772 * cb));

                const idx = (y * w + x) * 3;
                rgb[idx] = @intCast(math.clamp(r, 0, 255));
                rgb[idx + 1] = @intCast(math.clamp(g, 0, 255));
                rgb[idx + 2] = @intCast(math.clamp(b, 0, 255));
            }
        }
    }

    const orientation = parseJpegExifOrientation(buffer);
    if (orientation >= 2) {
        try autorotatePixels(allocator, &rgb, &width, &height, 3, orientation);
    }

    return DecodedImage{
        .width = width,
        .height = height,
        .channels = 3,
        .data = rgb,
    };
}

fn decodeScanBlock(
    br: *JpegBitReader,
    blk: *[64]i16,
    dc_pred: *i32,
    dc_tbl: *const HuffDecTable,
    ac_tbl: *const HuffDecTable,
    ss: usize,
    se: usize,
    ah: u5,
    al: u5,
    eob_run: *usize,
) CodecError!void {
    if (ss == 0) {
        if (ah == 0) {
            const s = try br.decodeHuff(dc_tbl);
            if (s > 15) return error.InvalidJpeg;
            const diff = br.readSigned(@intCast(s));
            dc_pred.* += diff;
            blk[0] = @intCast(dc_pred.* << al);
        } else {
            const bit: i16 = @intCast(br.readBits(1));
            blk[0] |= bit << @intCast(al);
        }
    }

    if (se == 0) return;

    var k: usize = @max(ss, 1);
    if (ah == 0) {
        if (eob_run.* > 0) {
            eob_run.* -= 1;
            return;
        }
        while (k <= se) {
            const rs = try br.decodeHuff(ac_tbl);
            const r: usize = rs >> 4;
            const s: u5 = @intCast(rs & 0x0F);
            if (s == 0) {
                if (r == 15) {
                    k += 16;
                    continue;
                }
                const extra = if (r > 0) br.readBits(@intCast(r)) else 0;
                eob_run.* = ((@as(usize, 1) << @intCast(r)) + extra) - 1;
                break;
            }
            k += r;
            if (k > se) return error.InvalidJpeg;
            const val = br.readSigned(s);
            blk[k] = @intCast(val << al);
            k += 1;
        }
    } else {
        // Progressive AC successive approximation refinement (Annex G.1.2.3)
        const p1: i16 = @as(i16, 1) << @intCast(al);
        const m1: i16 = -p1;
        if (eob_run.* > 0) {
            while (k <= se) : (k += 1) {
                if (blk[k] != 0) {
                    if (br.readBits(1) != 0 and (blk[k] & p1) == 0) {
                        blk[k] += if (blk[k] >= 0) p1 else m1;
                    }
                }
            }
            eob_run.* -= 1;
            return;
        }
        while (k <= se) {
            const rs = try br.decodeHuff(ac_tbl);
            var r: usize = rs >> 4;
            const s: usize = rs & 0x0F;
            if (s == 0) {
                if (r < 15) {
                    const extra = if (r > 0) br.readBits(@intCast(r)) else 0;
                    eob_run.* = ((@as(usize, 1) << @intCast(r)) + extra) - 1;
                    while (k <= se) : (k += 1) {
                        if (blk[k] != 0) {
                            if (br.readBits(1) != 0 and (blk[k] & p1) == 0) {
                                blk[k] += if (blk[k] >= 0) p1 else m1;
                            }
                        }
                    }
                    break;
                }
                // ZRL (r=15, s=0): skip 16 zero coefficients while refining non-zeros
                var zeros: usize = 0;
                while (k <= se) : (k += 1) {
                    if (blk[k] != 0) {
                        if (br.readBits(1) != 0 and (blk[k] & p1) == 0) {
                            blk[k] += if (blk[k] >= 0) p1 else m1;
                        }
                    } else {
                        zeros += 1;
                        if (zeros == 16) {
                            k += 1;
                            break;
                        }
                    }
                }
            } else if (s == 1) {
                const new_val: i16 = if (br.readBits(1) != 0) p1 else m1;
                while (k <= se) : (k += 1) {
                    if (blk[k] != 0) {
                        if (br.readBits(1) != 0 and (blk[k] & p1) == 0) {
                            blk[k] += if (blk[k] >= 0) p1 else m1;
                        }
                    } else {
                        if (r == 0) {
                            blk[k] = new_val;
                            k += 1;
                            break;
                        }
                        r -= 1;
                    }
                }
            } else {
                return error.InvalidJpeg;
            }
        }
    }
}

// ============================================================================
// Unit Tests
// ============================================================================

test "sha256 known vectors" {
    const t = std.testing;
    const empty_hex = sha256HexBuf("");
    try t.expectEqualStrings("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", &empty_hex);

    const abc_hex = try sha256HexAlloc(t.allocator, "abc");
    defer t.allocator.free(abc_hex);
    try t.expectEqualStrings("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", abc_hex);
}

test "sr xmp build and probe with xml escaping" {
    const t = std.testing;
    const xmp = try buildSrXmp(t.allocator, "Real-ESRGAN & <Anime> \"x2\" 'v3'", 2, 1730000000);
    defer t.allocator.free(xmp);

    try t.expect(mem.indexOf(u8, xmp, "&amp;") != null);
    try t.expect(mem.indexOf(u8, xmp, "&lt;Anime&gt;") != null);

    var marker = (try probeSrMarker(t.allocator, xmp)).?;
    defer marker.deinit(t.allocator);
    try t.expectEqualStrings("Real-ESRGAN & <Anime> \"x2\" 'v3'", marker.model);
    try t.expectEqual(@as(i32, 2), marker.scale);
    try t.expectEqual(@as(i64, 1730000000), marker.ts);

    const json = try probeSrMarkerJson(t.allocator, xmp);
    defer t.allocator.free(json);
    try t.expect(mem.indexOf(u8, json, "\"scale\":2") != null);

    const none_json = try probeSrMarkerJson(t.allocator, "no marker here");
    defer t.allocator.free(none_json);
    try t.expectEqualStrings("null", none_json);
}

fn makeTestJpegWithExif(allocator: Allocator, little_endian: bool, orientation: u16) ![]u8 {
    // Build minimal TIFF IFD0 with tag 0x0112 = orientation
    var tiff: [26]u8 = undefined;
    if (little_endian) {
        tiff[0] = 'I';
        tiff[1] = 'I';
        mem.writeInt(u16, tiff[2..4], 42, .little);
        mem.writeInt(u32, tiff[4..8], 8, .little);
        mem.writeInt(u16, tiff[8..10], 1, .little); // 1 entry
        mem.writeInt(u16, tiff[10..12], 0x0112, .little);
        mem.writeInt(u16, tiff[12..14], 3, .little); // SHORT
        mem.writeInt(u32, tiff[14..18], 1, .little); // count
        mem.writeInt(u16, tiff[18..20], orientation, .little);
        tiff[20] = 0;
        tiff[21] = 0;
        mem.writeInt(u32, tiff[22..26], 0, .little);
    } else {
        tiff[0] = 'M';
        tiff[1] = 'M';
        mem.writeInt(u16, tiff[2..4], 42, .big);
        mem.writeInt(u32, tiff[4..8], 8, .big);
        mem.writeInt(u16, tiff[8..10], 1, .big);
        mem.writeInt(u16, tiff[10..12], 0x0112, .big);
        mem.writeInt(u16, tiff[12..14], 3, .big);
        mem.writeInt(u32, tiff[14..18], 1, .big);
        mem.writeInt(u16, tiff[18..20], orientation, .big);
        tiff[20] = 0;
        tiff[21] = 0;
        mem.writeInt(u32, tiff[22..26], 0, .big);
    }

    const px = [_]u8{
        255, 0,   0,   0,   255, 0,
        0,   0,   255, 255, 255, 0,
    };
    const base_jpg = try encodeJpeg(allocator, &px, 2, 2, 3, 95, 444, "");
    defer allocator.free(base_jpg);

    const app1_len: u16 = @intCast(2 + 6 + tiff.len);
    var out = try allocator.alloc(u8, 2 + 2 + app1_len + (base_jpg.len - 2));
    out[0] = 0xFF;
    out[1] = 0xD8;
    out[2] = 0xFF;
    out[3] = 0xE1;
    mem.writeInt(u16, out[4..6], app1_len, .big);
    @memcpy(out[6..12], "Exif\x00\x00");
    @memcpy(out[12 .. 12 + tiff.len], &tiff);
    @memcpy(out[12 + tiff.len ..], base_jpg[2..]);
    return out;
}

test "exif orientation parse 1..8 and invalid inputs" {
    const t = std.testing;
    try t.expectEqual(@as(u8, 1), parseJpegExifOrientation(""));
    try t.expectEqual(@as(u8, 1), parseJpegExifOrientation("not a jpeg"));
    try t.expectEqual(@as(u8, 1), parseJpegExifOrientation("\xFF\xD8"));

    for (1..9) |o| {
        const jpg_le = try makeTestJpegWithExif(t.allocator, true, @intCast(o));
        defer t.allocator.free(jpg_le);
        try t.expectEqual(@as(u8, @intCast(o)), parseJpegExifOrientation(jpg_le));

        const jpg_be = try makeTestJpegWithExif(t.allocator, false, @intCast(o));
        defer t.allocator.free(jpg_be);
        try t.expectEqual(@as(u8, @intCast(o)), parseJpegExifOrientation(jpg_be));
    }

    const jpg_oob = try makeTestJpegWithExif(t.allocator, true, 9);
    defer t.allocator.free(jpg_oob);
    try t.expectEqual(@as(u8, 1), parseJpegExifOrientation(jpg_oob));
}

test "png encode and decode roundtrip with xmp and all channel counts" {
    const t = std.testing;
    const xmp = try buildSrXmp(t.allocator, "Real-ESRGANv3-anime-x2", 2, 1730000000);
    defer t.allocator.free(xmp);

    // 3ch RGB
    const w: u32 = 17;
    const h: u32 = 13;
    var rgb: [w * h * 3]u8 = undefined;
    for (0..h) |y| {
        for (0..w) |x| {
            const idx = (y * w + x) * 3;
            rgb[idx] = @intCast((x * 15 + y * 3) & 0xFF);
            rgb[idx + 1] = @intCast((y * 19 + 50) & 0xFF);
            rgb[idx + 2] = @intCast(((x ^ y) * 17) & 0xFF);
        }
    }
    const png_rgb = try encodePng(t.allocator, &rgb, w, h, 3, 90, -1, xmp);
    defer t.allocator.free(png_rgb);

    var m = (try probeSrMarker(t.allocator, png_rgb)).?;
    defer m.deinit(t.allocator);
    try t.expectEqualStrings("Real-ESRGANv3-anime-x2", m.model);
    try t.expectEqual(@as(i32, 2), m.scale);

    var dec_rgb = try decodePng(t.allocator, png_rgb);
    defer dec_rgb.deinit(t.allocator);
    try t.expectEqual(w, dec_rgb.width);
    try t.expectEqual(h, dec_rgb.height);
    try t.expectEqual(@as(u32, 3), dec_rgb.channels);
    try t.expectEqualSlices(u8, &rgb, dec_rgb.data);

    // 4ch RGBA at png_level=0 (stored) & png_level=9 (best)
    var rgba: [w * h * 4]u8 = undefined;
    for (0..w * h) |i| {
        rgba[i * 4] = rgb[i * 3];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = @intCast((i * 7) & 0xFF);
    }
    for ([_]i32{ 0, 9 }) |lvl| {
        const png_rgba = try encodePng(t.allocator, &rgba, w, h, 4, 90, lvl, xmp);
        defer t.allocator.free(png_rgba);
        var dec_rgba = try decodePng(t.allocator, png_rgba);
        defer dec_rgba.deinit(t.allocator);
        try t.expectEqual(@as(u32, 4), dec_rgba.channels);
        try t.expectEqualSlices(u8, &rgba, dec_rgba.data);
    }

    // 1ch Grayscale -> expands to 3ch RGB on decode
    var gray: [w * h]u8 = undefined;
    for (0..w * h) |i| gray[i] = @intCast((i * 11) & 0xFF);
    const png_gray = try encodePng(t.allocator, &gray, w, h, 1, 80, -1, "");
    defer t.allocator.free(png_gray);
    var dec_gray = try decodePng(t.allocator, png_gray);
    defer dec_gray.deinit(t.allocator);
    try t.expectEqual(@as(u32, 3), dec_gray.channels);
    for (0..w * h) |i| {
        try t.expectEqual(gray[i], dec_gray.data[i * 3]);
        try t.expectEqual(gray[i], dec_gray.data[i * 3 + 1]);
        try t.expectEqual(gray[i], dec_gray.data[i * 3 + 2]);
    }
}

test "jpeg encode and decode roundtrip 444 422 420 and xmp and autorotate" {
    const t = std.testing;
    const xmp = try buildSrXmp(t.allocator, "NomosUni-SPAN-x2", 2, 1730000123);
    defer t.allocator.free(xmp);

    const w: u32 = 24;
    const h: u32 = 16;
    var rgb: [w * h * 3]u8 = undefined;
    for (0..h) |y| {
        for (0..w) |x| {
            const idx = (y * w + x) * 3;
            rgb[idx] = @intCast((x * 10) & 0xFF);
            rgb[idx + 1] = @intCast((y * 14) & 0xFF);
            rgb[idx + 2] = 128;
        }
    }

    for ([_]i32{ 444, 422, 420 }) |subs| {
        const jpg = try encodeJpeg(t.allocator, &rgb, w, h, 3, 95, subs, xmp);
        defer t.allocator.free(jpg);

        var m = (try probeSrMarker(t.allocator, jpg)).?;
        defer m.deinit(t.allocator);
        try t.expectEqualStrings("NomosUni-SPAN-x2", m.model);
        try t.expectEqual(@as(i32, 2), m.scale);

        var dec = try decodeJpeg(t.allocator, jpg);
        defer dec.deinit(t.allocator);
        try t.expectEqual(w, dec.width);
        try t.expectEqual(h, dec.height);
        try t.expectEqual(@as(u32, 3), dec.channels);

        var max_diff: i32 = 0;
        for (rgb, dec.data) |a, b| {
            const d: i32 = @intCast(@abs(@as(i32, a) - @as(i32, b)));
            if (d > max_diff) max_diff = d;
        }
        try t.expect(max_diff < 25);
    }

    // EXIF orientation 6 (90 CW) swaps width & height on decode
    const rot_jpg = try makeTestJpegWithExif(t.allocator, true, 6);
    defer t.allocator.free(rot_jpg);
    // 2x2 stays 2x2; test non-square via manual 4x2 EXIF
    var dec_rot = try decodeJpeg(t.allocator, rot_jpg);
    defer dec_rot.deinit(t.allocator);
    try t.expectEqual(@as(u32, 2), dec_rot.width);
    try t.expectEqual(@as(u32, 2), dec_rot.height);
}

test "webp attach xmp simple and extended vp8x" {
    const t = std.testing;
    const xmp = try buildSrXmp(t.allocator, "Real-CUGAN-no-denoise-x2", 2, 1730000000);
    defer t.allocator.free(xmp);

    // Minimal valid VP8L 1x1 lossless WebP container
    const vp8l_payload = [_]u8{ 0x2F, 0x00, 0x00, 0x00, 0x00, 0x00 };
    var simple_webp: [12 + 8 + vp8l_payload.len]u8 = undefined;
    @memcpy(simple_webp[0..4], "RIFF");
    mem.writeInt(u32, simple_webp[4..8], 4 + 8 + vp8l_payload.len, .little);
    @memcpy(simple_webp[8..12], "WEBP");
    @memcpy(simple_webp[12..16], "VP8L");
    mem.writeInt(u32, simple_webp[16..20], vp8l_payload.len, .little);
    @memcpy(simple_webp[20..], &vp8l_payload);

    const muxed = try webpAttachXmp(t.allocator, &simple_webp, xmp);
    defer t.allocator.free(muxed);
    var m1 = (try probeSrMarker(t.allocator, muxed)).?;
    defer m1.deinit(t.allocator);
    try t.expectEqualStrings("Real-CUGAN-no-denoise-x2", m1.model);
    try t.expectEqual(@as(i32, 2), m1.scale);

    // Re-muxing an already-extended VP8X WebP replaces XMP cleanly
    const xmp2 = try buildSrXmp(t.allocator, "Span-x4", 4, 1730000999);
    defer t.allocator.free(xmp2);
    const remuxed = try webpAttachXmp(t.allocator, muxed, xmp2);
    defer t.allocator.free(remuxed);
    var m2 = (try probeSrMarker(t.allocator, remuxed)).?;
    defer m2.deinit(t.allocator);
    try t.expectEqualStrings("Span-x4", m2.model);
    try t.expectEqual(@as(i32, 4), m2.scale);
    try t.expectEqual(@as(i64, 1730000999), m2.ts);

    // Empty XMP returns exact copy
    const passthrough = try webpAttachXmp(t.allocator, &simple_webp, "");
    defer t.allocator.free(passthrough);
    try t.expectEqualSlices(u8, &simple_webp, passthrough);

    // Invalid WebP returns error.InvalidWebp
    try t.expectError(error.InvalidWebp, webpAttachXmp(t.allocator, "not-webp", xmp));
}

test "png 2ch gray-alpha and jpeg 1ch/4ch and autorotate 1..8" {
    const t = std.testing;

    // 2ch Grayscale+Alpha PNG expands to 4ch RGBA on decode
    const ga = [_]u8{ 10, 200, 120, 50, 240, 255, 0, 0 };
    const png_ga = try encodePng(t.allocator, &ga, 2, 2, 2, 90, -1, "");
    defer t.allocator.free(png_ga);
    var dec_ga = try decodePng(t.allocator, png_ga);
    defer dec_ga.deinit(t.allocator);
    try t.expectEqual(@as(u32, 4), dec_ga.channels);
    try t.expectEqualSlices(u8, &[_]u8{
        10,  10,  10,  200,
        120, 120, 120, 50,
        240, 240, 240, 255,
        0,   0,   0,   0,
    }, dec_ga.data);

    // 1ch Luma JPEG
    var luma: [16 * 16]u8 = undefined;
    for (0..256) |i| luma[i] = @intCast(i);
    const jpg_1ch = try encodeJpeg(t.allocator, &luma, 16, 16, 1, 95, 444, "");
    defer t.allocator.free(jpg_1ch);
    var dec_1ch = try decodeJpeg(t.allocator, jpg_1ch);
    defer dec_1ch.deinit(t.allocator);
    try t.expectEqual(@as(u32, 3), dec_1ch.channels);
    try t.expectEqual(@as(u32, 16), dec_1ch.width);

    // 4ch RGBA -> JPEG premultiplied over black (alpha=0 becomes 0,0,0)
    const rgba = [_]u8{
        200, 100, 50, 0,
        200, 100, 50, 255,
        200, 100, 50, 0,
        200, 100, 50, 255,
    };
    const jpg_4ch = try encodeJpeg(t.allocator, &rgba, 2, 2, 4, 95, 444, "");
    defer t.allocator.free(jpg_4ch);
    var dec_4ch = try decodeJpeg(t.allocator, jpg_4ch);
    defer dec_4ch.deinit(t.allocator);
    try t.expect(dec_4ch.data[0] < 20);
    try t.expect(dec_4ch.data[3] > 180);

    // Non-square 4x2 autorotate for orientations 1..8
    for (1..9) |o| {
        const w: u32 = 4;
        const h: u32 = 2;
        const orig = [_]u8{
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
            13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
        };
        var buf = try t.allocator.dupe(u8, &orig);
        defer t.allocator.free(buf);
        var rw = w;
        var rh = h;
        try autorotatePixels(t.allocator, &buf, &rw, &rh, 3, @intCast(o));
        if (o >= 5) {
            try t.expectEqual(@as(u32, 2), rw);
            try t.expectEqual(@as(u32, 4), rh);
        } else {
            try t.expectEqual(@as(u32, 4), rw);
            try t.expectEqual(@as(u32, 2), rh);
        }
    }
}
