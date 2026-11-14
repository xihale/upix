//! Tile layout, u8->f32 tile extraction, quadratic w*w edge-fade blending,
//! and blend finalization (matches `upix-core/src/tile_processor.rs` and
//! `upix-wasm/src/lib.rs`).

const std = @import("std");

pub const TileRegion = struct {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    overlap: u32,
};

/// Compute tile grid over an image of `(image_width, image_height)`.
/// Overlap is clamped to `[0, tile_size - 1]` and step to `>= 1`.
pub fn computeTileLayout(
    allocator: std.mem.Allocator,
    image_width: i32,
    image_height: i32,
    tile: i32,
    overlap: i32,
) ![]TileRegion {
    const tile_size = @max(tile, 1);
    const ov = std.math.clamp(overlap, 0, tile_size - 1);
    const step = @max(tile_size - ov, 1);

    var starts_x: std.ArrayList(i32) = .empty;
    defer starts_x.deinit(allocator);
    var starts_y: std.ArrayList(i32) = .empty;
    defer starts_y.deinit(allocator);

    var x: i32 = 0;
    while (x < image_width) {
        try starts_x.append(allocator, x);
        if (x + tile_size >= image_width) break;
        x += step;
    }
    var y: i32 = 0;
    while (y < image_height) {
        try starts_y.append(allocator, y);
        if (y + tile_size >= image_height) break;
        y += step;
    }
    if (starts_x.items.len == 0) try starts_x.append(allocator, 0);
    if (starts_y.items.len == 0) try starts_y.append(allocator, 0);

    const total = starts_x.items.len * starts_y.items.len;
    const regions = try allocator.alloc(TileRegion, total);
    var idx: usize = 0;
    for (starts_y.items) |sy| {
        for (starts_x.items) |sx| {
            regions[idx] = .{
                .x = @intCast(@max(0, sx)),
                .y = @intCast(@max(0, sy)),
                .width = @intCast(@max(0, @min(tile_size, image_width - sx))),
                .height = @intCast(@max(0, @min(tile_size, image_height - sy))),
                .overlap = @intCast(ov),
            };
            idx += 1;
        }
    }
    return regions;
}

/// Serialize `computeTileLayout` as JSON array for `upix_compute_tile_layout_json`.
/// Caller owns the returned slice.
pub fn computeTileLayoutJson(
    allocator: std.mem.Allocator,
    w: i32,
    h: i32,
    tile: i32,
    overlap: i32,
) ![]u8 {
    const regions = try computeTileLayout(allocator, w, h, tile, overlap);
    defer allocator.free(regions);

    var out: std.Io.Writer.Allocating = .init(allocator);
    errdefer out.deinit();
    var ws: std.json.Stringify = .{ .writer = &out.writer };

    try ws.beginArray();
    for (regions) |r| {
        try ws.beginObject();
        try ws.objectField("x");
        try ws.write(r.x);
        try ws.objectField("y");
        try ws.write(r.y);
        try ws.objectField("width");
        try ws.write(r.width);
        try ws.objectField("height");
        try ws.write(r.height);
        try ws.objectField("overlap");
        try ws.write(r.overlap);
        try ws.endObject();
    }
    try ws.endArray();
    return out.toOwnedSlice();
}

