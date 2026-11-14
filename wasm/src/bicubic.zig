//! Separable 2-pass Catmull-Rom (`a = -0.5`) single-channel `u8` resizer with
//! a 4-row horizontal ring cache (matches `upix-core/src/bicubic.rs`).

const std = @import("std");

inline fn bicubicKernel(t_in: f32) f32 {
    const a: f32 = -0.5;
    const t = @abs(t_in);
    if (t <= 1.0) {
        return (a + 2.0) * t * t * t - (a + 3.0) * t * t + 1.0;
    } else if (t < 2.0) {
        return a * t * t * t - 5.0 * a * t * t + 8.0 * a * t - 4.0 * a;
    } else {
        return 0.0;
    }
}

const HTap = struct {
    c0: usize,
    c1: usize,
    c2: usize,
    c3: usize,
    w0: f32,
    w1: f32,
    w2: f32,
    w3: f32,
};

/// Separable Catmull-Rom bicubic scale for a single channel (`u8` in, `u8` out).
pub fn bicubicScaleU8(
    allocator: std.mem.Allocator,
    src: []const u8,
    src_w: u32,
    src_h: u32,
    dst: []u8,
    dst_w: u32,
    dst_h: u32,
) !void {
    if (src_w == 0 or src_h == 0 or dst_w == 0 or dst_h == 0) return;

    const src_w_usize: usize = @intCast(src_w);
    const src_h_usize: usize = @intCast(src_h);
    const dst_w_usize: usize = @intCast(dst_w);
    const dst_h_usize: usize = @intCast(dst_h);

    const src_needed = std.math.mul(usize, src_w_usize, src_h_usize) catch return;
    const dst_needed = std.math.mul(usize, dst_w_usize, dst_h_usize) catch return;
    if (src.len < src_needed or dst.len < dst_needed) return;

    const scale_x = @as(f32, @floatFromInt(src_w)) / @as(f32, @floatFromInt(dst_w));
    const scale_y = @as(f32, @floatFromInt(src_h)) / @as(f32, @floatFromInt(dst_h));
    const max_sx: i32 = @as(i32, @intCast(src_w)) - 1;
    const max_sy: i32 = @as(i32, @intCast(src_h)) - 1;

    const h_taps = try allocator.alloc(HTap, dst_w_usize);
    defer allocator.free(h_taps);

    for (0..dst_w_usize) |dx| {
        const sx = (@as(f32, @floatFromInt(dx)) + 0.5) * scale_x - 0.5;
        const sx0: i32 = @intFromFloat(@floor(sx));
        const w0 = bicubicKernel(sx - @as(f32, @floatFromInt(sx0 - 1)));
        const w1 = bicubicKernel(sx - @as(f32, @floatFromInt(sx0)));
        const w2 = bicubicKernel(sx - @as(f32, @floatFromInt(sx0 + 1)));
        const w3 = bicubicKernel(sx - @as(f32, @floatFromInt(sx0 + 2)));
        const wsum = w0 + w1 + w2 + w3;
        const inv: f32 = if (@abs(wsum) > 1e-6) 1.0 / wsum else 0.0;
        h_taps[dx] = .{
            .c0 = @intCast(std.math.clamp(sx0 - 1, 0, max_sx)),
            .c1 = @intCast(std.math.clamp(sx0, 0, max_sx)),
            .c2 = @intCast(std.math.clamp(sx0 + 1, 0, max_sx)),
            .c3 = @intCast(std.math.clamp(sx0 + 2, 0, max_sx)),
            .w0 = w0 * inv,
            .w1 = w1 * inv,
            .w2 = w2 * inv,
            .w3 = w3 * inv,
        };
    }

    var cached_cy = [_]i32{ -1, -1, -1, -1 };
    const row_cache = try allocator.alloc(f32, dst_w_usize * 4);
    defer allocator.free(row_cache);
    @memset(row_cache, 0.0);

    for (0..dst_h_usize) |dy| {
        const drow = dst[dy * dst_w_usize .. (dy + 1) * dst_w_usize];
        const sy = (@as(f32, @floatFromInt(dy)) + 0.5) * scale_y - 0.5;
        const sy0: i32 = @intFromFloat(@floor(sy));
        const cys = [4]i32{
            std.math.clamp(sy0 - 1, 0, max_sy),
            std.math.clamp(sy0, 0, max_sy),
            std.math.clamp(sy0 + 1, 0, max_sy),
            std.math.clamp(sy0 + 2, 0, max_sy),
        };

        var wy0 = bicubicKernel(sy - @as(f32, @floatFromInt(sy0 - 1)));
        var wy1 = bicubicKernel(sy - @as(f32, @floatFromInt(sy0)));
        var wy2 = bicubicKernel(sy - @as(f32, @floatFromInt(sy0 + 1)));
        var wy3 = bicubicKernel(sy - @as(f32, @floatFromInt(sy0 + 2)));
        const wysum = wy0 + wy1 + wy2 + wy3;
        if (@abs(wysum) <= 1e-6) {
            @memset(drow, 0);
            continue;
        }
        const inv_wy = 1.0 / wysum;
        wy0 *= inv_wy;
        wy1 *= inv_wy;
        wy2 *= inv_wy;
        wy3 *= inv_wy;

        for (cys) |cy| {
            const cy_usize: usize = @intCast(cy);
            const slot = cy_usize & 3;
            if (cached_cy[slot] != cy) {
                const srow = src[cy_usize * src_w_usize .. (cy_usize + 1) * src_w_usize];
                const cbuf = row_cache[slot * dst_w_usize .. (slot + 1) * dst_w_usize];
                for (cbuf, h_taps) |*out_x, tap| {
                    out_x.* = @as(f32, @floatFromInt(srow[tap.c0])) * tap.w0 +
                        @as(f32, @floatFromInt(srow[tap.c1])) * tap.w1 +
                        @as(f32, @floatFromInt(srow[tap.c2])) * tap.w2 +
                        @as(f32, @floatFromInt(srow[tap.c3])) * tap.w3;
                }
                cached_cy[slot] = cy;
            }
        }

        const s0: usize = @as(usize, @intCast(cys[0])) & 3;
        const s1: usize = @as(usize, @intCast(cys[1])) & 3;
        const s2: usize = @as(usize, @intCast(cys[2])) & 3;
        const s3: usize = @as(usize, @intCast(cys[3])) & 3;
        const r0 = row_cache[s0 * dst_w_usize .. (s0 + 1) * dst_w_usize];
        const r1 = row_cache[s1 * dst_w_usize .. (s1 + 1) * dst_w_usize];
        const r2 = row_cache[s2 * dst_w_usize .. (s2 + 1) * dst_w_usize];
        const r3 = row_cache[s3 * dst_w_usize .. (s3 + 1) * dst_w_usize];

        for (0..dst_w_usize) |dx| {
            const val = r0[dx] * wy0 + r1[dx] * wy1 + r2[dx] * wy2 + r3[dx] * wy3;
            drow[dx] = @intFromFloat(std.math.clamp(val, 0.0, 255.0));
        }
    }
}

