//! WebAssembly C ABI exports (`upix_*`) matching `crates/upix-wasm/src/lib.rs`
//! and `web/src/runtime/wasm-bridge.js`.

const std = @import("std");
const builtin = @import("builtin");

const bicubic = @import("bicubic.zig");
const catalog = @import("catalog.zig");
const codec = @import("codec.zig");
const cpu_engine = @import("cpu_engine.zig");
const tile = @import("tile.zig");

pub const allocator: std.mem.Allocator = if (builtin.target.cpu.arch == .wasm32 and builtin.single_threaded)
    std.heap.wasm_allocator
else
    std.heap.smp_allocator;

var result_buf: []u8 = &.{};
var last_error_buf: []u8 = &.{};
var next_handle: u32 = 1;
var models: std.AutoHashMapUnmanaged(u32, *cpu_engine.WasmCpuEngine) = .empty;

fn setResultBytesOwned(bytes: []u8) i32 {
    if (result_buf.len > 0) {
        allocator.free(result_buf);
    }
    result_buf = bytes;
    return 0;
}

fn setError(msg: []const u8) i32 {
    if (last_error_buf.len > 0) {
        allocator.free(last_error_buf);
        last_error_buf = &.{};
    }
    last_error_buf = allocator.dupe(u8, msg) catch &.{};
    return -1;
}

fn setErrorFmt(comptime fmt: []const u8, args: anytype) i32 {
    if (last_error_buf.len > 0) {
        allocator.free(last_error_buf);
        last_error_buf = &.{};
    }
    last_error_buf = std.fmt.allocPrint(allocator, fmt, args) catch &.{};
    return -1;
}

fn sliceFromRaw(ptr: ?[*]const u8, len: usize) []const u8 {
    const p = ptr orelse return &.{};
    if (len == 0) return &.{};
    return p[0..len];
}

fn strFromRaw(ptr: ?[*]const u8, len: usize) ![]const u8 {
    const bytes = sliceFromRaw(ptr, len);
    if (!std.unicode.utf8ValidateSlice(bytes)) {
        return error.InvalidUtf8;
    }
    return bytes;
}

// ============================================================================
// Memory / Result / Error Buffers
// ============================================================================

pub export fn upix_alloc(size: usize) ?[*]u8 {
    if (size == 0) return null;
    const slice = allocator.alignedAlloc(u8, .@"16", size) catch return null;
    return slice.ptr;
}

pub export fn upix_dealloc(ptr: ?[*]u8, size: usize) void {
    const p = ptr orelse return;
    if (size == 0) return;
    const aligned: [*]align(16) u8 = @alignCast(p);
    allocator.free(aligned[0..size]);
}

pub export fn upix_result_ptr() ?[*]const u8 {
    if (result_buf.len == 0) return null;
    return result_buf.ptr;
}

pub export fn upix_result_len() usize {
    return result_buf.len;
}

pub export fn upix_last_error_ptr() ?[*]const u8 {
    if (last_error_buf.len == 0) return null;
    return last_error_buf.ptr;
}

pub export fn upix_last_error_len() usize {
    return last_error_buf.len;
}

// ============================================================================
// Catalog & Hashing
// ============================================================================