/// Extract one interleaved `u8` tile into normalized `0..1` `f32` buffer `out_f32`.
/// `src_channels` is the source pixel stride, `dst_channels` is the tile stride
/// (`dst_channels <= src_channels`, e.g. RGBA -> RGB).
pub fn extractTileU8(
    image_data: []const u8,
    image_width: i32,
    src_channels: i32,
    dst_channels: i32,
    region: TileRegion,
    out_f32: []f32,
) void {
    if (image_width <= 0 or src_channels <= 0 or dst_channels <= 0) return;
    const inv255: f32 = 1.0 / 255.0;
    const img_w: usize = @intCast(image_width);
    const src_c: usize = @intCast(src_channels);
    const dst_c: usize = @intCast(dst_channels);
    const rw: usize = @intCast(region.width);
    const rh: usize = @intCast(region.height);
    if (out_f32.len < rw * rh * dst_c) return;

    const copy_c = @min(src_c, dst_c);
    for (0..rh) |y| {
        const src_idx = ((@as(usize, region.y) + y) * img_w + @as(usize, region.x)) * src_c;
        const dst_idx = y * rw * dst_c;
        if (src_c == dst_c) {
            const cw = rw * dst_c;
            for (0..cw) |c| {
                out_f32[dst_idx + c] = @as(f32, @floatFromInt(image_data[src_idx + c])) * inv255;
            }
        } else {
            for (0..rw) |x| {
                const s = src_idx + x * src_c;
                const d = dst_idx + x * dst_c;
                for (0..copy_c) |c| {
                    out_f32[d + c] = @as(f32, @floatFromInt(image_data[s + c])) * inv255;
                }
                if (dst_c > copy_c) {
                    @memset(out_f32[d + copy_c .. d + dst_c], 0.0);
                }
            }
        }
    }
}

/// Allocating convenience wrapper around `extractTileU8`.
pub fn extractTileU8Alloc(
    allocator: std.mem.Allocator,
    image_data: []const u8,
    image_width: i32,
    src_channels: i32,
    dst_channels: i32,
    region: TileRegion,
) ![]f32 {
    const dst_c: usize = @intCast(@max(dst_channels, 0));
    const len = @as(usize, region.width) * @as(usize, region.height) * dst_c;
    const buf = try allocator.alloc(f32, len);
    extractTileU8(image_data, image_width, src_channels, dst_channels, region, buf);
    return buf;
}