test "constant image stays constant" {
    const testing = std.testing;
    const src: [16 * 16]u8 = @splat(137);
    var dst: [32 * 32]u8 = @splat(0);
    try bicubicScaleU8(testing.allocator, &src, 16, 16, &dst, 32, 32);
    for (dst) |v| try testing.expectEqual(@as(u8, 137), v);
}

test "downscale preserves constant" {
    const testing = std.testing;
    const src: [64 * 64]u8 = @splat(42);
    var dst: [16 * 16]u8 = @splat(0);
    try bicubicScaleU8(testing.allocator, &src, 64, 64, &dst, 16, 16);
    for (dst) |v| try testing.expectEqual(@as(u8, 42), v);
}

test "identity ratio is neutral" {
    const testing = std.testing;
    var src: [64]u8 = undefined;
    for (&src, 0..) |*v, i| v.* = @truncate(i * 3);
    var dst: [64]u8 = @splat(0);
    try bicubicScaleU8(testing.allocator, &src, 8, 8, &dst, 8, 8);
    try testing.expectEqualSlices(u8, &src, &dst);
}

test "linear ramp stays monotone" {
    const testing = std.testing;
    var src: [16]u8 = undefined;
    for (&src, 0..) |*v, i| v.* = @intCast(i * 16);
    var dst: [32]u8 = @splat(0);
    try bicubicScaleU8(testing.allocator, &src, 16, 1, &dst, 32, 1);
    for (0..dst.len - 1) |i| {
        try testing.expect(dst[i] <= dst[i + 1]);
    }
}