pub export fn upix_catalog_json() i32 {
    const json = catalog.exportCatalogJson(allocator) catch |err| {
        return setErrorFmt("catalog serialize error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(json);
}

pub export fn upix_classify_model_json(name_ptr: ?[*]const u8, name_len: usize) i32 {
    const name = strFromRaw(name_ptr, name_len) catch {
        return setError("invalid UTF-8 string in upix_classify_model_json");
    };
    const json = catalog.classifyModelJson(allocator, name) catch |err| {
        return setErrorFmt("classify serialize error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(json);
}

pub export fn upix_sha256_hex(ptr: ?[*]const u8, len: usize) i32 {
    const data = sliceFromRaw(ptr, len);
    const hex = codec.sha256HexAlloc(allocator, data) catch |err| {
        return setErrorFmt("sha256 error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(hex);
}

// ============================================================================
// Model Lifecycle & CPU Inference
// ============================================================================

pub export fn upix_model_load(
    name_ptr: ?[*]const u8,
    name_len: usize,
    param_ptr: ?[*]const u8,
    param_len: usize,
    bin_ptr: ?[*]const u8,
    bin_len: usize,
) u32 {
    const name = strFromRaw(name_ptr, name_len) catch {
        _ = setError("invalid UTF-8 model name");
        return 0;
    };
    const param_text = strFromRaw(param_ptr, param_len) catch {
        _ = setError("invalid UTF-8 param text");
        return 0;
    };
    const bin_bytes = sliceFromRaw(bin_ptr, bin_len);

    const eng = allocator.create(cpu_engine.WasmCpuEngine) catch {
        _ = setError("out of memory creating engine");
        return 0;
    };
    eng.* = cpu_engine.WasmCpuEngine.init(allocator);

    eng.loadModelBytes(name, param_text, bin_bytes) catch |err| {
        const msg = eng.lastError();
        if (msg.len > 0) {
            _ = setError(msg);
        } else {
            _ = setErrorFmt("model load error: {s}", .{@errorName(err)});
        }
        eng.deinit();
        allocator.destroy(eng);
        return 0;
    };

    var h = next_handle;
    while (h == 0 or models.contains(h)) {
        h +%= 1;
    }
    next_handle = h +% 1;
    if (next_handle == 0) next_handle = 1;

    models.put(allocator, h, eng) catch {
        _ = setError("out of memory storing model handle");
        eng.deinit();
        allocator.destroy(eng);
        return 0;
    };
    return h;
}

pub export fn upix_model_free(handle: u32) void {
    if (models.fetchRemove(handle)) |kv| {
        kv.value.deinit();
        allocator.destroy(kv.value);
    }
}

pub export fn upix_model_meta_json(handle: u32) i32 {
    const eng = models.get(handle) orelse {
        return setErrorFmt("invalid model handle {d}", .{handle});
    };
    const g = eng.graph() orelse {
        return setError("model has no graph");
    };
    const json = g.exportMetaJson(allocator) catch |err| {
        return setErrorFmt("meta serialize error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(json);
}

pub export fn upix_model_weights_ptr(handle: u32) ?[*]const f32 {
    const eng = models.get(handle) orelse return null;
    const g = eng.graph() orelse return null;
    if (g.weights.len == 0) return null;
    return g.weights.ptr;
}

pub export fn upix_model_weights_len(handle: u32) usize {
    const eng = models.get(handle) orelse return 0;
    const g = eng.graph() orelse return 0;
    return g.weights.len;
}

pub export fn upix_model_infer_f32(
    handle: u32,
    in_ptr: ?[*]const f32,
    out_ptr: ?[*]f32,
    w: i32,
    h: i32,
    c: i32,
) i32 {
    const ip = in_ptr orelse return setError("invalid arguments to upix_model_infer_f32");
    const op = out_ptr orelse return setError("invalid arguments to upix_model_infer_f32");
    if (w <= 0 or h <= 0 or c <= 0) {
        return setError("invalid arguments to upix_model_infer_f32");
    }
    const eng = models.get(handle) orelse {
        return setErrorFmt("invalid model handle {d}", .{handle});
    };

    const scale: usize = @intCast(@max(eng.getScaleFactor(), 1));
    const wu: usize = @intCast(w);
    const hu: usize = @intCast(h);
    const cu: usize = @intCast(c);
    const in_len = wu * hu * cu;
    const out_len = (wu * scale) * (hu * scale) * cu;

    eng.process(ip[0..in_len], op[0..out_len], w, h, c) catch |err| {
        const msg = eng.lastError();
        if (msg.len > 0) return setError(msg);
        return setErrorFmt("infer error: {s}", .{@errorName(err)});
    };
    return 0;
}

pub export fn upix_model_process_image_u8(
    handle: u32,
    in_ptr: ?[*]const u8,
    in_len: usize,
    w: i32,
    h: i32,
    c: i32,
    tile_size: i32,
    overlap: i32,
    alpha_bicubic: i32,
) i32 {
    const eng = models.get(handle) orelse {
        return setErrorFmt("invalid model handle {d}", .{handle});
    };
    const input = sliceFromRaw(in_ptr, in_len);
    const out = cpu_engine.processImageU8Single(
        allocator,
        input,
        w,
        h,
        c,
        eng,
        tile_size,
        overlap,
        alpha_bicubic != 0,
    ) catch |err| {
        const msg = eng.lastError();
        if (msg.len > 0) return setError(msg);
        return setErrorFmt("process image error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(out);
}

// ============================================================================
// Tile Layout / Extraction / Blending / Bicubic
// ============================================================================

pub export fn upix_compute_tile_layout_json(w: i32, h: i32, tile_size: i32, overlap: i32) i32 {
    const json = tile.computeTileLayoutJson(allocator, w, h, tile_size, overlap) catch |err| {
        return setErrorFmt("tile layout serialize error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(json);
}

pub export fn upix_extract_tile_u8(
    img_ptr: ?[*]const u8,
    img_w: i32,
    img_h: i32,
    src_c: i32,
    dst_c: i32,
    x: u32,
    y: u32,
    tw: u32,
    th: u32,
    out_f32_ptr: ?[*]f32,
) i32 {
    const ip = img_ptr orelse return setError("invalid arguments to upix_extract_tile_u8");
    const op = out_f32_ptr orelse return setError("invalid arguments to upix_extract_tile_u8");
    if (img_w <= 0 or img_h <= 0 or src_c <= 0 or dst_c <= 0) {
        return setError("invalid arguments to upix_extract_tile_u8");
    }
    const img_len: usize = @as(usize, @intCast(img_w)) * @as(usize, @intCast(img_h)) * @as(usize, @intCast(src_c));
    const out_len: usize = @as(usize, tw) * @as(usize, th) * @as(usize, @intCast(dst_c));
    const region: tile.TileRegion = .{
        .x = x,
        .y = y,
        .width = tw,
        .height = th,
        .overlap = 0,
    };
    tile.extractTileU8(ip[0..img_len], img_w, src_c, dst_c, region, op[0..out_len]);
    return 0;
}

pub export fn upix_blend_tile(
    tile_ptr: ?[*]const f32,
    accum_ptr: ?[*]f32,
    weight_ptr: ?[*]f32,
    out_w: i32,
    out_h: i32,
    channels: i32,
    rx: u32,
    ry: u32,
    rw: u32,
    rh: u32,
    scale: i32,
    overlap: i32,
) void {
    const tp = tile_ptr orelse return;
    const ap = accum_ptr orelse return;
    const wp = weight_ptr orelse return;
    if (out_w <= 0 or out_h <= 0 or channels <= 0) return;

    const s: i32 = @max(scale, 1);
    const s_u: usize = @intCast(s);
    const ch_u: usize = @intCast(channels);
    const tile_len: usize = (@as(usize, rw) * s_u) * (@as(usize, rh) * s_u) * ch_u;
    const px_count: usize = @as(usize, @intCast(out_w)) * @as(usize, @intCast(out_h));
    const region: tile.TileRegion = .{
        .x = rx,
        .y = ry,
        .width = rw,
        .height = rh,
        .overlap = @intCast(@max(overlap, 0)),
    };
    tile.blendTile(
        allocator,
        tp[0..tile_len],
        ap[0 .. px_count * ch_u],
        wp[0..px_count],
        out_w,
        out_h,
        channels,
        region,
        s,
        overlap,
    ) catch return;
}

pub export fn upix_finalize_blend_u8(
    accum_ptr: ?[*]const f32,
    weight_ptr: ?[*]const f32,
    alpha_ptr: ?[*]const u8,
    out_u8_ptr: ?[*]u8,
    out_w: i32,
    out_h: i32,
    model_channels: i32,
    out_channels: i32,
) void {
    const ap = accum_ptr orelse return;
    const wp = weight_ptr orelse return;
    const op = out_u8_ptr orelse return;
    const ow: usize = @intCast(@max(out_w, 0));
    const oh: usize = @intCast(@max(out_h, 0));
    const px = ow * oh;
    const mc: usize = @intCast(@max(model_channels, 1));
    const oc: usize = @intCast(@max(out_channels, 1));
    const alpha: ?[]const u8 = if (alpha_ptr) |alp| alp[0..px] else null;
    tile.finalizeBlendU8(
        ap[0 .. px * mc],
        wp[0..px],
        alpha,
        op[0 .. px * oc],
        px,
        mc,
        oc,
    );
}

pub export fn upix_bicubic_scale_u8(
    src_ptr: ?[*]const u8,
    src_w: u32,
    src_h: u32,
    dst_ptr: ?[*]u8,
    dst_w: u32,
    dst_h: u32,
) void {
    const sp = src_ptr orelse return;
    const dp = dst_ptr orelse return;
    if (src_w == 0 or src_h == 0 or dst_w == 0 or dst_h == 0) return;
    const src_len: usize = @as(usize, src_w) * @as(usize, src_h);
    const dst_len: usize = @as(usize, dst_w) * @as(usize, dst_h);
    bicubic.bicubicScaleU8(allocator, sp[0..src_len], src_w, src_h, dp[0..dst_len], dst_w, dst_h) catch return;
}

// ============================================================================
// Image Codecs & Metadata
// ============================================================================

pub export fn upix_probe_sr_marker_json(bytes_ptr: ?[*]const u8, bytes_len: usize) i32 {
    const bytes = sliceFromRaw(bytes_ptr, bytes_len);
    const json = codec.probeSrMarkerJson(allocator, bytes) catch |err| {
        return setErrorFmt("marker serialize error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(json);
}

pub export fn upix_build_sr_xmp(
    model_ptr: ?[*]const u8,
    model_len: usize,
    scale: i32,
    ts_sec: i64,
) i32 {
    const model = strFromRaw(model_ptr, model_len) catch {
        return setError("invalid UTF-8 string in upix_build_sr_xmp");
    };
    const xmp = codec.buildSrXmp(allocator, model, scale, ts_sec) catch |err| {
        return setErrorFmt("build xmp error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(xmp);
}

pub export fn upix_parse_jpeg_orientation(bytes_ptr: ?[*]const u8, bytes_len: usize) u32 {
    const bytes = sliceFromRaw(bytes_ptr, bytes_len);
    return @as(u32, codec.parseJpegExifOrientation(bytes));
}

pub export fn upix_encode_png(
    px_ptr: ?[*]const u8,
    px_len: usize,
    w: u32,
    h: u32,
    c: u32,
    quality: i32,
    png_level: i32,
    xmp_ptr: ?[*]const u8,
    xmp_len: usize,
) i32 {
    const px = sliceFromRaw(px_ptr, px_len);
    const xmp = strFromRaw(xmp_ptr, xmp_len) catch {
        return setError("invalid UTF-8 XMP in upix_encode_png");
    };
    const out = codec.encodePng(allocator, px, w, h, c, quality, png_level, xmp) catch |err| {
        return setErrorFmt("PNG encode error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(out);
}

pub export fn upix_encode_jpeg(
    px_ptr: ?[*]const u8,
    px_len: usize,
    w: u32,
    h: u32,
    c: u32,
    quality: i32,
    subsampling: i32,
    xmp_ptr: ?[*]const u8,
    xmp_len: usize,
) i32 {
    const px = sliceFromRaw(px_ptr, px_len);
    const xmp = strFromRaw(xmp_ptr, xmp_len) catch {
        return setError("invalid UTF-8 XMP in upix_encode_jpeg");
    };
    const out = codec.encodeJpeg(allocator, px, w, h, c, quality, subsampling, xmp) catch |err| {
        return setErrorFmt("JPEG encode error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(out);
}

pub export fn upix_webp_attach_xmp(
    webp_ptr: ?[*]const u8,
    webp_len: usize,
    xmp_ptr: ?[*]const u8,
    xmp_len: usize,
) i32 {
    const webp = sliceFromRaw(webp_ptr, webp_len);
    const xmp = strFromRaw(xmp_ptr, xmp_len) catch {
        return setError("invalid UTF-8 XMP in upix_webp_attach_xmp");
    };
    const out = codec.webpAttachXmp(allocator, webp, xmp) catch |err| {
        return setErrorFmt("WebP attach XMP error: {s}", .{@errorName(err)});
    };
    return setResultBytesOwned(out);
}

test "alloc, catalog, sha256, error ABI smoke" {
    const p = upix_alloc(256) orelse return error.OutOfMemory;
    p[0] = 42;
    upix_dealloc(p, 256);

    try std.testing.expectEqual(@as(i32, 0), upix_catalog_json());
    const cat_ptr = upix_result_ptr() orelse return error.MissingResult;
    const cat_len = upix_result_len();
    try std.testing.expect(cat_len > 100);
    try std.testing.expect(std.mem.indexOf(u8, cat_ptr[0..cat_len], "Real-ESRGAN") != null);

    try std.testing.expectEqual(@as(i32, 0), upix_sha256_hex("abc".ptr, 3));
    const h_ptr = upix_result_ptr() orelse return error.MissingResult;
    try std.testing.expectEqualStrings(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        h_ptr[0..upix_result_len()],
    );

    try std.testing.expectEqual(@as(i32, -1), upix_model_meta_json(99999));
    try std.testing.expect(upix_last_error_len() > 0);
}