/// Blend one processed (scaled) tile into the float accumulator with `w * w`
/// quadratic edge fade.
pub fn blendTile(
    allocator: std.mem.Allocator,
    tile_data: []const f32,
    output_data: []f32,
    weight_data: []f32,
    output_width: i32,
    output_height: i32,
    channels: i32,
    region: TileRegion,
    scale_factor: i32,
    overlap: i32,
) !void {
    if (output_width <= 0 or output_height <= 0 or channels <= 0) return;
    const scale_i = @max(scale_factor, 1);
    const scale_u: u32 = @intCast(scale_i);
    const out_x = region.x * scale_u;
    const out_y = region.y * scale_u;
    const out_w = region.width * scale_u;
    const out_h = region.height * scale_u;
    const out_w_usize: usize = @intCast(out_w);
    const out_h_usize: usize = @intCast(out_h);
    const out_img_w: usize = @intCast(output_width);
    const out_img_h: usize = @intCast(output_height);
    const ch_usize: usize = @intCast(channels);

    if (out_w_usize == 0 or out_h_usize == 0) return;

    const scaled_overlap = overlap * scale_i;
    const has_left_neighbor = out_x > 0;
    const has_top_neighbor = out_y > 0;
    const has_right_neighbor = out_x + out_w < @as(u32, @intCast(out_img_w));
    const has_bottom_neighbor = out_y + out_h < @as(u32, @intCast(out_img_h));

    if (scaled_overlap <= 0) {
        for (0..out_h_usize) |y| {
            const row_pixel = (@as(usize, out_y) + y) * out_img_w + @as(usize, out_x);
            const src_row = tile_data[y * out_w_usize * ch_usize .. (y + 1) * out_w_usize * ch_usize];
            const dst_row = output_data[row_pixel * ch_usize .. (row_pixel + out_w_usize) * ch_usize];
            const w_row = weight_data[row_pixel .. row_pixel + out_w_usize];
            if (ch_usize == 3) {
                for (0..out_w_usize) |x| {
                    const b = x * 3;
                    dst_row[b] += src_row[b];
                    dst_row[b + 1] += src_row[b + 1];
                    dst_row[b + 2] += src_row[b + 2];
                    w_row[x] += 1.0;
                }
            } else {
                for (0..out_w_usize) |x| {
                    const b = x * ch_usize;
                    for (0..ch_usize) |c| {
                        dst_row[b + c] += src_row[b + c];
                    }
                    w_row[x] += 1.0;
                }
            }
        }
        return;
    }

    const overlap_x: u32 = @min(@as(u32, @intCast(@max(scaled_overlap, 0))), out_w);
    const overlap_y: u32 = @min(@as(u32, @intCast(@max(scaled_overlap, 0))), out_h);
    const inv_ov_x: f32 = if (overlap_x > 0) 1.0 / @as(f32, @floatFromInt(overlap_x)) else 0.0;
    const inv_ov_y: f32 = if (overlap_y > 0) 1.0 / @as(f32, @floatFromInt(overlap_y)) else 0.0;

    const x_weight = try allocator.alloc(f32, out_w_usize);
    defer allocator.free(x_weight);
    const y_weight = try allocator.alloc(f32, out_h_usize);
    defer allocator.free(y_weight);

    for (0..out_w_usize) |x| {
        const xu: u32 = @intCast(x);
        var w: f32 = 1.0;
        if (overlap_x > 0) {
            if (has_left_neighbor and xu < overlap_x) {
                w = @as(f32, @floatFromInt(xu + 1)) * inv_ov_x;
            } else if (has_right_neighbor and xu >= out_w - overlap_x) {
                w = @as(f32, @floatFromInt(out_w - xu)) * inv_ov_x;
            }
        }
        x_weight[x] = w * w;
    }

    for (0..out_h_usize) |y| {
        const yu: u32 = @intCast(y);
        var w: f32 = 1.0;
        if (overlap_y > 0) {
            if (has_top_neighbor and yu < overlap_y) {
                w = @as(f32, @floatFromInt(yu + 1)) * inv_ov_y;
            } else if (has_bottom_neighbor and yu >= out_h - overlap_y) {
                w = @as(f32, @floatFromInt(out_h - yu)) * inv_ov_y;
            }
        }
        y_weight[y] = w * w;
    }

    for (0..out_h_usize) |y| {
        const yw = y_weight[y];
        const row_pixel = (@as(usize, out_y) + y) * out_img_w + @as(usize, out_x);
        const src_row = tile_data[y * out_w_usize * ch_usize .. (y + 1) * out_w_usize * ch_usize];
        const dst_row = output_data[row_pixel * ch_usize .. (row_pixel + out_w_usize) * ch_usize];
        const w_row = weight_data[row_pixel .. row_pixel + out_w_usize];
        if (ch_usize == 3) {
            for (0..out_w_usize) |x| {
                const weight = x_weight[x] * yw;
                const b = x * 3;
                dst_row[b] += src_row[b] * weight;
                dst_row[b + 1] += src_row[b + 1] * weight;
                dst_row[b + 2] += src_row[b + 2] * weight;
                w_row[x] += weight;
            }
        } else {
            for (0..out_w_usize) |x| {
                const weight = x_weight[x] * yw;
                const b = x * ch_usize;
                for (0..ch_usize) |c| {
                    dst_row[b + c] += src_row[b + c] * weight;
                }
                w_row[x] += weight;
            }
        }
    }
}

inline fn clamp255ToU8(v: f32) u8 {
    if (v >= 0.0) {
        const clamped = if (v <= 255.0) v else 255.0;
        return @intFromFloat(clamped + 0.5);
    }
    return 0;
}