test "degenerate and zero dimensions do not panic" {
    const testing = std.testing;
    const src = [_]u8{200};
    var dst: [64]u8 = @splat(255);
    try bicubicScaleU8(testing.allocator, &src, 1, 1, &dst, 8, 8);
    try bicubicScaleU8(testing.allocator, &src, 0, 1, &dst, 8, 8);
    try bicubicScaleU8(testing.allocator, &src, 1, 1, &dst, 0, 8);
}

test "separable matches 2d reference within 1 ulp" {
    const testing = std.testing;
    const src_w: u32 = 23;
    const src_h: u32 = 19;
    const dst_w: u32 = 61;
    const dst_h: u32 = 47;
    var src: [src_w * src_h]u8 = undefined;
    for (&src, 0..) |*v, i| v.* = @intCast((i * 73 + 19) % 256);
    var dst_sep: [dst_w * dst_h]u8 = @splat(0);
    try bicubicScaleU8(testing.allocator, &src, src_w, src_h, &dst_sep, dst_w, dst_h);

    const scale_x = @as(f32, @floatFromInt(src_w)) / @as(f32, @floatFromInt(dst_w));
    const scale_y = @as(f32, @floatFromInt(src_h)) / @as(f32, @floatFromInt(dst_h));
    for (0..dst_h) |dy| {
        const sy = (@as(f32, @floatFromInt(dy)) + 0.5) * scale_y - 0.5;
        const sy0: i32 = @intFromFloat(@floor(sy));
        for (0..dst_w) |dx| {
            const sx = (@as(f32, @floatFromInt(dx)) + 0.5) * scale_x - 0.5;
            const sx0: i32 = @intFromFloat(@floor(sx));
            var sum: f32 = 0.0;
            var wsum: f32 = 0.0;
            var m: i32 = -1;
            while (m <= 2) : (m += 1) {
                const cy: usize = @intCast(std.math.clamp(sy0 + m, 0, @as(i32, @intCast(src_h)) - 1));
                const wy = bicubicKernel(sy - @as(f32, @floatFromInt(sy0 + m)));
                var n: i32 = -1;
                while (n <= 2) : (n += 1) {
                    const cx: usize = @intCast(std.math.clamp(sx0 + n, 0, @as(i32, @intCast(src_w)) - 1));
                    const wx = bicubicKernel(sx - @as(f32, @floatFromInt(sx0 + n)));
                    const w = wx * wy;
                    sum += @as(f32, @floatFromInt(src[cy * src_w + cx])) * w;
                    wsum += w;
                }
            }
            const norm = if (@abs(wsum) > 1e-6) sum / wsum else 0.0;
            const ref_val: u8 = @intFromFloat(std.math.clamp(norm, 0.0, 255.0));
            const sep_val = dst_sep[dy * dst_w + dx];
            const diff = if (sep_val >= ref_val) sep_val - ref_val else ref_val - sep_val;
            try testing.expect(diff <= 1);
        }
    }
}