/// Normalize accumulated `f32` pixels by `weights`, quantize to `u8`, and
/// optionally merge a separately-scaled alpha plane.
pub fn finalizeBlendU8(
    accum: []const f32,
    weights: []const f32,
    alpha_plane: ?[]const u8,
    out_u8: []u8,
    pixel_count: usize,
    model_channels: usize,
    out_channels: usize,
) void {
    if (pixel_count == 0 or model_channels == 0 or out_channels == 0) return;

    if (model_channels == 3 and out_channels == 3) {
        for (0..pixel_count) |p| {
            const w = weights[p];
            const scale: f32 = if (w > 1e-6) 255.0 / w else 0.0;
            const b = p * 3;
            out_u8[b] = clamp255ToU8(accum[b] * scale);
            out_u8[b + 1] = clamp255ToU8(accum[b + 1] * scale);
            out_u8[b + 2] = clamp255ToU8(accum[b + 2] * scale);
        }
        return;
    }

    if (model_channels == 3 and out_channels == 4) {
        if (alpha_plane) |alpha| {
            for (0..pixel_count) |p| {
                const w = weights[p];
                const scale: f32 = if (w > 1e-6) 255.0 / w else 0.0;
                const sb = p * 3;
                const db = p * 4;
                out_u8[db] = clamp255ToU8(accum[sb] * scale);
                out_u8[db + 1] = clamp255ToU8(accum[sb + 1] * scale);
                out_u8[db + 2] = clamp255ToU8(accum[sb + 2] * scale);
                out_u8[db + 3] = alpha[p];
            }
        } else {
            for (0..pixel_count) |p| {
                const w = weights[p];
                const scale: f32 = if (w > 1e-6) 255.0 / w else 0.0;
                const sb = p * 3;
                const db = p * 4;
                out_u8[db] = clamp255ToU8(accum[sb] * scale);
                out_u8[db + 1] = clamp255ToU8(accum[sb + 1] * scale);
                out_u8[db + 2] = clamp255ToU8(accum[sb + 2] * scale);
                out_u8[db + 3] = 255;
            }
        }
        return;
    }

    for (0..pixel_count) |p| {
        const w = weights[p];
        const inv_w: f32 = if (w > 1e-6) 1.0 / w else 0.0;
        const s_base = p * model_channels;
        const d_base = p * out_channels;
        for (0..model_channels) |c| {
            const raw = accum[s_base + c] * inv_w;
            const v: f32 = if (raw >= 0.0) (if (raw <= 1.0) raw else 1.0) else 0.0;
            out_u8[d_base + c] = @intFromFloat(v * 255.0 + 0.5);
        }
        if (out_channels == 4 and model_channels == 3) {
            out_u8[d_base + 3] = if (alpha_plane) |a| a[p] else 255;
        }
    }
}

test "computeTileLayout covers image and clamps trailing tiles" {
    const testing = std.testing;
    const regions = try computeTileLayout(testing.allocator, 100, 80, 64, 16);
    defer testing.allocator.free(regions);

    // step = 48: x starts 0, 48 (48+64=112 >= 100 -> stop); y starts 0, 48
    try testing.expectEqual(@as(usize, 4), regions.len);
    try testing.expectEqual(TileRegion{ .x = 0, .y = 0, .width = 64, .height = 64, .overlap = 16 }, regions[0]);
    try testing.expectEqual(TileRegion{ .x = 48, .y = 0, .width = 52, .height = 64, .overlap = 16 }, regions[1]);
    try testing.expectEqual(TileRegion{ .x = 0, .y = 48, .width = 64, .height = 32, .overlap = 16 }, regions[2]);
    try testing.expectEqual(TileRegion{ .x = 48, .y = 48, .width = 52, .height = 32, .overlap = 16 }, regions[3]);

    const json = try computeTileLayoutJson(testing.allocator, 100, 80, 64, 16);
    defer testing.allocator.free(json);
    var parsed = try std.json.parseFromSlice([]TileRegion, testing.allocator, json, .{});
    defer parsed.deinit();
    try testing.expectEqual(@as(usize, 4), parsed.value.len);
    try testing.expectEqual(regions[1], parsed.value[1]);
}

test "extractTileU8 deinterleaves RGBA to RGB" {
    const testing = std.testing;
    // 4x2 RGBA image
    var img: [4 * 2 * 4]u8 = undefined;
    for (0..8) |p| {
        img[p * 4 + 0] = @intCast(p * 10);
        img[p * 4 + 1] = @intCast(p * 20);
        img[p * 4 + 2] = @intCast(p * 30);
        img[p * 4 + 3] = 255;
    }
    var out: [2 * 2 * 3]f32 = undefined;
    const reg = TileRegion{ .x = 1, .y = 0, .width = 2, .height = 2, .overlap = 0 };
    extractTileU8(&img, 4, 4, 3, reg, &out);

    // Pixel (1,0) is index 1 -> (10, 20, 30)
    try testing.expectApproxEqAbs(@as(f32, 10.0 / 255.0), out[0], 1e-6);
    try testing.expectApproxEqAbs(@as(f32, 20.0 / 255.0), out[1], 1e-6);
    try testing.expectApproxEqAbs(@as(f32, 30.0 / 255.0), out[2], 1e-6);
    // Pixel (1,1) is index 5 -> (50, 100, 150)
    try testing.expectApproxEqAbs(@as(f32, 50.0 / 255.0), out[6], 1e-6);
    try testing.expectApproxEqAbs(@as(f32, 100.0 / 255.0), out[7], 1e-6);
    try testing.expectApproxEqAbs(@as(f32, 150.0 / 255.0), out[8], 1e-6);
}

test "blendTile and finalizeBlendU8 reconstruct constant image with overlap" {
    const testing = std.testing;
    const w: i32 = 16;
    const h: i32 = 8;
    const scale: i32 = 2;
    const out_w = w * scale;
    const out_h = h * scale;
    const out_px: usize = @intCast(out_w * out_h);

    const regions = try computeTileLayout(testing.allocator, w, h, 10, 4);
    defer testing.allocator.free(regions);

    var img: [16 * 8 * 4]u8 = undefined;
    for (0..16 * 8) |p| {
        img[p * 4 + 0] = 64;
        img[p * 4 + 1] = 128;
        img[p * 4 + 2] = 200;
        img[p * 4 + 3] = 77;
    }

    const accum = try testing.allocator.alloc(f32, out_px * 3);
    defer testing.allocator.free(accum);
    @memset(accum, 0.0);
    const weights = try testing.allocator.alloc(f32, out_px);
    defer testing.allocator.free(weights);
    @memset(weights, 0.0);

    for (regions) |reg| {
        const tile_in = try extractTileU8Alloc(testing.allocator, &img, w, 4, 3, reg);
        defer testing.allocator.free(tile_in);

        // Simulate 2x nearest upscale
        const tw_out = reg.width * @as(u32, @intCast(scale));
        const th_out = reg.height * @as(u32, @intCast(scale));
        const tile_out = try testing.allocator.alloc(f32, tw_out * th_out * 3);
        defer testing.allocator.free(tile_out);
        for (0..th_out) |oy| {
            for (0..tw_out) |ox| {
                const sx = ox / 2;
                const sy = oy / 2;
                const si = (sy * reg.width + sx) * 3;
                const di = (oy * tw_out + ox) * 3;
                @memcpy(tile_out[di .. di + 3], tile_in[si .. si + 3]);
            }
        }
        try blendTile(testing.allocator, tile_out, accum, weights, out_w, out_h, 3, reg, scale, 4);
    }

    const alpha = try testing.allocator.alloc(u8, out_px);
    defer testing.allocator.free(alpha);
    @memset(alpha, 77);
    const out_u8 = try testing.allocator.alloc(u8, out_px * 4);
    defer testing.allocator.free(out_u8);

    finalizeBlendU8(accum, weights, alpha, out_u8, out_px, 3, 4);
    for (0..out_px) |p| {
        try testing.expectEqual(@as(u8, 64), out_u8[p * 4 + 0]);
        try testing.expectEqual(@as(u8, 128), out_u8[p * 4 + 1]);
        try testing.expectEqual(@as(u8, 200), out_u8[p * 4 + 2]);
        try testing.expectEqual(@as(u8, 77), out_u8[p * 4 + 3]);
    }
}
