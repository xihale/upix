//! Pure-Zig CPU inference engine for ncnn graphs (`ncnn.NcnnGraph`),
//! featuring `@Vector(4, f32)` SIMD microkernels (lowering to WASM `v128` under
//! `+simd128` and scalarizing cleanly on baseline `wasm32-freestanding`),
//! `im2col` + tiled 6x8 GEMM, conv/deconv activation epilogue fusion, and
//! reference-counted tensor buffer pooling (`Workspace`).

const std = @import("std");
const ncnn = @import("ncnn.zig");
const tile = @import("tile.zig");
const bicubic = @import("bicubic.zig");

const Vec4 = @Vector(4, f32);

pub const EngineError = error{
    NotInitialized,
    NoModelLoaded,
    InvalidDimensions,
    InputBufferTooSmall,
    OutputBufferTooSmall,
    InvalidGraph,
    UnsupportedLayer,
    OutOfMemory,
};

inline fn loadVec4(ptr: [*]const f32) Vec4 {
    return @as(*align(@alignOf(f32)) const Vec4, @ptrCast(ptr)).*;
}

inline fn storeVec4(ptr: [*]f32, v: Vec4) void {
    @as(*align(@alignOf(f32)) Vec4, @ptrCast(ptr)).* = v;
}

const Tensor = struct {
    w: usize,
    h: usize,
    c: usize,
    backing: []f32,
    len: usize,

    inline fn data(self: Tensor) []f32 {
        return self.backing[0..self.len];
    }

    inline fn dataConst(self: Tensor) []const f32 {
        return self.backing[0..self.len];
    }

    inline fn planeSize(self: Tensor) usize {
        return self.w * self.h;
    }

    inline fn channel(self: Tensor, ch: usize) []const f32 {
        const ps = self.w * self.h;
        return self.backing[ch * ps .. (ch + 1) * ps];
    }

    inline fn channelMut(self: Tensor, ch: usize) []f32 {
        const ps = self.w * self.h;
        return self.backing[ch * ps .. (ch + 1) * ps];
    }
};

const Workspace = struct {
    allocator: std.mem.Allocator,
    free_buffers: std.ArrayList([]f32),
    col_buf: []f32,
    b_pack: []f32,
    weight_perm_buf: []f32,

    fn init(allocator: std.mem.Allocator) Workspace {
        return .{
            .allocator = allocator,
            .free_buffers = .empty,
            .col_buf = &.{},
            .b_pack = &.{},
            .weight_perm_buf = &.{},
        };
    }

    fn deinit(self: *Workspace) void {
        for (self.free_buffers.items) |buf| {
            self.allocator.free(buf);
        }
        self.free_buffers.deinit(self.allocator);
        if (self.col_buf.len > 0) self.allocator.free(self.col_buf);
        if (self.b_pack.len > 0) self.allocator.free(self.b_pack);
        if (self.weight_perm_buf.len > 0) self.allocator.free(self.weight_perm_buf);
        self.* = undefined;
    }

    fn ensureScratch(self: *Workspace, buf: *[]f32, needed: usize) ![]f32 {
        if (buf.len < needed) {
            if (buf.len > 0) self.allocator.free(buf.*);
            buf.* = try self.allocator.alloc(f32, needed);
        }
        return buf.*[0..needed];
    }

    fn allocBuf(self: *Workspace, len: usize, fill: ?f32) ![]f32 {
        if (len == 0) return &.{};
        var best_idx: ?usize = null;
        var best_cap: usize = std.math.maxInt(usize);
        const max_acceptable = @max(len *| 4, 1024);
        for (self.free_buffers.items, 0..) |buf, i| {
            const cap = buf.len;
            if (cap >= len and cap <= max_acceptable and cap < best_cap) {
                best_cap = cap;
                best_idx = i;
            }
        }

        const backing: []f32 = if (best_idx) |idx|
            self.free_buffers.swapRemove(idx)
        else if (self.free_buffers.pop()) |any| blk: {
            if (any.len >= len) break :blk any;
            const grown = self.allocator.realloc(any, len) catch |err| {
                self.allocator.free(any);
                return err;
            };
            break :blk grown;
        } else try self.allocator.alloc(f32, len);

        if (fill) |val| {
            @memset(backing[0..len], val);
        }
        return backing;
    }

    fn allocTensor(self: *Workspace, w: usize, h: usize, c: usize, fill: ?f32) !Tensor {
        const len = w * h * c;
        const backing = try self.allocBuf(len, fill);
        return .{
            .w = w,
            .h = h,
            .c = c,
            .backing = backing,
            .len = len,
        };
    }

    fn recycle(self: *Workspace, t: Tensor) void {
        if (t.backing.len == 0) return;
        if (self.free_buffers.items.len < 40) {
            self.free_buffers.append(self.allocator, t.backing) catch {
                self.allocator.free(t.backing);
            };
        } else {
            self.allocator.free(t.backing);
        }
    }
};

// ---------------------------------------------------------------------------
// Activations & epilogue descriptors
// ---------------------------------------------------------------------------

inline fn applyActivationScalar(v: f32, act_type: i32, params: []const f32) f32 {
    return switch (act_type) {
        0 => v,
        1 => @max(v, 0.0),
        2 => blk: {
            const slope = if (params.len > 0) params[0] else 0.1;
            break :blk if (v > 0.0) v else v * slope;
        },
        3 => blk: {
            const min_v = if (params.len > 0) params[0] else 0.0;
            const max_v = if (params.len > 1) params[1] else 1.0;
            break :blk @min(max_v, @max(min_v, v));
        },
        4 => 1.0 / (1.0 + @exp(-v)),
        5 => blk: {
            const sp = if (v > 20.0)
                v
            else if (v < -20.0)
                @exp(v)
            else
                @log(1.0 + @exp(v));
            break :blk v * std.math.tanh(sp);
        },
        6 => blk: {
            const alpha = if (params.len > 0) params[0] else 1.0 / 6.0;
            const beta = if (params.len > 1) params[1] else 0.5;
            break :blk v * std.math.clamp(v * alpha + beta, 0.0, 1.0);
        },
        else => v,
    };
}

inline fn applyLeakyReluSlice(data: []f32, slope: f32) void {
    const n = data.len;
    const ptr = data.ptr;
    const vzero: Vec4 = @splat(0.0);
    var i: usize = 0;

    if (slope == 0.0) {
        while (i + 8 <= n) : (i += 8) {
            const x0 = loadVec4(ptr + i);
            const x1 = loadVec4(ptr + i + 4);
            storeVec4(ptr + i, @max(vzero, x0));
            storeVec4(ptr + i + 4, @max(vzero, x1));
        }
        while (i < n) : (i += 1) {
            ptr[i] = @max(ptr[i], 0.0);
        }
    } else {
        const vslope: Vec4 = @splat(slope);
        while (i + 8 <= n) : (i += 8) {
            const x0 = loadVec4(ptr + i);
            const x1 = loadVec4(ptr + i + 4);
            const y0 = @select(f32, x0 < vzero, x0 * vslope, x0);
            const y1 = @select(f32, x1 < vzero, x1 * vslope, x1);
            storeVec4(ptr + i, y0);
            storeVec4(ptr + i + 4, y1);
        }
        while (i < n) : (i += 1) {
            const v = ptr[i];
            ptr[i] = if (v < 0.0) v * slope else v;
        }
    }
}

inline fn applyClipSlice(data: []f32, min_val: f32, max_val: f32) void {
    const n = data.len;
    const ptr = data.ptr;
    const vmin: Vec4 = @splat(min_val);
    const vmax: Vec4 = @splat(max_val);
    var i: usize = 0;
    while (i + 8 <= n) : (i += 8) {
        const x0 = loadVec4(ptr + i);
        const x1 = loadVec4(ptr + i + 4);
        storeVec4(ptr + i, @min(vmax, @max(vmin, x0)));
        storeVec4(ptr + i + 4, @min(vmax, @max(vmin, x1)));
    }
    while (i < n) : (i += 1) {
        ptr[i] = @min(max_val, @max(min_val, ptr[i]));
    }
}

const ActKind = enum {
    none,
    relu,
    leaky,
    clip,
    sigmoid,
    mish,
    hard_sig,
    prelu_chan,
    swish,
};

const ActSpec = struct {
    kind: ActKind,
    p0: f32,
    p1: f32,
    slopes: []const f32,

    const empty: ActSpec = .{
        .kind = .none,
        .p0 = 0.0,
        .p1 = 0.0,
        .slopes = &.{},
    };

    fn fromNcnn(act_type: i32, params: []const f32) ActSpec {
        return switch (act_type) {
            1 => .{ .kind = .relu, .p0 = 0.0, .p1 = 0.0, .slopes = &.{} },
            2 => .{
                .kind = .leaky,
                .p0 = if (params.len > 0) params[0] else 0.1,
                .p1 = 0.0,
                .slopes = &.{},
            },
            3 => .{
                .kind = .clip,
                .p0 = if (params.len > 0) params[0] else 0.0,
                .p1 = if (params.len > 1) params[1] else 1.0,
                .slopes = &.{},
            },
            4 => .{ .kind = .sigmoid, .p0 = 0.0, .p1 = 0.0, .slopes = &.{} },
            5 => .{ .kind = .mish, .p0 = 0.0, .p1 = 0.0, .slopes = &.{} },
            6 => .{
                .kind = .hard_sig,
                .p0 = if (params.len > 0) params[0] else 1.0 / 6.0,
                .p1 = if (params.len > 1) params[1] else 0.5,
                .slopes = &.{},
            },
            else => .empty,
        };
    }
};

inline fn applySpecScalar(v: f32, spec: ActSpec) f32 {
    return switch (spec.kind) {
        .none, .prelu_chan => v,
        .swish => v / (1.0 + @exp(-v)),
        .relu => @max(v, 0.0),
        .leaky => if (v > 0.0) v else v * spec.p0,
        .clip => @min(spec.p1, @max(spec.p0, v)),
        .sigmoid => 1.0 / (1.0 + @exp(-v)),
        .mish => applyActivationScalar(v, 5, &.{ spec.p0, spec.p1 }),
        .hard_sig => applyActivationScalar(v, 6, &.{ spec.p0, spec.p1 }),
    };
}

fn applyActSlice(data: []f32, spec: ActSpec, channels: usize, plane: usize) void {
    switch (spec.kind) {
        .none => {},
        .relu => applyLeakyReluSlice(data, 0.0),
        .leaky => applyLeakyReluSlice(data, spec.p0),
        .clip => applyClipSlice(data, spec.p0, spec.p1),
        .prelu_chan => {
            if (channels <= 1) {
                const s = if (spec.slopes.len > 0) spec.slopes[0] else 0.0;
                applyLeakyReluSlice(data, s);
            } else {
                const ch = @min(channels, data.len / @max(plane, 1));
                for (0..ch) |c| {
                    const s = if (c < spec.slopes.len) spec.slopes[c] else 0.0;
                    applyLeakyReluSlice(data[c * plane .. (c + 1) * plane], s);
                }
            }
        },
        else => {
            for (data) |*v| {
                v.* = applySpecScalar(v.*, spec);
            }
        },
    }
}

inline fn slopeGroup(slopes: []const f32, g: usize, out_g: usize) []const f32 {
    const start = @min(g * out_g, slopes.len);
    const end = @min((g + 1) * out_g, slopes.len);
    return slopes[start..end];
}

const Epi = union(enum) {
    none: void,
    relu: void,
    leaky: f32,
    clip: struct { min: f32, max: f32 },
    prelu: []const f32,

    inline fn handles(spec: ActSpec) bool {
        return switch (spec.kind) {
            .none, .relu, .leaky, .clip, .prelu_chan => true,
            else => false,
        };
    }

    inline fn fromSpec(spec: ActSpec) Epi {
        return switch (spec.kind) {
            .none => .{ .none = {} },
            .relu => .{ .relu = {} },
            .leaky => .{ .leaky = spec.p0 },
            .clip => .{ .clip = .{ .min = spec.p0, .max = spec.p1 } },
            .prelu_chan => .{ .prelu = spec.slopes },
            else => .{ .none = {} },
        };
    }

    inline fn applyRow(self: Epi, row_slice: []f32, m: usize) void {
        switch (self) {
            .none => {},
            .relu => applyLeakyReluSlice(row_slice, 0.0),
            .leaky => |s| applyLeakyReluSlice(row_slice, s),
            .clip => |c| applyClipSlice(row_slice, c.min, c.max),
            .prelu => |slopes| {
                const s = if (m < slopes.len) slopes[m] else 0.0;
                applyLeakyReluSlice(row_slice, s);
            },
        }
    }
};

// ---------------------------------------------------------------------------
// Vectorized elementwise helpers
// ---------------------------------------------------------------------------

const ZipOp = enum {
    add,
    sub,
    mul,

    inline fn fromBinaryOp(op_type: i32) ?ZipOp {
        return switch (op_type) {
            0 => .add,
            1 => .sub,
            2 => .mul,
            else => null,
        };
    }
};

inline fn zip3Slice(dst: []f32, a: []const f32, b: []const f32, op: ZipOp) void {
    const n = dst.len;
    const dp = dst.ptr;
    const ap = a.ptr;
    const bp = b.ptr;
    var i: usize = 0;
    while (i + 8 <= n) : (i += 8) {
        const a0 = loadVec4(ap + i);
        const a1 = loadVec4(ap + i + 4);
        const b0 = loadVec4(bp + i);
        const b1 = loadVec4(bp + i + 4);
        const r0, const r1 = switch (op) {
            .add => .{ a0 + b0, a1 + b1 },
            .sub => .{ a0 - b0, a1 - b1 },
            .mul => .{ a0 * b0, a1 * b1 },
        };
        storeVec4(dp + i, r0);
        storeVec4(dp + i + 4, r1);
    }
    while (i < n) : (i += 1) {
        dp[i] = switch (op) {
            .add => ap[i] + bp[i],
            .sub => ap[i] - bp[i],
            .mul => ap[i] * bp[i],
        };
    }
}

inline fn zipScalarSlice(dst: []f32, src: []const f32, op: ZipOp, scalar: f32) void {
    const n = dst.len;
    const dp = dst.ptr;
    const sp = src.ptr;
    const vs: Vec4 = @splat(scalar);
    var i: usize = 0;
    while (i + 8 <= n) : (i += 8) {
        const s0 = loadVec4(sp + i);
        const s1 = loadVec4(sp + i + 4);
        const r0, const r1 = switch (op) {
            .add => .{ s0 + vs, s1 + vs },
            .sub => .{ s0 - vs, s1 - vs },
            .mul => .{ s0 * vs, s1 * vs },
        };
        storeVec4(dp + i, r0);
        storeVec4(dp + i + 4, r1);
    }
    while (i < n) : (i += 1) {
        dp[i] = switch (op) {
            .add => sp[i] + scalar,
            .sub => sp[i] - scalar,
            .mul => sp[i] * scalar,
        };
    }
}

inline fn zip2Slice(dst: []f32, src: []const f32, op: ZipOp) void {
    const n = dst.len;
    const dp = dst.ptr;
    const sp = src.ptr;
    var i: usize = 0;
    while (i + 8 <= n) : (i += 8) {
        const d0 = loadVec4(dp + i);
        const d1 = loadVec4(dp + i + 4);
        const s0 = loadVec4(sp + i);
        const s1 = loadVec4(sp + i + 4);
        const r0, const r1 = switch (op) {
            .add => .{ d0 + s0, d1 + s1 },
            .sub => .{ d0 - s0, d1 - s1 },
            .mul => .{ d0 * s0, d1 * s1 },
        };
        storeVec4(dp + i, r0);
        storeVec4(dp + i + 4, r1);
    }
    while (i < n) : (i += 1) {
        dp[i] = switch (op) {
            .add => dp[i] + sp[i],
            .sub => dp[i] - sp[i],
            .mul => dp[i] * sp[i],
        };
    }
}

inline fn saxpySlice(dst: []f32, src: []const f32, alpha: f32) void {
    const n = dst.len;
    const dp = dst.ptr;
    const sp = src.ptr;
    const va: Vec4 = @splat(alpha);
    var j: usize = 0;
    while (j + 8 <= n) : (j += 8) {
        const d0 = loadVec4(dp + j);
        const d1 = loadVec4(dp + j + 4);
        const s0 = loadVec4(sp + j);
        const s1 = loadVec4(sp + j + 4);
        storeVec4(dp + j, d0 + va * s0);
        storeVec4(dp + j + 4, d1 + va * s1);
    }
    while (j < n) : (j += 1) {
        dp[j] += alpha * sp[j];
    }
}

// ---------------------------------------------------------------------------
// Packed row-major BLIS-style GEMM with 6x8 SIMD microkernel
// ---------------------------------------------------------------------------

const GEMM_KC: usize = 128;
const GEMM_NC: usize = 512;
const GEMM_NR: usize = 8;

fn packBPanels(
    ws: *Workspace,
    b: []const f32,
    n_dim: usize,
    kc0: usize,
    kc_len: usize,
    jc0: usize,
    nc_len: usize,
) ![]f32 {
    const num_panels = std.math.divCeil(usize, nc_len, GEMM_NR) catch unreachable;
    const needed = num_panels * kc_len * GEMM_NR;
    const b_pack = try ws.ensureScratch(&ws.b_pack, needed);
    const full_panels = nc_len / GEMM_NR;

    const b_ptr = b.ptr;
    var dst_ptr = b_pack.ptr;
    for (0..full_panels) |p| {
        const col = jc0 + p * GEMM_NR;
        for (0..kc_len) |k| {
            const src = b_ptr + (kc0 + k) * n_dim + col;
            storeVec4(dst_ptr, loadVec4(src));
            storeVec4(dst_ptr + 4, loadVec4(src + 4));
            dst_ptr += GEMM_NR;
        }
    }

    const rem = nc_len - full_panels * GEMM_NR;
    if (rem > 0) {
        const col = jc0 + full_panels * GEMM_NR;
        const panel_dst = b_pack[full_panels * kc_len * GEMM_NR .. needed];
        for (0..kc_len) |k| {
            const src_off = (kc0 + k) * n_dim + col;
            const dst_row = panel_dst[k * GEMM_NR .. (k + 1) * GEMM_NR];
            @memcpy(dst_row[0..rem], b[src_off .. src_off + rem]);
            @memset(dst_row[rem..], 0.0);
        }
    }
    return b_pack;
}

inline fn gemmUkernel6x8(
    kc_len: usize,
    a_ptr: [*]const f32,
    k_dim: usize,
    kc0: usize,
    bp_ptr: [*]const f32,
    c_ptr: [*]f32,
    c_stride: usize,
    first_kc: bool,
    bias6: [6]f32,
) void {
    const c0 = c_ptr;
    const c1 = c0 + c_stride;
    const c2 = c1 + c_stride;
    const c3 = c2 + c_stride;
    const c4 = c3 + c_stride;
    const c5 = c4 + c_stride;

    var acc00: Vec4 = undefined;
    var acc01: Vec4 = undefined;
    var acc10: Vec4 = undefined;
    var acc11: Vec4 = undefined;
    var acc20: Vec4 = undefined;
    var acc21: Vec4 = undefined;
    var acc30: Vec4 = undefined;
    var acc31: Vec4 = undefined;
    var acc40: Vec4 = undefined;
    var acc41: Vec4 = undefined;
    var acc50: Vec4 = undefined;
    var acc51: Vec4 = undefined;

    if (first_kc) {
        acc00 = @splat(bias6[0]);
        acc01 = acc00;
        acc10 = @splat(bias6[1]);
        acc11 = acc10;
        acc20 = @splat(bias6[2]);
        acc21 = acc20;
        acc30 = @splat(bias6[3]);
        acc31 = acc30;
        acc40 = @splat(bias6[4]);
        acc41 = acc40;
        acc50 = @splat(bias6[5]);
        acc51 = acc50;
    } else {
        acc00 = loadVec4(c0);
        acc01 = loadVec4(c0 + 4);
        acc10 = loadVec4(c1);
        acc11 = loadVec4(c1 + 4);
        acc20 = loadVec4(c2);
        acc21 = loadVec4(c2 + 4);
        acc30 = loadVec4(c3);
        acc31 = loadVec4(c3 + 4);
        acc40 = loadVec4(c4);
        acc41 = loadVec4(c4 + 4);
        acc50 = loadVec4(c5);
        acc51 = loadVec4(c5 + 4);
    }

    const a0_base = a_ptr + kc0;
    const a1_base = a0_base + k_dim;
    const a2_base = a1_base + k_dim;
    const a3_base = a2_base + k_dim;
    const a4_base = a3_base + k_dim;
    const a5_base = a4_base + k_dim;
    var bp = bp_ptr;

    for (0..kc_len) |k| {
        const bv0 = loadVec4(bp);
        const bv1 = loadVec4(bp + 4);
        bp += 8;

        var av: Vec4 = @splat(a0_base[k]);
        acc00 += av * bv0;
        acc01 += av * bv1;

        av = @splat(a1_base[k]);
        acc10 += av * bv0;
        acc11 += av * bv1;

        av = @splat(a2_base[k]);
        acc20 += av * bv0;
        acc21 += av * bv1;

        av = @splat(a3_base[k]);
        acc30 += av * bv0;
        acc31 += av * bv1;

        av = @splat(a4_base[k]);
        acc40 += av * bv0;
        acc41 += av * bv1;

        av = @splat(a5_base[k]);
        acc50 += av * bv0;
        acc51 += av * bv1;
    }

    storeVec4(c0, acc00);
    storeVec4(c0 + 4, acc01);
    storeVec4(c1, acc10);
    storeVec4(c1 + 4, acc11);
    storeVec4(c2, acc20);
    storeVec4(c2 + 4, acc21);
    storeVec4(c3, acc30);
    storeVec4(c3 + 4, acc31);
    storeVec4(c4, acc40);
    storeVec4(c4 + 4, acc41);
    storeVec4(c5, acc50);
    storeVec4(c5 + 4, acc51);
}

inline fn gemmUkernel1x8(
    kc_len: usize,
    a_row_ptr: [*]const f32,
    bp_ptr: [*]const f32,
    c_ptr: [*]f32,
    first_kc: bool,
    bias0: f32,
) void {
    var acc0: Vec4 = undefined;
    var acc1: Vec4 = undefined;
    if (first_kc) {
        acc0 = @splat(bias0);
        acc1 = acc0;
    } else {
        acc0 = loadVec4(c_ptr);
        acc1 = loadVec4(c_ptr + 4);
    }

    var bp = bp_ptr;
    for (0..kc_len) |k| {
        const bv0 = loadVec4(bp);
        const bv1 = loadVec4(bp + 4);
        bp += 8;
        const av: Vec4 = @splat(a_row_ptr[k]);
        acc0 += av * bv0;
        acc1 += av * bv1;
    }

    storeVec4(c_ptr, acc0);
    storeVec4(c_ptr + 4, acc1);
}

fn gemmRcr(
    ws: *Workspace,
    m_dim: usize,
    n_dim: usize,
    k_dim: usize,
    a: []const f32,
    b: []const f32,
    bias: ?[]const f32,
    c: []f32,
    epi: Epi,
) !void {
    if (m_dim == 0 or n_dim == 0) return;
    if (k_dim == 0) {
        for (0..m_dim) |m| {
            const init = if (bias) |bs| bs[m] else 0.0;
            const row = c[m * n_dim .. (m + 1) * n_dim];
            @memset(row, init);
            epi.applyRow(row, m);
        }
        return;
    }

    const has_epi = epi != .none;
    var jc0: usize = 0;
    while (jc0 < n_dim) {
        const nc_len = @min(n_dim - jc0, GEMM_NC);
        const full_panels = nc_len / GEMM_NR;
        const rem_cols = nc_len - full_panels * GEMM_NR;

        var kc0: usize = 0;
        while (kc0 < k_dim) {
            const kc_len = @min(k_dim - kc0, GEMM_KC);
            const first_kc = kc0 == 0;
            const kc_last = kc0 + kc_len == k_dim;
            const b_pack = try packBPanels(ws, b, n_dim, kc0, kc_len, jc0, nc_len);

            const bp_base = b_pack.ptr;
            const a_base = a.ptr;
            const c_base = c.ptr;

            var m: usize = 0;
            while (m + 6 <= m_dim) : (m += 6) {
                const bias6: [6]f32 = if (bias) |bs|
                    .{ bs[m], bs[m + 1], bs[m + 2], bs[m + 3], bs[m + 4], bs[m + 5] }
                else
                    .{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

                const a_m = a_base + m * k_dim;
                for (0..full_panels) |p| {
                    const col = jc0 + p * GEMM_NR;
                    gemmUkernel6x8(
                        kc_len,
                        a_m,
                        k_dim,
                        kc0,
                        bp_base + p * kc_len * GEMM_NR,
                        c_base + m * n_dim + col,
                        n_dim,
                        first_kc,
                        bias6,
                    );
                }
                if (rem_cols > 0) {
                    const col = jc0 + full_panels * GEMM_NR;
                    var tmp: [6 * GEMM_NR]f32 = @splat(0.0);
                    if (!first_kc) {
                        for (0..6) |r| {
                            for (0..rem_cols) |lane| {
                                tmp[r * GEMM_NR + lane] = c_base[(m + r) * n_dim + col + lane];
                            }
                        }
                    }
                    gemmUkernel6x8(
                        kc_len,
                        a_m,
                        k_dim,
                        kc0,
                        bp_base + full_panels * kc_len * GEMM_NR,
                        &tmp,
                        GEMM_NR,
                        first_kc,
                        bias6,
                    );
                    for (0..6) |r| {
                        for (0..rem_cols) |lane| {
                            c_base[(m + r) * n_dim + col + lane] = tmp[r * GEMM_NR + lane];
                        }
                    }
                }
                if (kc_last and has_epi) {
                    for (0..6) |r| {
                        const row_off = (m + r) * n_dim + jc0;
                        epi.applyRow(c[row_off .. row_off + nc_len], m + r);
                    }
                }
            }

            while (m < m_dim) : (m += 1) {
                const bias0 = if (bias) |bs| bs[m] else 0.0;
                const a_row_ptr = a_base + m * k_dim + kc0;
                for (0..full_panels) |p| {
                    const col = jc0 + p * GEMM_NR;
                    gemmUkernel1x8(
                        kc_len,
                        a_row_ptr,
                        bp_base + p * kc_len * GEMM_NR,
                        c_base + m * n_dim + col,
                        first_kc,
                        bias0,
                    );
                }
                if (rem_cols > 0) {
                    const col = jc0 + full_panels * GEMM_NR;
                    var tmp: [GEMM_NR]f32 = @splat(0.0);
                    if (!first_kc) {
                        for (0..rem_cols) |lane| {
                            tmp[lane] = c_base[m * n_dim + col + lane];
                        }
                    }
                    gemmUkernel1x8(
                        kc_len,
                        a_row_ptr,
                        bp_base + full_panels * kc_len * GEMM_NR,
                        &tmp,
                        first_kc,
                        bias0,
                    );
                    for (0..rem_cols) |lane| {
                        c_base[m * n_dim + col + lane] = tmp[lane];
                    }
                }
                if (kc_last and has_epi) {
                    const row_off = m * n_dim + jc0;
                    epi.applyRow(c[row_off .. row_off + nc_len], m);
                }
            }

            kc0 += kc_len;
        }
        jc0 += nc_len;
    }
}

// ---------------------------------------------------------------------------
// Layer forward implementations
// ---------------------------------------------------------------------------

fn resolveConvPads(
    in_w: usize,
    in_h: usize,
    kernel_extent_w: usize,
    kernel_extent_h: usize,
    stride_w: usize,
    stride_h: usize,
    pad_left: i32,
    pad_right: i32,
    pad_top: i32,
    pad_bottom: i32,
) struct { usize, usize, usize, usize } {
    const iw_i: isize = @intCast(in_w);
    const ih_i: isize = @intCast(in_h);
    const kw_i: isize = @intCast(kernel_extent_w);
    const kh_i: isize = @intCast(kernel_extent_h);
    const sw_i: isize = @intCast(stride_w);
    const sh_i: isize = @intCast(stride_h);

    const pl: usize, const pr: usize = if (pad_left == -233) blk: {
        const wpad: usize = @intCast(@max(kw_i + @divTrunc(iw_i - 1, sw_i) * sw_i - iw_i, 0));
        break :blk .{ wpad / 2, wpad - wpad / 2 };
    } else if (pad_left == -234) blk: {
        const wpad: usize = @intCast(@max(kw_i + @divTrunc(iw_i - 1, sw_i) * sw_i - iw_i, 0));
        break :blk .{ wpad - wpad / 2, wpad / 2 };
    } else .{
        @intCast(@max(pad_left, 0)),
        @intCast(@max(pad_right, 0)),
    };

    const pt: usize, const pb: usize = if (pad_top == -233) blk: {
        const hpad: usize = @intCast(@max(kh_i + @divTrunc(ih_i - 1, sh_i) * sh_i - ih_i, 0));
        break :blk .{ hpad / 2, hpad - hpad / 2 };
    } else if (pad_top == -234) blk: {
        const hpad: usize = @intCast(@max(kh_i + @divTrunc(ih_i - 1, sh_i) * sh_i - ih_i, 0));
        break :blk .{ hpad - hpad / 2, hpad / 2 };
    } else .{
        @intCast(@max(pad_top, 0)),
        @intCast(@max(pad_bottom, 0)),
    };

    return .{ pl, pr, pt, pb };
}

fn forwardConv(
    ws: *Workspace,
    inp: Tensor,
    num_output: usize,
    num_input: usize,
    kernel_w: usize,
    kernel_h: usize,
    dilation_w: usize,
    dilation_h: usize,
    stride_w: usize,
    stride_h: usize,
    pad_left: i32,
    pad_right: i32,
    pad_top: i32,
    pad_bottom: i32,
    pad_value: f32,
    group: usize,
    weights: []const f32,
    bias: ?[]const f32,
    act: ActSpec,
) !Tensor {
    if (inp.c != num_input) return error.InvalidGraph;
    const kext_w = dilation_w * (kernel_w - 1) + 1;
    const kext_h = dilation_h * (kernel_h - 1) + 1;
    const pl, const pr, const pt, const pb = resolveConvPads(
        inp.w,
        inp.h,
        kext_w,
        kext_h,
        stride_w,
        stride_h,
        pad_left,
        pad_right,
        pad_top,
        pad_bottom,
    );
    const padded_w = inp.w + pl + pr;
    const padded_h = inp.h + pt + pb;
    if (padded_w < kext_w or padded_h < kext_h) return error.InvalidGraph;
    const out_w = (padded_w - kext_w) / stride_w + 1;
    const out_h = (padded_h - kext_h) / stride_h + 1;
    const out_plane = out_w * out_h;
    const in_plane = inp.w * inp.h;

    const out = try ws.allocTensor(out_w, out_h, num_output, null);
    errdefer ws.recycle(out);

    // Fast path 1: 1x1 conv, stride 1, pad 0 -> pure GEMM without im2col.
    if (kernel_w == 1 and kernel_h == 1 and stride_w == 1 and stride_h == 1 and pl == 0 and pr == 0 and pt == 0 and pb == 0) {
        const in_g = num_input / group;
        const out_g = num_output / group;
        const epi_cap = Epi.handles(act);
        for (0..group) |g| {
            const a = weights[g * out_g * in_g .. (g + 1) * out_g * in_g];
            const b = inp.backing[g * in_g * in_plane .. (g + 1) * in_g * in_plane];
            const bs: ?[]const f32 = if (bias) |b_slice| b_slice[g * out_g .. (g + 1) * out_g] else null;
            const c = out.backing[g * out_g * out_plane .. (g + 1) * out_g * out_plane];
            const epi: Epi = if (epi_cap) switch (act.kind) {
                .prelu_chan => .{ .prelu = slopeGroup(act.slopes, g, out_g) },
                else => Epi.fromSpec(act),
            } else .{ .none = {} };
            try gemmRcr(ws, out_g, out_plane, in_g, a, b, bs, c, epi);
        }
        if (!epi_cap) {
            applyActSlice(out.data(), act, num_output, out_plane);
        }
        return out;
    }

    // Fast path 2: Depthwise convolution.
    if (group == num_input and group == num_output) {
        const maxk = kernel_w * kernel_h;
        for (0..num_output) |c| {
            const kptr = weights[c * maxk .. (c + 1) * maxk];
            const init = if (bias) |bs| bs[c] else 0.0;
            const src = inp.channel(c);
            const dst = out.channelMut(c);
            @memset(dst, init);

            for (0..kernel_h) |ky| {
                const sy_base: isize = @as(isize, @intCast(ky * dilation_h)) - @as(isize, @intCast(pt));
                for (0..kernel_w) |kx| {
                    const sx_base: isize = @as(isize, @intCast(kx * dilation_w)) - @as(isize, @intCast(pl));
                    const w_val = kptr[ky * kernel_w + kx];
                    for (0..out_h) |oy| {
                        const sy = sy_base + @as(isize, @intCast(oy * stride_h));
                        const dst_row = dst[oy * out_w .. (oy + 1) * out_w];
                        if (sy < 0 or sy >= @as(isize, @intCast(inp.h))) {
                            if (pad_value != 0.0) {
                                const add = w_val * pad_value;
                                for (dst_row) |*d| d.* += add;
                            }
                            continue;
                        }
                        const sy_u: usize = @intCast(sy);
                        const src_row = src[sy_u * inp.w .. (sy_u + 1) * inp.w];
                        if (stride_w == 1) {
                            const ox_start: usize = @intCast(@max(-sx_base, 0));
                            const ox_end: usize = @min(@as(usize, @intCast(@max(@as(isize, @intCast(inp.w)) - sx_base, 0))), out_w);
                            if (pad_value != 0.0) {
                                const add = w_val * pad_value;
                                for (dst_row[0..ox_start]) |*d| d.* += add;
                                for (dst_row[ox_end..]) |*d| d.* += add;
                            }
                            if (ox_start < ox_end) {
                                const s_off: usize = @intCast(sx_base + @as(isize, @intCast(ox_start)));
                                const seg_len = ox_end - ox_start;
                                saxpySlice(dst_row[ox_start..ox_end], src_row[s_off .. s_off + seg_len], w_val);
                            }
                        } else {
                            for (dst_row, 0..) |*d, ox| {
                                const sx = sx_base + @as(isize, @intCast(ox * stride_w));
                                const sv = if (sx >= 0 and sx < @as(isize, @intCast(inp.w)))
                                    src_row[@intCast(sx)]
                                else
                                    pad_value;
                                d.* += w_val * sv;
                            }
                        }
                    }
                }
            }
        }
        applyActSlice(out.data(), act, num_output, out_plane);
        return out;
    }

    // General / grouped convolution: im2col + GEMM.
    const in_g = num_input / group;
    const out_g = num_output / group;
    const maxk = kernel_w * kernel_h;
    const k_dim = in_g * maxk;
    const col_len = k_dim * out_plane;
    const col = try ws.ensureScratch(&ws.col_buf, col_len);
    const epi_cap = Epi.handles(act);

    for (0..group) |g| {
        for (0..in_g) |ic| {
            const src = inp.channel(g * in_g + ic);
            for (0..kernel_h) |ky| {
                const sy_base: isize = @as(isize, @intCast(ky * dilation_h)) - @as(isize, @intCast(pt));
                for (0..kernel_w) |kx| {
                    const sx_base: isize = @as(isize, @intCast(kx * dilation_w)) - @as(isize, @intCast(pl));
                    const col_row_idx = (ic * maxk) + ky * kernel_w + kx;
                    const col_row = col[col_row_idx * out_plane .. (col_row_idx + 1) * out_plane];

                    for (0..out_h) |oy| {
                        const sy = sy_base + @as(isize, @intCast(oy * stride_h));
                        const dst_seg = col_row[oy * out_w .. (oy + 1) * out_w];
                        if (sy < 0 or sy >= @as(isize, @intCast(inp.h))) {
                            @memset(dst_seg, pad_value);
                            continue;
                        }
                        const sy_u: usize = @intCast(sy);
                        const src_row = src[sy_u * inp.w .. (sy_u + 1) * inp.w];
                        if (stride_w == 1) {
                            const out_w_i: isize = @intCast(out_w);
                            const ox_start: usize = @intCast(std.math.clamp(-sx_base, 0, out_w_i));
                            const ox_end: usize = @intCast(std.math.clamp(@as(isize, @intCast(inp.w)) - sx_base, 0, out_w_i));
                            if (ox_start > 0) @memset(dst_seg[0..ox_start], pad_value);
                            if (ox_start < ox_end) {
                                const s_off: usize = @intCast(sx_base + @as(isize, @intCast(ox_start)));
                                const seg_len = ox_end - ox_start;
                                @memcpy(dst_seg[ox_start..ox_end], src_row[s_off .. s_off + seg_len]);
                            }
                            if (ox_end < out_w) @memset(dst_seg[ox_end..], pad_value);
                        } else {
                            for (dst_seg, 0..) |*slot, ox| {
                                const sx = sx_base + @as(isize, @intCast(ox * stride_w));
                                slot.* = if (sx >= 0 and sx < @as(isize, @intCast(inp.w)))
                                    src_row[@intCast(sx)]
                                else
                                    pad_value;
                            }
                        }
                    }
                }
            }
        }

        const a = weights[g * out_g * k_dim .. (g + 1) * out_g * k_dim];
        const bs: ?[]const f32 = if (bias) |b_slice| b_slice[g * out_g .. (g + 1) * out_g] else null;
        const c = out.backing[g * out_g * out_plane .. (g + 1) * out_g * out_plane];
        const epi: Epi = if (epi_cap) switch (act.kind) {
            .prelu_chan => .{ .prelu = slopeGroup(act.slopes, g, out_g) },
            else => Epi.fromSpec(act),
        } else .{ .none = {} };
        try gemmRcr(ws, out_g, out_plane, k_dim, a, col, bs, c, epi);
    }

    if (!epi_cap) {
        applyActSlice(out.data(), act, num_output, out_plane);
    }
    return out;
}

fn forwardDeconv(
    ws: *Workspace,
    inp: Tensor,
    num_output: usize,
    num_input: usize,
    kernel_w: usize,
    kernel_h: usize,
    dilation_w: usize,
    dilation_h: usize,
    stride_w: usize,
    stride_h: usize,
    pad_left: i32,
    pad_right: i32,
    pad_top: i32,
    pad_bottom: i32,
    output_pad_right: i32,
    output_pad_bottom: i32,
    output_w_param: i32,
    output_h_param: i32,
    group: usize,
    weights: []const f32,
    bias: ?[]const f32,
    act: ActSpec,
) !Tensor {
    if (inp.c != num_input) return error.InvalidGraph;
    const kext_w = dilation_w * (kernel_w - 1) + 1;
    const kext_h = dilation_h * (kernel_h - 1) + 1;
    const full_w = (inp.w - 1) * stride_w + kext_w + @as(usize, @intCast(@max(output_pad_right, 0)));
    const full_h = (inp.h - 1) * stride_h + kext_h + @as(usize, @intCast(@max(output_pad_bottom, 0)));

    const pl: usize, const pt: usize, const out_w: usize, const out_h: usize = if (output_w_param > 0 and output_h_param > 0) blk: {
        const ow: usize = @intCast(output_w_param);
        const oh: usize = @intCast(output_h_param);
        const wpad = full_w -| ow;
        const hpad = full_h -| oh;
        if (pad_left == -233) {
            break :blk .{ wpad / 2, hpad / 2, ow, oh };
        } else if (pad_left == -234) {
            break :blk .{ wpad - wpad / 2, hpad - hpad / 2, ow, oh };
        } else {
            break :blk .{ @intCast(@max(pad_left, 0)), @intCast(@max(pad_top, 0)), ow, oh };
        }
    } else blk: {
        const l: usize = @intCast(@max(pad_left, 0));
        const r: usize = @intCast(@max(pad_right, 0));
        const t: usize = @intCast(@max(pad_top, 0));
        const b: usize = @intCast(@max(pad_bottom, 0));
        if (full_w <= l + r or full_h <= t + b) return error.InvalidGraph;
        break :blk .{ l, t, full_w - l - r, full_h - t - b };
    };

    const in_plane = inp.w * inp.h;
    const in_g = num_input / group;
    const out_g = num_output / group;
    const maxk = kernel_w * kernel_h;

    const out = try ws.allocTensor(out_w, out_h, num_output, null);
    errdefer ws.recycle(out);
    for (0..num_output) |c| {
        const init = if (bias) |bs| bs[c] else 0.0;
        @memset(out.channelMut(c), init);
    }

    const m_dim = out_g * maxk;
    const perm_len = m_dim * in_g;
    const col_len = m_dim * in_plane;
    const w_perm = try ws.ensureScratch(&ws.weight_perm_buf, perm_len);
    const col = try ws.ensureScratch(&ws.col_buf, col_len);

    for (0..group) |g| {
        const w_src = weights[g * out_g * in_g * maxk .. (g + 1) * out_g * in_g * maxk];
        for (0..out_g) |oc| {
            for (0..in_g) |ic| {
                const src_k = w_src[(oc * in_g + ic) * maxk .. (oc * in_g + ic + 1) * maxk];
                for (src_k, 0..) |wv, k| {
                    w_perm[(oc * maxk + k) * in_g + ic] = wv;
                }
            }
        }

        const b_in = inp.backing[g * in_g * in_plane .. (g + 1) * in_g * in_plane];
        try gemmRcr(ws, m_dim, in_plane, in_g, w_perm, b_in, null, col, .{ .none = {} });

        for (0..out_g) |oc| {
            const dst = out.channelMut(g * out_g + oc);
            for (0..kernel_h) |ky| {
                for (0..kernel_w) |kx| {
                    const k = ky * kernel_w + kx;
                    const col_plane = col[(oc * maxk + k) * in_plane .. (oc * maxk + k + 1) * in_plane];
                    for (0..inp.h) |iy| {
                        const fy = iy * stride_h + ky * dilation_h;
                        if (fy < pt or fy >= pt + out_h) continue;
                        const oy = fy - pt;
                        const src_row = col_plane[iy * inp.w .. (iy + 1) * inp.w];
                        const dst_row = dst[oy * out_w .. (oy + 1) * out_w];
                        for (src_row, 0..) |val, ix| {
                            const fx = ix * stride_w + kx * dilation_w;
                            if (fx >= pl and fx < pl + out_w) {
                                dst_row[fx - pl] += val;
                            }
                        }
                    }
                }
            }
        }
    }

    const out_plane = out_w * out_h;
    applyActSlice(out.data(), act, num_output, out_plane);
    return out;
}

inline fn cubicWeights(fx_in: f32, dim: usize) struct { [4]usize, [4]f32 } {
    var fx = fx_in;
    var sx: isize = @intFromFloat(@floor(fx));
    fx -= @as(f32, @floatFromInt(sx));
    if (sx < -1) {
        sx = -1;
        fx = 0.0;
    }
    if (sx >= @as(isize, @intCast(dim))) {
        sx = @as(isize, @intCast(dim)) - 1;
        fx = 0.0;
    }
    const a: f32 = -0.75;
    const c0 = ((a * (fx + 1.0) - 5.0 * a) * (fx + 1.0) + 8.0 * a) * (fx + 1.0) - 4.0 * a;
    const c1 = ((a + 2.0) * fx - (a + 3.0)) * fx * fx + 1.0;
    const one_minus_fx = 1.0 - fx;
    const c2 = ((a + 2.0) * one_minus_fx - (a + 3.0)) * one_minus_fx * one_minus_fx + 1.0;
    const c3 = 1.0 - c0 - c1 - c2;
    const max_i: isize = @max(@as(isize, @intCast(dim)) - 1, 0);
    const idx = [4]usize{
        @intCast(std.math.clamp(sx - 1, 0, max_i)),
        @intCast(std.math.clamp(sx, 0, max_i)),
        @intCast(std.math.clamp(sx + 1, 0, max_i)),
        @intCast(std.math.clamp(sx + 2, 0, max_i)),
    };
    return .{ idx, .{ c0, c1, c2, c3 } };
}

fn forwardInterp(
    ws: *Workspace,
    inp: Tensor,
    resize_type: i32,
    height_scale: f32,
    width_scale: f32,
    output_height: i32,
    output_width: i32,
    align_corner: bool,
) !Tensor {
    const out_w: usize = if (output_width > 0)
        @intCast(output_width)
    else
        @max(@as(usize, @intFromFloat(@max(@as(f32, @floatFromInt(inp.w)) * width_scale, 0.0))), 1);
    const out_h: usize = if (output_height > 0)
        @intCast(output_height)
    else
        @max(@as(usize, @intFromFloat(@max(@as(f32, @floatFromInt(inp.h)) * height_scale, 0.0))), 1);

    const out = try ws.allocTensor(out_w, out_h, inp.c, null);
    errdefer ws.recycle(out);

    if (out_w == inp.w and out_h == inp.h) {
        @memcpy(out.data(), inp.dataConst());
        return out;
    }

    const alloc = ws.allocator;

    switch (resize_type) {
        1 => {
            const ws_inv = @as(f32, @floatFromInt(inp.w)) / @as(f32, @floatFromInt(out_w));
            const hs_inv = @as(f32, @floatFromInt(inp.h)) / @as(f32, @floatFromInt(out_h));
            const x_ofs = try alloc.alloc(usize, out_w);
            defer alloc.free(x_ofs);
            const y_ofs = try alloc.alloc(usize, out_h);
            defer alloc.free(y_ofs);

            for (0..out_w) |dx| {
                x_ofs[dx] = @min(@as(usize, @intFromFloat(@as(f32, @floatFromInt(dx)) * ws_inv)), inp.w - 1);
            }
            for (0..out_h) |dy| {
                y_ofs[dy] = @min(@as(usize, @intFromFloat(@as(f32, @floatFromInt(dy)) * hs_inv)), inp.h - 1);
            }

            for (0..inp.c) |c| {
                const src = inp.channel(c);
                const dst = out.channelMut(c);
                for (y_ofs, 0..) |sy, dy| {
                    const srow = src[sy * inp.w .. (sy + 1) * inp.w];
                    const drow = dst[dy * out_w .. (dy + 1) * out_w];
                    for (x_ofs, 0..) |sx, dx| {
                        drow[dx] = srow[sx];
                    }
                }
            }
        },
        2 => {
            const scale_x: f32 = if (align_corner)
                (if (out_w > 1) @as(f32, @floatFromInt(inp.w - 1)) / @as(f32, @floatFromInt(out_w - 1)) else 0.0)
            else
                @as(f32, @floatFromInt(inp.w)) / @as(f32, @floatFromInt(out_w));
            const scale_y: f32 = if (align_corner)
                (if (out_h > 1) @as(f32, @floatFromInt(inp.h - 1)) / @as(f32, @floatFromInt(out_h - 1)) else 0.0)
            else
                @as(f32, @floatFromInt(inp.h)) / @as(f32, @floatFromInt(out_h));

            const Tap = struct { i0: usize, i1: usize, w0: f32, w1: f32 };
            const x_tab = try alloc.alloc(Tap, out_w);
            defer alloc.free(x_tab);
            const y_tab = try alloc.alloc(Tap, out_h);
            defer alloc.free(y_tab);

            for (0..out_w) |dx| {
                var fx = if (align_corner)
                    @as(f32, @floatFromInt(dx)) * scale_x
                else
                    (@as(f32, @floatFromInt(dx)) + 0.5) * scale_x - 0.5;
                if (inp.w == 1) {
                    x_tab[dx] = .{ .i0 = 0, .i1 = 0, .w0 = 1.0, .w1 = 0.0 };
                } else {
                    if (fx < 0.0) fx = 0.0;
                    var sx: isize = @intFromFloat(@floor(fx));
                    fx -= @as(f32, @floatFromInt(sx));
                    if (sx >= @as(isize, @intCast(inp.w)) - 1) {
                        sx = @as(isize, @intCast(inp.w)) - 2;
                        fx = 1.0;
                    }
                    const s0: usize = @intCast(sx);
                    x_tab[dx] = .{ .i0 = s0, .i1 = s0 + 1, .w0 = 1.0 - fx, .w1 = fx };
                }
            }

            for (0..out_h) |dy| {
                var fy = if (align_corner)
                    @as(f32, @floatFromInt(dy)) * scale_y
                else
                    (@as(f32, @floatFromInt(dy)) + 0.5) * scale_y - 0.5;
                if (inp.h == 1) {
                    y_tab[dy] = .{ .i0 = 0, .i1 = 0, .w0 = 1.0, .w1 = 0.0 };
                } else {
                    if (fy < 0.0) fy = 0.0;
                    var sy: isize = @intFromFloat(@floor(fy));
                    fy -= @as(f32, @floatFromInt(sy));
                    if (sy >= @as(isize, @intCast(inp.h)) - 1) {
                        sy = @as(isize, @intCast(inp.h)) - 2;
                        fy = 1.0;
                    }
                    const s0: usize = @intCast(sy);
                    y_tab[dy] = .{ .i0 = s0, .i1 = s0 + 1, .w0 = 1.0 - fy, .w1 = fy };
                }
            }

            for (0..inp.c) |c| {
                const src = inp.channel(c);
                const dst = out.channelMut(c);
                for (y_tab, 0..) |yt, dy| {
                    const r0 = src[yt.i0 * inp.w .. (yt.i0 + 1) * inp.w];
                    const r1 = src[yt.i1 * inp.w .. (yt.i1 + 1) * inp.w];
                    const drow = dst[dy * out_w .. (dy + 1) * out_w];
                    for (x_tab, 0..) |xt, dx| {
                        const v0 = r0[xt.i0] * xt.w0 + r0[xt.i1] * xt.w1;
                        const v1 = r1[xt.i0] * xt.w0 + r1[xt.i1] * xt.w1;
                        drow[dx] = v0 * yt.w0 + v1 * yt.w1;
                    }
                }
            }
        },
        else => {
            const scale_x: f32 = if (align_corner)
                (if (out_w > 1) @as(f32, @floatFromInt(inp.w - 1)) / @as(f32, @floatFromInt(out_w - 1)) else 0.0)
            else
                @as(f32, @floatFromInt(inp.w)) / @as(f32, @floatFromInt(out_w));
            const scale_y: f32 = if (align_corner)
                (if (out_h > 1) @as(f32, @floatFromInt(inp.h - 1)) / @as(f32, @floatFromInt(out_h - 1)) else 0.0)
            else
                @as(f32, @floatFromInt(inp.h)) / @as(f32, @floatFromInt(out_h));

            const CubicTap = struct { idx: [4]usize, w: [4]f32 };
            const x_tab = try alloc.alloc(CubicTap, out_w);
            defer alloc.free(x_tab);
            const y_tab = try alloc.alloc(CubicTap, out_h);
            defer alloc.free(y_tab);
            const row_buf = try alloc.alloc([4]f32, out_w);
            defer alloc.free(row_buf);

            for (0..out_w) |dx| {
                const fx = if (align_corner)
                    @as(f32, @floatFromInt(dx)) * scale_x
                else
                    (@as(f32, @floatFromInt(dx)) + 0.5) * scale_x - 0.5;
                const cw = cubicWeights(fx, inp.w);
                x_tab[dx] = .{ .idx = cw[0], .w = cw[1] };
            }
            for (0..out_h) |dy| {
                const fy = if (align_corner)
                    @as(f32, @floatFromInt(dy)) * scale_y
                else
                    (@as(f32, @floatFromInt(dy)) + 0.5) * scale_y - 0.5;
                const cw = cubicWeights(fy, inp.h);
                y_tab[dy] = .{ .idx = cw[0], .w = cw[1] };
            }

            for (0..inp.c) |c| {
                const src = inp.channel(c);
                const dst = out.channelMut(c);
                for (y_tab, 0..) |yt, dy| {
                    const r0 = src[yt.idx[0] * inp.w .. (yt.idx[0] + 1) * inp.w];
                    const r1 = src[yt.idx[1] * inp.w .. (yt.idx[1] + 1) * inp.w];
                    const r2 = src[yt.idx[2] * inp.w .. (yt.idx[2] + 1) * inp.w];
                    const r3 = src[yt.idx[3] * inp.w .. (yt.idx[3] + 1) * inp.w];
                    for (x_tab, 0..) |xt, dx| {
                        const sx = xt.idx;
                        const wx = xt.w;
                        row_buf[dx][0] = r0[sx[0]] * wx[0] + r0[sx[1]] * wx[1] + r0[sx[2]] * wx[2] + r0[sx[3]] * wx[3];
                        row_buf[dx][1] = r1[sx[0]] * wx[0] + r1[sx[1]] * wx[1] + r1[sx[2]] * wx[2] + r1[sx[3]] * wx[3];
                        row_buf[dx][2] = r2[sx[0]] * wx[0] + r2[sx[1]] * wx[1] + r2[sx[2]] * wx[2] + r2[sx[3]] * wx[3];
                        row_buf[dx][3] = r3[sx[0]] * wx[0] + r3[sx[1]] * wx[1] + r3[sx[2]] * wx[2] + r3[sx[3]] * wx[3];
                    }
                    const drow = dst[dy * out_w .. (dy + 1) * out_w];
                    const wy = yt.w;
                    for (drow, 0..) |*slot, dx| {
                        const rb = row_buf[dx];
                        slot.* = rb[0] * wy[0] + rb[1] * wy[1] + rb[2] * wy[2] + rb[3] * wy[3];
                    }
                }
            }
        },
    }

    return out;
}

inline fn binaryEval(op_type: i32, a: f32, b: f32) f32 {
    return switch (op_type) {
        0 => a + b,
        1 => a - b,
        2 => a * b,
        3 => a / b,
        4 => @max(a, b),
        5 => @min(a, b),
        6 => std.math.pow(f32, a, b),
        7 => b - a,
        8 => b / a,
        9 => std.math.pow(f32, b, a),
        10 => std.math.atan2(a, b),
        11 => std.math.atan2(b, a),
        else => a + b,
    };
}

fn forwardBinary(
    ws: *Workspace,
    a: Tensor,
    b_opt: ?Tensor,
    op_type: i32,
    with_scalar: bool,
    scalar: f32,
) !Tensor {
    if (with_scalar) {
        const out = try ws.allocTensor(a.w, a.h, a.c, null);
        if (ZipOp.fromBinaryOp(op_type)) |op| {
            zipScalarSlice(out.data(), a.dataConst(), op, scalar);
        } else {
            for (out.data(), a.dataConst()) |*d, s| {
                d.* = binaryEval(op_type, s, scalar);
            }
        }
        return out;
    }

    const b = b_opt orelse return error.InvalidGraph;

    if (a.w == b.w and a.h == b.h and a.c == b.c) {
        const out = try ws.allocTensor(a.w, a.h, a.c, null);
        if (ZipOp.fromBinaryOp(op_type)) |op| {
            zip3Slice(out.data(), a.dataConst(), b.dataConst(), op);
        } else {
            for (out.data(), a.dataConst(), b.dataConst()) |*d, va, vb| {
                d.* = binaryEval(op_type, va, vb);
            }
        }
        return out;
    }

    if (a.c == b.c and b.w == 1 and b.h == 1) {
        const out = try ws.allocTensor(a.w, a.h, a.c, null);
        const zop = ZipOp.fromBinaryOp(op_type);
        for (0..a.c) |c| {
            const bv = b.backing[c];
            const src = a.channel(c);
            const dst = out.channelMut(c);
            if (zop) |op| {
                zipScalarSlice(dst, src, op, bv);
            } else {
                for (dst, src) |*d, va| {
                    d.* = binaryEval(op_type, va, bv);
                }
            }
        }
        return out;
    }

    if (a.c == b.c and a.w == 1 and a.h == 1) {
        const out = try ws.allocTensor(b.w, b.h, b.c, null);
        const zop = ZipOp.fromBinaryOp(op_type);
        for (0..b.c) |c| {
            const av = a.backing[c];
            const src = b.channel(c);
            const dst = out.channelMut(c);
            if (zop) |op| {
                zipScalarSlice(dst, src, op, av);
            } else {
                for (dst, src) |*d, vb| {
                    d.* = binaryEval(op_type, av, vb);
                }
            }
        }
        return out;
    }

    const oc = @max(a.c, b.c);
    const oh = @max(a.h, b.h);
    const ow = @max(a.w, b.w);
    const out = try ws.allocTensor(ow, oh, oc, null);
    for (0..oc) |c| {
        const ac = if (a.c == 1) 0 else @min(c, a.c - 1);
        const bc = if (b.c == 1) 0 else @min(c, b.c - 1);
        const a_plane = a.channel(ac);
        const b_plane = b.channel(bc);
        const o_plane = out.channelMut(c);
        for (0..oh) |y| {
            const ay = if (a.h == 1) 0 else @min(y, a.h - 1);
            const by = if (b.h == 1) 0 else @min(y, b.h - 1);
            const a_row = a_plane[ay * a.w .. (ay + 1) * a.w];
            const b_row = b_plane[by * b.w .. (by + 1) * b.w];
            const o_row = o_plane[y * ow .. (y + 1) * ow];
            for (o_row, 0..) |*slot, x| {
                const ax = if (a.w == 1) 0 else @min(x, a.w - 1);
                const bx = if (b.w == 1) 0 else @min(x, b.w - 1);
                slot.* = binaryEval(op_type, a_row[ax], b_row[bx]);
            }
        }
    }
    return out;
}

fn resolveCropIdx(val: i32, dim: usize, is_end: bool) usize {
    if (is_end and (val == -233 or val == std.math.maxInt(i32))) {
        return dim;
    }
    const dim_i: i32 = @intCast(dim);
    const r = if (val < 0) dim_i + val else val;
    return @intCast(std.math.clamp(r, 0, dim_i));
}

fn resolveCropRegion(
    in_c: usize,
    in_h: usize,
    in_w: usize,
    woffset: i32,
    hoffset: i32,
    coffset: i32,
    outw: i32,
    outh: i32,
    outc: i32,
    woffset2: i32,
    hoffset2: i32,
    coffset2: i32,
    starts: []const i32,
    ends: []const i32,
    axes: []const i32,
) struct { usize, usize, usize, usize, usize, usize } {
    if (starts.len > 0 and starts.len == ends.len) {
        var cs: usize = 0;
        var ce: usize = in_c;
        var hs: usize = 0;
        var he: usize = in_h;
        var ws_x: usize = 0;
        var we_x: usize = in_w;

        for (0..starts.len) |idx| {
            const axis = if (idx < axes.len) axes[idx] else @as(i32, @intCast(idx));
            const norm_axis = if (axis < 0) 3 + axis else axis;
            switch (norm_axis) {
                0 => {
                    cs = resolveCropIdx(starts[idx], in_c, false);
                    ce = resolveCropIdx(ends[idx], in_c, true);
                },
                1 => {
                    hs = resolveCropIdx(starts[idx], in_h, false);
                    he = resolveCropIdx(ends[idx], in_h, true);
                },
                2 => {
                    ws_x = resolveCropIdx(starts[idx], in_w, false);
                    we_x = resolveCropIdx(ends[idx], in_w, true);
                },
                else => {},
            }
        }
        return .{ cs, @max(ce, cs), hs, @max(he, hs), ws_x, @max(we_x, ws_x) };
    }

    const cs: usize = @intCast(@max(coffset, 0));
    const ce: usize = if (outc == -233 or outc <= 0)
        in_c -| @as(usize, @intCast(@max(coffset2, 0)))
    else
        @min(cs + @as(usize, @intCast(outc)), in_c);

    const hs: usize = @intCast(@max(hoffset, 0));
    const he: usize = if (outh == -233 or outh <= 0)
        in_h -| @as(usize, @intCast(@max(hoffset2, 0)))
    else
        @min(hs + @as(usize, @intCast(outh)), in_h);

    const ws_x: usize = @intCast(@max(woffset, 0));
    const we_x: usize = if (outw == -233 or outw <= 0)
        in_w -| @as(usize, @intCast(@max(woffset2, 0)))
    else
        @min(ws_x + @as(usize, @intCast(outw)), in_w);

    return .{ cs, @max(ce, cs), hs, @max(he, hs), ws_x, @max(we_x, ws_x) };
}

// ---------------------------------------------------------------------------
// Cached execution plan: Conv/Deconv + activation epilogue fusion
// ---------------------------------------------------------------------------

const FusedSpec = struct {
    kind: ActKind,
    p0: f32,
    p1: f32,
    slopes: ncnn.WeightSpan,

    fn resolve(self: FusedSpec, graph: *const ncnn.NcnnGraph) ActSpec {
        return .{
            .kind = self.kind,
            .p0 = self.p0,
            .p1 = self.p1,
            .slopes = if (self.kind == .prelu_chan) graph.slice(self.slopes) else &.{},
        };
    }
};

const ExecPlan = struct {
    fused: []?FusedSpec,
    skip: []bool,
    bottoms2: [][]usize,
    tops2: [][]usize,
    remaining2: []usize,
    input_blob: usize,
    output_blob: usize,

    fn deinit(self: *ExecPlan, allocator: std.mem.Allocator) void {
        allocator.free(self.fused);
        allocator.free(self.skip);
        for (self.bottoms2) |b| allocator.free(b);
        allocator.free(self.bottoms2);
        for (self.tops2) |t| allocator.free(t);
        allocator.free(self.tops2);
        allocator.free(self.remaining2);
        self.* = undefined;
    }
};

fn resolveChain(canon: []const usize, start: usize) usize {
    var b = start;
    while (canon[b] != b) {
        b = canon[b];
    }
    return b;
}

fn buildExecPlan(allocator: std.mem.Allocator, graph: *const ncnn.NcnnGraph) !ExecPlan {
    const meta = &graph.meta;
    const n = meta.layers.len;

    const fused = try allocator.alloc(?FusedSpec, n);
    errdefer allocator.free(fused);
    @memset(fused, null);

    const skip = try allocator.alloc(bool, n);
    errdefer allocator.free(skip);
    @memset(skip, false);

    const canon = try allocator.alloc(usize, meta.blob_count);
    defer allocator.free(canon);
    for (0..meta.blob_count) |i| canon[i] = i;

    var i: usize = 0;
    while (i + 1 < n) : (i += 1) {
        const own_act: i32 = switch (meta.layers[i].op) {
            .convolution => |c| c.activation_type,
            .deconvolution => |d| d.activation_type,
            else => continue,
        };
        if (own_act != 0) continue;
        if (meta.layers[i].tops.len == 0) continue;

        const top = meta.layers[i].tops[0];
        const consumers = if (top < meta.blob_consumers.len) meta.blob_consumers[top] else 0;
        if (consumers != 1) continue;

        const j = i + 1;
        const next = &meta.layers[j];
        if (next.bottoms.len == 0 or next.bottoms[0] != top) continue;
        if (next.tops.len == 0) continue;

        const spec: FusedSpec = switch (next.op) {
            .re_lu => |r| .{
                .kind = if (r.slope == 0.0) .relu else .leaky,
                .p0 = r.slope,
                .p1 = 0.0,
                .slopes = .{},
            },
            .clip => |c| .{
                .kind = .clip,
                .p0 = c.min,
                .p1 = c.max,
                .slopes = .{},
            },
            .p_re_lu => |p| if (p.num_slope <= 1) blk: {
                const s_slice = graph.slice(p.slope);
                break :blk .{
                    .kind = .leaky,
                    .p0 = if (s_slice.len > 0) s_slice[0] else 0.0,
                    .p1 = 0.0,
                    .slopes = .{},
                };
            } else .{
                .kind = .prelu_chan,
                .p0 = 0.0,
                .p1 = 0.0,
                .slopes = p.slope,
            },
            .sigmoid => .{ .kind = .sigmoid, .p0 = 0.0, .p1 = 0.0, .slopes = .{} },
            .swish => .{ .kind = .swish, .p0 = 0.0, .p1 = 0.0, .slopes = .{} },
            else => continue,
        };

        canon[next.tops[0]] = top;
        fused[i] = spec;
        skip[j] = true;
    }

    const bottoms2 = try allocator.alloc([]usize, n);
    var init_b: usize = 0;
    errdefer {
        for (bottoms2[0..init_b]) |b| allocator.free(b);
        allocator.free(bottoms2);
    }
    for (meta.layers, 0..) |layer, li| {
        const bs = try allocator.alloc(usize, layer.bottoms.len);
        for (layer.bottoms, 0..) |b, bi| bs[bi] = resolveChain(canon, b);
        bottoms2[li] = bs;
        init_b = li + 1;
    }

    const tops2 = try allocator.alloc([]usize, n);
    var init_t: usize = 0;
    errdefer {
        for (tops2[0..init_t]) |t| allocator.free(t);
        allocator.free(tops2);
    }
    for (meta.layers, 0..) |layer, li| {
        const ts = try allocator.alloc(usize, layer.tops.len);
        for (layer.tops, 0..) |t, ti| ts[ti] = resolveChain(canon, t);
        tops2[li] = ts;
        init_t = li + 1;
    }

    const input_blob = resolveChain(canon, meta.input_blob);
    const output_blob = resolveChain(canon, meta.output_blob);

    const remaining2 = try allocator.alloc(usize, meta.blob_count);
    @memset(remaining2, 0);
    for (bottoms2, 0..) |bs, l| {
        if (skip[l]) continue;
        for (bs) |b| remaining2[b] += 1;
    }

    return .{
        .fused = fused,
        .skip = skip,
        .bottoms2 = bottoms2,
        .tops2 = tops2,
        .remaining2 = remaining2,
        .input_blob = input_blob,
        .output_blob = output_blob,
    };
}

// ---------------------------------------------------------------------------
// Planar graph runner
// ---------------------------------------------------------------------------

fn runGraphPlanar(
    ws: *Workspace,
    graph: *const ncnn.NcnnGraph,
    plan: *const ExecPlan,
    feed: Tensor,
) !Tensor {
    const alloc = ws.allocator;
    const meta = &graph.meta;

    const blobs = try alloc.alloc(?Tensor, plan.remaining2.len);
    defer alloc.free(blobs);
    @memset(blobs, null);
    defer {
        for (blobs) |maybe_t| {
            if (maybe_t) |t| ws.recycle(t);
        }
    }

    const remaining = try alloc.dupe(usize, plan.remaining2);
    defer alloc.free(remaining);
    if (plan.output_blob < remaining.len) {
        remaining[plan.output_blob] += 1;
    }

    blobs[plan.input_blob] = feed;

    for (meta.layers, 0..) |layer, layer_idx| {
        if (plan.skip[layer_idx]) continue;

        const bottoms = plan.bottoms2[layer_idx];
        const tops = plan.tops2[layer_idx];

        switch (layer.op) {
            .input => {},
            .memory_data => |md| {
                const slice = graph.slice(md.data);
                const tw: usize, const th: usize, const tc: usize = if (md.d > 0)
                    .{ @intCast(@max(md.w, 1)), @intCast(@max(md.h, 1) * md.d), @intCast(@max(md.c, 1)) }
                else if (md.c > 0)
                    .{ @intCast(@max(md.w, 1)), @intCast(@max(md.h, 1)), @intCast(md.c) }
                else if (md.h > 0)
                    .{ @intCast(@max(md.w, 1)), @intCast(md.h), 1 }
                else
                    .{ 1, 1, @intCast(@max(md.w, 1)) };
                const t = try ws.allocTensor(tw, th, tc, null);
                if (slice.len != t.len) {
                    ws.recycle(t);
                    return error.InvalidGraph;
                }
                @memcpy(t.data(), slice);
                if (tops.len > 0) {
                    blobs[tops[0]] = t;
                } else {
                    ws.recycle(t);
                }
            },
            .split => {
                const b0 = bottoms[0];
                const src = blobs[b0] orelse return error.InvalidGraph;
                const can_move_last = remaining[b0] == 1 and tops.len > 0;
                const copy_count = if (can_move_last) tops.len - 1 else tops.len;
                for (0..copy_count) |idx| {
                    const dup = try ws.allocTensor(src.w, src.h, src.c, null);
                    @memcpy(dup.data(), src.dataConst());
                    blobs[tops[idx]] = dup;
                }
                if (can_move_last) {
                    const moved = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    blobs[tops[tops.len - 1]] = moved;
                    continue;
                }
            },
            .convolution => |c| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const act: ActSpec = if (plan.fused[layer_idx]) |fused|
                    fused.resolve(graph)
                else
                    ActSpec.fromNcnn(c.activation_type, c.activation_params);

                const out = try forwardConv(
                    ws,
                    inp,
                    c.num_output,
                    c.num_input,
                    c.kernel_w,
                    c.kernel_h,
                    c.dilation_w,
                    c.dilation_h,
                    c.stride_w,
                    c.stride_h,
                    c.pad_left,
                    c.pad_right,
                    c.pad_top,
                    c.pad_bottom,
                    c.pad_value,
                    c.group,
                    graph.slice(c.weight),
                    if (c.bias_term) graph.slice(c.bias) else null,
                    act,
                );
                blobs[tops[0]] = out;
            },
            .deconvolution => |d| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const act: ActSpec = if (plan.fused[layer_idx]) |fused|
                    fused.resolve(graph)
                else
                    ActSpec.fromNcnn(d.activation_type, d.activation_params);

                const out = try forwardDeconv(
                    ws,
                    inp,
                    d.num_output,
                    d.num_input,
                    d.kernel_w,
                    d.kernel_h,
                    d.dilation_w,
                    d.dilation_h,
                    d.stride_w,
                    d.stride_h,
                    d.pad_left,
                    d.pad_right,
                    d.pad_top,
                    d.pad_bottom,
                    d.output_pad_right,
                    d.output_pad_bottom,
                    d.output_w,
                    d.output_h,
                    d.group,
                    graph.slice(d.weight),
                    if (d.bias_term) graph.slice(d.bias) else null,
                    act,
                );
                blobs[tops[0]] = out;
            },
            .inner_product => |ip| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                if (inp.len != ip.num_input) return error.InvalidGraph;
                const w_mat = graph.slice(ip.weight);
                const b_vec: ?[]const f32 = if (ip.bias_term) graph.slice(ip.bias) else null;
                const out = try ws.allocTensor(1, 1, ip.num_output, null);
                const inp_data = inp.dataConst();
                for (0..ip.num_output) |oc| {
                    const row = w_mat[oc * ip.num_input .. (oc + 1) * ip.num_input];
                    var sum: f32 = if (b_vec) |bs| bs[oc] else 0.0;
                    for (row, inp_data) |wv, iv| sum += wv * iv;
                    out.backing[oc] = applyActivationScalar(sum, ip.activation_type, ip.activation_params);
                }
                blobs[tops[0]] = out;
            },
            .pooling => |p| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                if (p.global_pooling) {
                    const out = try ws.allocTensor(1, 1, inp.c, null);
                    const ps: f32 = @floatFromInt(@max(inp.planeSize(), 1));
                    for (0..inp.c) |c| {
                        const ch = inp.channel(c);
                        if (p.pooling_type == 0) {
                            var mx = -std.math.inf(f32);
                            for (ch) |v| mx = @max(mx, v);
                            out.backing[c] = mx;
                        } else {
                            var sum: f32 = 0.0;
                            for (ch) |v| sum += v;
                            out.backing[c] = sum / ps;
                        }
                    }
                    blobs[tops[0]] = out;
                } else {
                    const pl: usize = @intCast(@max(p.pad_left, 0));
                    const pr: usize = @intCast(@max(p.pad_right, 0));
                    const pt: usize = @intCast(@max(p.pad_top, 0));
                    const pb: usize = @intCast(@max(p.pad_bottom, 0));
                    if (inp.w + pl + pr < p.kernel_w or inp.h + pt + pb < p.kernel_h) return error.InvalidGraph;
                    const out_w = (inp.w + pl + pr - p.kernel_w) / p.stride_w + 1;
                    const out_h = (inp.h + pt + pb - p.kernel_h) / p.stride_h + 1;
                    const out = try ws.allocTensor(out_w, out_h, inp.c, null);
                    for (0..inp.c) |c| {
                        const src = inp.channel(c);
                        const dst = out.channelMut(c);
                        for (0..out_h) |oy| {
                            for (0..out_w) |ox| {
                                if (p.pooling_type == 0) {
                                    var mx = -std.math.inf(f32);
                                    for (0..p.kernel_h) |ky| {
                                        const sy: isize = @as(isize, @intCast(oy * p.stride_h + ky)) - @as(isize, @intCast(pt));
                                        if (sy < 0 or sy >= @as(isize, @intCast(inp.h))) continue;
                                        const sy_u: usize = @intCast(sy);
                                        for (0..p.kernel_w) |kx| {
                                            const sx: isize = @as(isize, @intCast(ox * p.stride_w + kx)) - @as(isize, @intCast(pl));
                                            if (sx >= 0 and sx < @as(isize, @intCast(inp.w))) {
                                                mx = @max(mx, src[sy_u * inp.w + @as(usize, @intCast(sx))]);
                                            }
                                        }
                                    }
                                    dst[oy * out_w + ox] = mx;
                                } else {
                                    var sum: f32 = 0.0;
                                    var count: usize = 0;
                                    for (0..p.kernel_h) |ky| {
                                        const sy: isize = @as(isize, @intCast(oy * p.stride_h + ky)) - @as(isize, @intCast(pt));
                                        for (0..p.kernel_w) |kx| {
                                            const sx: isize = @as(isize, @intCast(ox * p.stride_w + kx)) - @as(isize, @intCast(pl));
                                            if (sy >= 0 and sy < @as(isize, @intCast(inp.h)) and sx >= 0 and sx < @as(isize, @intCast(inp.w))) {
                                                sum += src[@as(usize, @intCast(sy)) * inp.w + @as(usize, @intCast(sx))];
                                                count += 1;
                                            } else if (p.avgpool_count_include_pad) {
                                                count += 1;
                                            }
                                        }
                                    }
                                    dst[oy * out_w + ox] = sum / @as(f32, @floatFromInt(@max(count, 1)));
                                }
                            }
                        }
                    }
                    blobs[tops[0]] = out;
                }
            },
            .p_re_lu => |pr| {
                const b0 = bottoms[0];
                const top0 = tops[0];
                const slopes = graph.slice(pr.slope);
                if (b0 == top0 or remaining[b0] == 1) {
                    const t = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    if (pr.num_slope <= 1) {
                        applyLeakyReluSlice(t.data(), slopes[0]);
                    } else {
                        for (0..t.c) |c| applyLeakyReluSlice(t.channelMut(c), slopes[c]);
                    }
                    blobs[top0] = t;
                    continue;
                } else {
                    const inp = blobs[b0].?;
                    const out = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    @memcpy(out.data(), inp.dataConst());
                    if (pr.num_slope <= 1) {
                        applyLeakyReluSlice(out.data(), slopes[0]);
                    } else {
                        for (0..out.c) |c| applyLeakyReluSlice(out.channelMut(c), slopes[c]);
                    }
                    blobs[top0] = out;
                }
            },
            .re_lu => |r| {
                const b0 = bottoms[0];
                const top0 = tops[0];
                if (b0 == top0 or remaining[b0] == 1) {
                    const t = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    applyLeakyReluSlice(t.data(), r.slope);
                    blobs[top0] = t;
                    continue;
                } else {
                    const inp = blobs[b0].?;
                    const out = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    @memcpy(out.data(), inp.dataConst());
                    applyLeakyReluSlice(out.data(), r.slope);
                    blobs[top0] = out;
                }
            },
            .clip => |cl| {
                const b0 = bottoms[0];
                const top0 = tops[0];
                if (b0 == top0 or remaining[b0] == 1) {
                    const t = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    applyClipSlice(t.data(), cl.min, cl.max);
                    blobs[top0] = t;
                    continue;
                } else {
                    const inp = blobs[b0].?;
                    const out = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    @memcpy(out.data(), inp.dataConst());
                    applyClipSlice(out.data(), cl.min, cl.max);
                    blobs[top0] = out;
                }
            },
            .swish => {
                const b0 = bottoms[0];
                const top0 = tops[0];
                if (b0 == top0 or remaining[b0] == 1) {
                    const t = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    for (t.data()) |*v| v.* = v.* / (1.0 + @exp(-v.*));
                    blobs[top0] = t;
                    continue;
                } else {
                    const inp = blobs[b0].?;
                    const out = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    for (out.data(), inp.dataConst()) |*d, v| d.* = v / (1.0 + @exp(-v));
                    blobs[top0] = out;
                }
            },
            .sigmoid => {
                const b0 = bottoms[0];
                const top0 = tops[0];
                if (b0 == top0 or remaining[b0] == 1) {
                    const t = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    for (t.data()) |*v| v.* = 1.0 / (1.0 + @exp(-v.*));
                    blobs[top0] = t;
                    continue;
                } else {
                    const inp = blobs[b0].?;
                    const out = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    for (out.data(), inp.dataConst()) |*d, v| d.* = 1.0 / (1.0 + @exp(-v));
                    blobs[top0] = out;
                }
            },
            .binary_op => |bo| {
                const b0 = bottoms[0];
                const a = blobs[b0] orelse return error.InvalidGraph;
                const b_opt: ?Tensor = if (bo.with_scalar) null else blk: {
                    if (bottoms.len < 2) return error.InvalidGraph;
                    const b1 = bottoms[1];
                    break :blk blobs[b1] orelse return error.InvalidGraph;
                };
                const out = try forwardBinary(ws, a, b_opt, bo.op_type, bo.with_scalar, bo.scalar);
                blobs[tops[0]] = out;
            },
            .eltwise => |ew| {
                const b0 = bottoms[0];
                const top0 = tops[0];
                const c0: f32 = if (ew.coeffs.len > 0) ew.coeffs[0] else 1.0;
                if (ew.op_type == 1 and (b0 == top0 or remaining[b0] == 1) and @abs(c0 - 1.0) < 1e-7) {
                    const out = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    for (bottoms[1..], 0..) |bi, idx| {
                        const coeff = if (idx + 1 < ew.coeffs.len) ew.coeffs[idx + 1] else 1.0;
                        const bt = blobs[bi] orelse return error.InvalidGraph;
                        saxpySlice(out.data(), bt.dataConst(), coeff);
                    }
                    blobs[top0] = out;
                } else {
                    const first = blobs[b0] orelse return error.InvalidGraph;
                    const out = try ws.allocTensor(first.w, first.h, first.c, null);
                    switch (ew.op_type) {
                        0 => {
                            @memcpy(out.data(), first.dataConst());
                            for (bottoms[1..]) |bi| {
                                const bt = blobs[bi] orelse return error.InvalidGraph;
                                zip2Slice(out.data(), bt.dataConst(), .mul);
                            }
                        },
                        2 => {
                            @memcpy(out.data(), first.dataConst());
                            for (bottoms[1..]) |bi| {
                                const bt = blobs[bi] orelse return error.InvalidGraph;
                                for (out.data(), bt.dataConst()) |*d, v| d.* = @max(d.*, v);
                            }
                        },
                        else => {
                            if (@abs(c0 - 1.0) < 1e-7) {
                                @memcpy(out.data(), first.dataConst());
                            } else {
                                for (out.data(), first.dataConst()) |*d, v| d.* = v * c0;
                            }
                            for (bottoms[1..], 0..) |bi, idx| {
                                const coeff = if (idx + 1 < ew.coeffs.len) ew.coeffs[idx + 1] else 1.0;
                                const bt = blobs[bi] orelse return error.InvalidGraph;
                                saxpySlice(out.data(), bt.dataConst(), coeff);
                            }
                        },
                    }
                    blobs[top0] = out;
                }
            },
            .concat => |cc| {
                const first = blobs[bottoms[0]] orelse return error.InvalidGraph;
                if (cc.axis == 0) {
                    var total_c: usize = 0;
                    for (bottoms) |bi| total_c += (blobs[bi] orelse return error.InvalidGraph).c;
                    const out = try ws.allocTensor(first.w, first.h, total_c, null);
                    var offset: usize = 0;
                    for (bottoms) |bi| {
                        const bt = blobs[bi].?;
                        @memcpy(out.backing[offset .. offset + bt.len], bt.dataConst());
                        offset += bt.len;
                    }
                    blobs[tops[0]] = out;
                } else if (cc.axis == 1) {
                    var total_h: usize = 0;
                    for (bottoms) |bi| total_h += (blobs[bi] orelse return error.InvalidGraph).h;
                    const out = try ws.allocTensor(first.w, total_h, first.c, null);
                    for (0..first.c) |c| {
                        var y_off: usize = 0;
                        const dst = out.channelMut(c);
                        for (bottoms) |bi| {
                            const bt = blobs[bi].?;
                            const seg_len = bt.h * bt.w;
                            @memcpy(dst[y_off * first.w .. y_off * first.w + seg_len], bt.channel(c));
                            y_off += bt.h;
                        }
                    }
                    blobs[tops[0]] = out;
                } else {
                    var total_w: usize = 0;
                    for (bottoms) |bi| total_w += (blobs[bi] orelse return error.InvalidGraph).w;
                    const out = try ws.allocTensor(total_w, first.h, first.c, null);
                    for (0..first.c) |c| {
                        const dst = out.channelMut(c);
                        for (0..first.h) |y| {
                            var x_off: usize = 0;
                            for (bottoms) |bi| {
                                const bt = blobs[bi].?;
                                const srow = bt.channel(c)[y * bt.w .. (y + 1) * bt.w];
                                @memcpy(dst[y * total_w + x_off .. y * total_w + x_off + bt.w], srow);
                                x_off += bt.w;
                            }
                        }
                    }
                    blobs[tops[0]] = out;
                }
            },
            .slice => |sl| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                if (sl.axis != 0) return error.UnsupportedLayer;
                const n_tops = tops.len;
                const sizes = try alloc.alloc(usize, n_tops);
                defer alloc.free(sizes);

                if (sl.indices.len > 0) {
                    var prev: usize = 0;
                    var write_idx: usize = 0;
                    for (sl.indices) |idx_val| {
                        if (write_idx >= n_tops) break;
                        const pos: usize = if (idx_val < 0)
                            @intCast(@max(@as(i32, @intCast(inp.c)) + idx_val, 0))
                        else
                            @min(@as(usize, @intCast(idx_val)), inp.c);
                        sizes[write_idx] = pos -| prev;
                        prev = pos;
                        write_idx += 1;
                    }
                    if (write_idx < n_tops) {
                        sizes[write_idx] = inp.c -| prev;
                        write_idx += 1;
                    }
                    while (write_idx < n_tops) : (write_idx += 1) {
                        sizes[write_idx] = 0;
                    }
                } else if (sl.slices.len > 0) {
                    var known: usize = 0;
                    var auto_cnt: usize = 0;
                    for (sl.slices) |s| {
                        if (s > 0) known += @intCast(s) else auto_cnt += 1;
                    }
                    const auto_sz = (inp.c -| known) / @max(auto_cnt, 1);
                    for (0..n_tops) |i| {
                        const s = if (i < sl.slices.len) sl.slices[i] else -1;
                        sizes[i] = if (s > 0) @intCast(s) else auto_sz;
                    }
                } else {
                    const each = inp.c / @max(n_tops, 1);
                    @memset(sizes, each);
                }

                const ps = inp.planeSize();
                var ch_off: usize = 0;
                for (tops, sizes) |top_idx, sz| {
                    const part = try ws.allocTensor(inp.w, inp.h, sz, null);
                    const src_slice = inp.backing[ch_off * ps .. (ch_off + sz) * ps];
                    @memcpy(part.data(), src_slice);
                    blobs[top_idx] = part;
                    ch_off += sz;
                }
            },
            .group_norm => |gn| {
                const b0 = bottoms[0];
                const top0 = tops[0];
                const gam: ?[]const f32 = if (gn.affine) graph.slice(gn.gamma) else null;
                const bet: ?[]const f32 = if (gn.affine) graph.slice(gn.beta) else null;
                const in_place = b0 == top0 or remaining[b0] == 1;
                const t: Tensor = if (in_place) blk: {
                    const taken = blobs[b0].?;
                    blobs[b0] = null;
                    remaining[b0] = 0;
                    break :blk taken;
                } else blk: {
                    const inp = blobs[b0].?;
                    const dup = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    @memcpy(dup.data(), inp.dataConst());
                    break :blk dup;
                };

                const ch_total = if (gn.channels > 0) gn.channels else t.c;
                const groups = @max(gn.group, 1);
                const ch_g = ch_total / groups;
                const ps = t.planeSize();
                const group_elems: f32 = @floatFromInt(ch_g * ps);

                for (0..groups) |g| {
                    const g_slice = t.backing[g * ch_g * ps .. (g + 1) * ch_g * ps];
                    var sum: f32 = 0.0;
                    for (g_slice) |v| sum += v;
                    const mean = sum / group_elems;
                    var sqsum: f32 = 0.0;
                    for (g_slice) |v| {
                        const d = v - mean;
                        sqsum += d * d;
                    }
                    const variance = sqsum / group_elems;
                    const a = 1.0 / @sqrt(variance + gn.eps);
                    const b = -mean * a;
                    if (gam != null and bet != null) {
                        const gs = gam.?;
                        const bs = bet.?;
                        for (0..ch_g) |q| {
                            const gv = gs[g * ch_g + q];
                            const bv = bs[g * ch_g + q];
                            const scale = a * gv;
                            const bias_v = b * gv + bv;
                            const plane = g_slice[q * ps .. (q + 1) * ps];
                            for (plane) |*v| v.* = v.* * scale + bias_v;
                        }
                    } else {
                        for (g_slice) |*v| v.* = v.* * a + b;
                    }
                }
                blobs[top0] = t;
                if (remaining[b0] == 0) continue;
            },
            .interp => |it| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const out = try forwardInterp(
                    ws,
                    inp,
                    it.resize_type,
                    it.height_scale,
                    it.width_scale,
                    it.output_height,
                    it.output_width,
                    it.align_corner,
                );
                blobs[tops[0]] = out;
            },
            .pixel_shuffle => |ps_op| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const r = ps_op.upscale_factor;
                if (r <= 1) {
                    const out = try ws.allocTensor(inp.w, inp.h, inp.c, null);
                    @memcpy(out.data(), inp.dataConst());
                    blobs[tops[0]] = out;
                } else {
                    const r2 = r * r;
                    if (inp.c % r2 != 0) return error.InvalidGraph;
                    const out_c = inp.c / r2;
                    const out_w = inp.w * r;
                    const out_h = inp.h * r;
                    const out = try ws.allocTensor(out_w, out_h, out_c, null);
                    for (0..out_c) |oc| {
                        const dst = out.channelMut(oc);
                        for (0..r) |sh| {
                            for (0..r) |sw| {
                                const ic = if (ps_op.mode == 0)
                                    oc * r2 + sh * r + sw
                                else
                                    (sh * r + sw) * out_c + oc;
                                const src = inp.channel(ic);
                                for (0..inp.h) |y| {
                                    const srow = src[y * inp.w .. (y + 1) * inp.w];
                                    const drow = dst[(y * r + sh) * out_w .. (y * r + sh + 1) * out_w];
                                    for (srow, 0..) |v, x| {
                                        drow[x * r + sw] = v;
                                    }
                                }
                            }
                        }
                    }
                    blobs[tops[0]] = out;
                }
            },
            .reorg => |ro| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const s = ro.stride;
                if (s == 0 or inp.w % s != 0 or inp.h % s != 0) return error.InvalidGraph;
                const out_w = inp.w / s;
                const out_h = inp.h / s;
                const s2 = s * s;
                const out_c = inp.c * s2;
                const out = try ws.allocTensor(out_w, out_h, out_c, null);
                for (0..inp.c) |ic| {
                    const src = inp.channel(ic);
                    for (0..s) |sh| {
                        for (0..s) |sw| {
                            const oc = if (ro.mode == 0)
                                ic * s2 + sh * s + sw
                            else
                                (sh * s + sw) * inp.c + ic;
                            const dst = out.channelMut(oc);
                            for (0..out_h) |oy| {
                                const srow = src[(oy * s + sh) * inp.w .. (oy * s + sh + 1) * inp.w];
                                const drow = dst[oy * out_w .. (oy + 1) * out_w];
                                for (drow, 0..) |*slot, ox| {
                                    slot.* = srow[ox * s + sw];
                                }
                            }
                        }
                    }
                }
                blobs[tops[0]] = out;
            },
            .crop => |cr| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const cs, const ce, const hs, const he, const ws_x, const we_x = resolveCropRegion(
                    inp.c,
                    inp.h,
                    inp.w,
                    cr.woffset,
                    cr.hoffset,
                    cr.coffset,
                    cr.outw,
                    cr.outh,
                    cr.outc,
                    cr.woffset2,
                    cr.hoffset2,
                    cr.coffset2,
                    cr.starts,
                    cr.ends,
                    cr.axes,
                );
                const oc = ce - cs;
                const oh = he - hs;
                const ow = we_x - ws_x;
                const out = try ws.allocTensor(ow, oh, oc, null);
                for (0..oc) |c| {
                    const src = inp.channel(cs + c);
                    const dst = out.channelMut(c);
                    for (0..oh) |y| {
                        const srow = src[(hs + y) * inp.w + ws_x .. (hs + y) * inp.w + we_x];
                        @memcpy(dst[y * ow .. (y + 1) * ow], srow);
                    }
                }
                blobs[tops[0]] = out;
            },
            .padding => |pd| {
                const b0 = bottoms[0];
                const inp = blobs[b0] orelse return error.InvalidGraph;
                const ow = inp.w + pd.left + pd.right;
                const oh = inp.h + pd.top + pd.bottom;
                const out = try ws.allocTensor(ow, oh, inp.c, pd.value);
                for (0..inp.c) |c| {
                    const src = inp.channel(c);
                    const dst = out.channelMut(c);
                    for (0..oh) |dy| {
                        const sy_i = @as(isize, @intCast(dy)) - @as(isize, @intCast(pd.top));
                        for (0..ow) |dx| {
                            const sx_i = @as(isize, @intCast(dx)) - @as(isize, @intCast(pd.left));
                            dst[dy * ow + dx] = switch (pd.pad_type) {
                                1 => blk: {
                                    const sy: usize = @intCast(std.math.clamp(sy_i, 0, @as(isize, @intCast(inp.h)) - 1));
                                    const sx: usize = @intCast(std.math.clamp(sx_i, 0, @as(isize, @intCast(inp.w)) - 1));
                                    break :blk src[sy * inp.w + sx];
                                },
                                2 => blk: {
                                    const sy = reflectIndex(sy_i, @intCast(inp.h));
                                    const sx = reflectIndex(sx_i, @intCast(inp.w));
                                    break :blk src[sy * inp.w + sx];
                                },
                                else => if (sy_i >= 0 and sy_i < @as(isize, @intCast(inp.h)) and sx_i >= 0 and sx_i < @as(isize, @intCast(inp.w)))
                                    src[@as(usize, @intCast(sy_i)) * inp.w + @as(usize, @intCast(sx_i))]
                                else
                                    pd.value,
                            };
                        }
                    }
                }
                blobs[tops[0]] = out;
            },
        }

        // Decrement consumer refcounts and recycle dead bottom blobs immediately.
        for (bottoms) |b| {
            if (remaining[b] > 0) {
                remaining[b] -= 1;
                if (remaining[b] == 0) {
                    if (blobs[b]) |dead| {
                        blobs[b] = null;
                        ws.recycle(dead);
                    }
                }
            }
        }
    }

    const out = blobs[plan.output_blob] orelse return error.InvalidGraph;
    blobs[plan.output_blob] = null;
    return out;
}

inline fn reflectIndex(idx_in: isize, len: isize) usize {
    if (len <= 1) return 0;
    var idx = idx_in;
    while (idx < 0 or idx >= len) {
        if (idx < 0) idx = -idx;
        if (idx >= len) idx = 2 * len - 2 - idx;
    }
    return @intCast(idx);
}

// ---------------------------------------------------------------------------
// Public Engine API
// ---------------------------------------------------------------------------

pub const WasmCpuEngine = struct {
    allocator: std.mem.Allocator,
    initialized: bool,
    graph_data: ?ncnn.NcnnGraph,
    owns_graph: bool,
    graph_ptr: ?*const ncnn.NcnnGraph,
    plan: ?ExecPlan,
    workspace: Workspace,
    scale_override: ?i32,
    last_error_buf: [256]u8,
    last_error_len: usize,

    pub fn init(allocator: std.mem.Allocator) WasmCpuEngine {
        return .{
            .allocator = allocator,
            .initialized = true,
            .graph_data = null,
            .owns_graph = false,
            .graph_ptr = null,
            .plan = null,
            .workspace = Workspace.init(allocator),
            .scale_override = null,
            .last_error_buf = undefined,
            .last_error_len = 0,
        };
    }

    pub fn fromGraph(allocator: std.mem.Allocator, g: *const ncnn.NcnnGraph) !WasmCpuEngine {
        var eng = init(allocator);
        errdefer eng.deinit();
        eng.graph_ptr = g;
        eng.owns_graph = false;
        eng.plan = try buildExecPlan(allocator, g);
        return eng;
    }

    pub fn deinit(self: *WasmCpuEngine) void {
        if (self.plan) |*p| {
            p.deinit(self.allocator);
            self.plan = null;
        }
        if (self.owns_graph) {
            if (self.graph_data) |*g| {
                g.deinit(self.allocator);
            }
        }
        self.graph_data = null;
        self.graph_ptr = null;
        self.workspace.deinit();
        self.* = undefined;
    }

    pub fn lastError(self: *const WasmCpuEngine) []const u8 {
        return self.last_error_buf[0..self.last_error_len];
    }

    fn setErr(self: *WasmCpuEngine, comptime fmt: []const u8, args: anytype) void {
        const res = std.fmt.bufPrint(&self.last_error_buf, fmt, args) catch |err| switch (err) {
            error.NoSpaceLeft => self.last_error_buf[0..],
        };
        self.last_error_len = res.len;
    }

    pub fn loadModelBytes(
        self: *WasmCpuEngine,
        model_name: []const u8,
        param_text: []const u8,
        bin_bytes: []const u8,
    ) !void {
        if (!self.initialized) {
            self.setErr("Engine not initialized", .{});
            return error.NotInitialized;
        }
        var parsed = ncnn.parseNcnnModel(self.allocator, model_name, param_text, bin_bytes) catch |err| {
            self.setErr("ncnn parse failed: {s}", .{@errorName(err)});
            return err;
        };
        errdefer parsed.deinit(self.allocator);

        const new_plan = try buildExecPlan(self.allocator, &parsed);

        if (self.plan) |*old_plan| old_plan.deinit(self.allocator);
        if (self.owns_graph) {
            if (self.graph_data) |*old_g| old_g.deinit(self.allocator);
        }
        self.graph_data = parsed;
        self.owns_graph = true;
        self.graph_ptr = null;
        self.plan = new_plan;
        self.scale_override = null;
        self.last_error_len = 0;
    }

    pub fn graph(self: *const WasmCpuEngine) ?*const ncnn.NcnnGraph {
        if (self.graph_data) |*g| return g;
        return self.graph_ptr;
    }

    pub fn scaleFactorDetected(self: *const WasmCpuEngine) bool {
        if (self.graph()) |g| return g.meta.scale_detected;
        return false;
    }

    pub fn getScaleFactor(self: *const WasmCpuEngine) i32 {
        if (self.graph()) |g| {
            if (g.meta.scale_detected) return g.meta.scale_factor;
        }
        if (self.scale_override) |ov| return ov;
        if (self.graph()) |g| return g.meta.scale_factor;
        return 2;
    }

    pub fn setScaleFactor(self: *WasmCpuEngine, scale: i32) void {
        if (!self.scaleFactorDetected() and scale > 0) {
            self.scale_override = scale;
        }
    }

    pub fn modelInputChannels(self: *const WasmCpuEngine) i32 {
        if (self.graph()) |g| return g.meta.input_channels;
        return 0;
    }

    /// Infer interleaved HWC float32 image (`width * height * channels`).
    pub fn process(
        self: *WasmCpuEngine,
        input: []const f32,
        output: []f32,
        width: i32,
        height: i32,
        channels: i32,
    ) !void {
        if (!self.initialized) {
            self.setErr("Engine not initialized", .{});
            return error.NotInitialized;
        }
        const g = self.graph() orelse {
            self.setErr("No model loaded", .{});
            return error.NoModelLoaded;
        };
        const p = &(self.plan orelse {
            self.setErr("No execution plan", .{});
            return error.NoModelLoaded;
        });

        if (width <= 0 or height <= 0 or channels <= 0) {
            self.setErr("Invalid input dimensions", .{});
            return error.InvalidDimensions;
        }
        const w: usize = @intCast(width);
        const h: usize = @intCast(height);
        const c: usize = @intCast(channels);
        if (input.len < w * h * c) {
            self.setErr("Input buffer too small", .{});
            return error.InputBufferTooSmall;
        }

        const scale: usize = @intCast(@max(self.getScaleFactor(), 1));
        const expected_out_w = w * scale;
        const expected_out_h = h * scale;
        if (output.len < expected_out_w * expected_out_h * c) {
            self.setErr("Output buffer too small", .{});
            return error.OutputBufferTooSmall;
        }

        const prepad: usize = @intCast(@max(g.meta.prepadding, 0));
        const align_v: usize = @intCast(@max(g.meta.input_align, 1));
        const base_w = w + 2 * prepad;
        const base_h = h + 2 * prepad;
        const extra_w = (align_v - (base_w % align_v)) % align_v;
        const extra_h = (align_v - (base_h % align_v)) % align_v;
        const feed_w = base_w + extra_w;
        const feed_h = base_h + extra_h;

        var feed = try self.workspace.allocTensor(feed_w, feed_h, c, null);
        if (prepad == 0 and extra_w == 0 and extra_h == 0) {
            if (c == 3) {
                const ps = w * h;
                const r_plane = feed.backing[0..ps];
                const g_plane = feed.backing[ps .. 2 * ps];
                const b_plane = feed.backing[2 * ps .. 3 * ps];
                for (0..ps) |i| {
                    const base = i * 3;
                    r_plane[i] = input[base];
                    g_plane[i] = input[base + 1];
                    b_plane[i] = input[base + 2];
                }
            } else {
                for (0..c) |ch| {
                    const dst = feed.channelMut(ch);
                    for (0..h) |y| {
                        const drow = dst[y * w .. (y + 1) * w];
                        const srow = input[y * w * c .. (y + 1) * w * c];
                        for (drow, 0..) |*slot, x| {
                            slot.* = srow[x * c + ch];
                        }
                    }
                }
            }
        } else {
            const w_i: isize = @intCast(w);
            const h_i: isize = @intCast(h);
            const prepad_i: isize = @intCast(prepad);
            for (0..c) |ch| {
                const dst = feed.channelMut(ch);
                for (0..feed_h) |fy| {
                    const sy: usize = @intCast(std.math.clamp(@as(isize, @intCast(fy)) - prepad_i, 0, h_i - 1));
                    const srow = input[sy * w * c .. (sy + 1) * w * c];
                    const drow = dst[fy * feed_w .. (fy + 1) * feed_w];
                    for (drow, 0..) |*slot, fx| {
                        const sx: usize = @intCast(std.math.clamp(@as(isize, @intCast(fx)) - prepad_i, 0, w_i - 1));
                        slot.* = srow[sx * c + ch];
                    }
                }
            }
        }

        // `runGraphPlanar` takes ownership of `feed` (recycles it when dead).
        const out = runGraphPlanar(&self.workspace, g, p, feed) catch |err| {
            self.setErr("graph execution failed: {s}", .{@errorName(err)});
            return err;
        };
        defer self.workspace.recycle(out);

        if (out.c != c or out.w < expected_out_w or out.h < expected_out_h) {
            self.setErr(
                "Output shape mismatch: expected={}x{}x{}, actual={}x{}x{}",
                .{ expected_out_w, expected_out_h, c, out.w, out.h, out.c },
            );
            return error.InvalidGraph;
        }

        if (c == 3 and out.w == expected_out_w and out.h == expected_out_h) {
            const ps = expected_out_w * expected_out_h;
            const r_plane = out.backing[0..ps];
            const g_plane = out.backing[ps .. 2 * ps];
            const b_plane = out.backing[2 * ps .. 3 * ps];
            for (0..ps) |i| {
                const base = i * 3;
                output[base] = r_plane[i];
                output[base + 1] = g_plane[i];
                output[base + 2] = b_plane[i];
            }
        } else {
            for (0..c) |ch| {
                const src = out.channel(ch);
                for (0..expected_out_h) |oy| {
                    const srow = src[oy * out.w .. oy * out.w + expected_out_w];
                    const drow = output[oy * expected_out_w * c .. (oy + 1) * expected_out_w * c];
                    for (srow, 0..) |v, ox| {
                        drow[ox * c + ch] = v;
                    }
                }
            }
        }
    }
};

/// Single-threaded tiled `u8` pipeline using `tile.zig` layout, extraction,
/// quadratic `w*w` seam blending, and `bicubic.zig` alpha bypass.
/// Caller owns the returned slice (`out_w * out_h * channels` bytes).
pub fn processImageU8Single(
    allocator: std.mem.Allocator,
    input_data: []const u8,
    input_width: i32,
    input_height: i32,
    channels: i32,
    engine: *WasmCpuEngine,
    tile_size: i32,
    overlap: i32,
    enable_alpha_bicubic: bool,
) ![]u8 {
    if (input_width <= 0 or input_height <= 0 or channels < 1 or channels > 4) {
        engine.setErr("Invalid input image dimensions or channels", .{});
        return error.InvalidDimensions;
    }
    const iw_u: usize = @intCast(input_width);
    const ih_u: usize = @intCast(input_height);
    const ch_u: usize = @intCast(channels);
    const expected_in = iw_u * ih_u * ch_u;
    if (input_data.len < expected_in) {
        engine.setErr("Input pixel buffer smaller than width*height*channels", .{});
        return error.InputBufferTooSmall;
    }

    const ts = @max(tile_size, 32);
    const ov = std.math.clamp(overlap, 0, ts - 1);
    const regions = try tile.computeTileLayout(allocator, input_width, input_height, ts, ov);
    defer allocator.free(regions);

    const scale = @max(engine.getScaleFactor(), 1);
    const out_w = input_width * scale;
    const out_h = input_height * scale;
    const out_w_u: usize = @intCast(out_w);
    const out_h_u: usize = @intCast(out_h);
    const out_px = out_w_u * out_h_u;

    const model_in_ch = engine.modelInputChannels();
    const alpha_split = channels == 4 and (enable_alpha_bicubic or model_in_ch == 3);
    const net_ch: i32 = if (alpha_split) 3 else channels;
    const net_ch_u: usize = @intCast(net_ch);

    var alpha_out: ?[]u8 = null;
    defer if (alpha_out) |a| allocator.free(a);
    if (alpha_split) {
        const px_in = iw_u * ih_u;
        const alpha_src = try allocator.alloc(u8, px_in);
        defer allocator.free(alpha_src);
        for (0..px_in) |i| {
            alpha_src[i] = input_data[i * 4 + 3];
        }
        const alpha_dst = try allocator.alloc(u8, out_px);
        errdefer allocator.free(alpha_dst);
        if (scale == 1) {
            @memcpy(alpha_dst, alpha_src);
        } else {
            try bicubic.bicubicScaleU8(
                allocator,
                alpha_src,
                @intCast(input_width),
                @intCast(input_height),
                alpha_dst,
                @intCast(out_w),
                @intCast(out_h),
            );
        }
        alpha_out = alpha_dst;
    }

    const accum = try allocator.alloc(f32, out_px * net_ch_u);
    defer allocator.free(accum);
    @memset(accum, 0.0);

    const weights = try allocator.alloc(f32, out_px);
    defer allocator.free(weights);
    @memset(weights, 0.0);

    var tile_in_buf: []f32 = &.{};
    defer if (tile_in_buf.len > 0) allocator.free(tile_in_buf);
    var tile_out_buf: []f32 = &.{};
    defer if (tile_out_buf.len > 0) allocator.free(tile_out_buf);

    const scale_u: usize = @intCast(scale);
    for (regions) |region| {
        const rw: usize = @intCast(region.width);
        const rh: usize = @intCast(region.height);
        const in_len = rw * rh * net_ch_u;
        if (tile_in_buf.len < in_len) {
            if (tile_in_buf.len > 0) allocator.free(tile_in_buf);
            tile_in_buf = try allocator.alloc(f32, in_len);
        }
        tile.extractTileU8(input_data, input_width, channels, net_ch, region, tile_in_buf[0..in_len]);

        const out_len = (rw * scale_u) * (rh * scale_u) * net_ch_u;
        if (tile_out_buf.len < out_len) {
            if (tile_out_buf.len > 0) allocator.free(tile_out_buf);
            tile_out_buf = try allocator.alloc(f32, out_len);
        }

        try engine.process(
            tile_in_buf[0..in_len],
            tile_out_buf[0..out_len],
            @intCast(region.width),
            @intCast(region.height),
            net_ch,
        );

        try tile.blendTile(
            allocator,
            tile_out_buf[0..out_len],
            accum,
            weights,
            out_w,
            out_h,
            net_ch,
            region,
            scale,
            ov,
        );
    }

    const out_u8 = try allocator.alloc(u8, out_px * ch_u);
    tile.finalizeBlendU8(accum, weights, alpha_out, out_u8, out_px, net_ch_u, ch_u);
    return out_u8;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

fn testPrng(seed: u64, i: usize, lo: f32, hi: f32) f32 {
    var x = seed *% 0x9E37_79B9_7F4A_7C15 +% @as(u64, i);
    x = std.math.rotl(u64, x, 24) *% 0xBF58_476D_1CE4_E5B9;
    const u: f32 = @as(f32, @floatFromInt(x >> 40)) / @as(f32, @floatFromInt(@as(u64, 1) << 24));
    return lo + u * (hi - lo);
}

const BinBuilder = struct {
    allocator: std.mem.Allocator,
    bytes: std.ArrayList(u8),

    fn init(allocator: std.mem.Allocator) BinBuilder {
        return .{ .allocator = allocator, .bytes = .empty };
    }

    fn deinit(self: *BinBuilder) void {
        self.bytes.deinit(self.allocator);
    }

    fn tagged(self: *BinBuilder, vals: []const f32) !void {
        try self.bytes.appendSlice(self.allocator, &[_]u8{ 0, 0, 0, 0 });
        try self.raw(vals);
    }

    fn raw(self: *BinBuilder, vals: []const f32) !void {
        for (vals) |v| {
            const b: [4]u8 = @bitCast(v);
            try self.bytes.appendSlice(self.allocator, &b);
        }
    }

    fn finish(self: *BinBuilder) ![]u8 {
        return self.bytes.toOwnedSlice(self.allocator);
    }
};

fn testInputChw(allocator: std.mem.Allocator, w: usize, h: usize, c: usize) ![]f32 {
    const out = try allocator.alloc(f32, w * h * c);
    for (out, 0..) |*slot, i| {
        slot.* = testPrng(0xABCD, i, -1.5, 1.5);
    }
    return out;
}

fn assertClose(a: []const f32, b: []const f32, tol: f32) !void {
    try std.testing.expectEqual(b.len, a.len);
    for (a, b) |x, y| {
        const d = @abs(x - y);
        try std.testing.expect(d <= tol);
    }
}

fn runGraphDirect(
    allocator: std.mem.Allocator,
    engine: *WasmCpuEngine,
    input: []const f32,
    w: usize,
    h: usize,
    c: usize,
) ![]f32 {
    const g = engine.graph().?;
    const feed = try engine.workspace.allocTensor(w, h, c, null);
    @memcpy(feed.data(), input);
    const out = try runGraphPlanar(&engine.workspace, g, &engine.plan.?, feed);
    defer engine.workspace.recycle(out);
    return allocator.dupe(f32, out.dataConst());
}

fn refConv(
    allocator: std.mem.Allocator,
    inp: []const f32,
    w: usize,
    h: usize,
    cin: usize,
    weights: []const f32,
    bias: []const f32,
    cout: usize,
    kw: usize,
    stride: usize,
    act_kind: enum { none, relu, prelu, sigmoid },
    slopes: []const f32,
) ![]f32 {
    const pl = (kw - 1) / 2;
    const ow = (w + 2 * pl - kw) / stride + 1;
    const oh = (h + 2 * pl - kw) / stride + 1;
    const out = try allocator.alloc(f32, cout * oh * ow);
    for (0..cout) |oc| {
        for (0..oh) |oy| {
            for (0..ow) |ox| {
                var s = bias[oc];
                for (0..cin) |ic| {
                    for (0..kw) |ky| {
                        for (0..kw) |kx| {
                            const sy = @as(isize, @intCast(oy * stride + ky)) - @as(isize, @intCast(pl));
                            const sx = @as(isize, @intCast(ox * stride + kx)) - @as(isize, @intCast(pl));
                            if (sy >= 0 and sx >= 0 and sy < @as(isize, @intCast(h)) and sx < @as(isize, @intCast(w))) {
                                const ki = ky * kw + kx;
                                s += weights[(oc * cin + ic) * kw * kw + ki] *
                                    inp[ic * h * w + @as(usize, @intCast(sy)) * w + @as(usize, @intCast(sx))];
                            }
                        }
                    }
                }
                out[oc * oh * ow + oy * ow + ox] = switch (act_kind) {
                    .none => s,
                    .relu => @max(s, 0.0),
                    .prelu => if (s < 0.0) s * slopes[oc] else s,
                    .sigmoid => 1.0 / (1.0 + @exp(-s)),
                };
            }
        }
    }
    return out;
}

test "fused conv3x3 activation matches scalar reference" {
    const alloc = std.testing.allocator;
    const W = 11;
    const H = 9;
    const CIN = 3;
    const COUT = 7;
    const K = 3;
    const inp = try testInputChw(alloc, W, H, CIN);
    defer alloc.free(inp);

    const maxk = K * K;
    const weights = try alloc.alloc(f32, COUT * CIN * maxk);
    defer alloc.free(weights);
    for (weights, 0..) |*v, i| v.* = testPrng(0x1234 +% i, i, -0.7, 0.7);

    const bias = try alloc.alloc(f32, COUT);
    defer alloc.free(bias);
    for (bias, 0..) |*v, i| v.* = testPrng(0x77, i, -0.2, 0.2);

    const slopes = try alloc.alloc(f32, COUT);
    defer alloc.free(slopes);
    for (slopes, 0..) |*v, i| v.* = 0.05 + 0.02 * @as(f32, @floatFromInt(i % 4));

    inline for (.{ "prelu", "relu", "sigmoid", "" }) |act| {
        var bb = BinBuilder.init(alloc);
        defer bb.deinit();
        try bb.tagged(weights);
        try bb.raw(bias);
        if (comptime std.mem.eql(u8, act, "prelu")) try bb.raw(slopes);
        const bin = try bb.finish();
        defer alloc.free(bin);

        const param = if (comptime std.mem.eql(u8, act, "prelu"))
            try std.fmt.allocPrint(alloc, "7767517\n3 3\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nConvolution conv0 1 1 in0 c1 0={d} 1=3 11=3 2=1 12=1 3=1 13=1 4=1 14=1 15=1 16=1 5=1 6={d}\nPReLU act 1 1 c1 c2 0={d}\n", .{ W, H, CIN, COUT, COUT * CIN * maxk, COUT })
        else if (comptime std.mem.eql(u8, act, "relu"))
            try std.fmt.allocPrint(alloc, "7767517\n3 3\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nConvolution conv0 1 1 in0 c1 0={d} 1=3 11=3 2=1 12=1 3=1 13=1 4=1 14=1 15=1 16=1 5=1 6={d}\nReLU act 1 1 c1 c2 0=0\n", .{ W, H, CIN, COUT, COUT * CIN * maxk })
        else if (comptime std.mem.eql(u8, act, "sigmoid"))
            try std.fmt.allocPrint(alloc, "7767517\n3 3\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nConvolution conv0 1 1 in0 c1 0={d} 1=3 11=3 2=1 12=1 3=1 13=1 4=1 14=1 15=1 16=1 5=1 6={d}\nSigmoid act 1 1 c1 c2\n", .{ W, H, CIN, COUT, COUT * CIN * maxk })
        else
            try std.fmt.allocPrint(alloc, "7767517\n2 2\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nConvolution conv0 1 1 in0 c1 0={d} 1=3 11=3 2=1 12=1 3=1 13=1 4=1 14=1 15=1 16=1 5=1 6={d}\n", .{ W, H, CIN, COUT, COUT * CIN * maxk });
        defer alloc.free(param);

        var eng = WasmCpuEngine.init(alloc);
        defer eng.deinit();
        try eng.loadModelBytes("fused3x3", param, bin);

        const actual = try runGraphDirect(alloc, &eng, inp, W, H, CIN);
        defer alloc.free(actual);

        const expect = try refConv(alloc, inp, W, H, CIN, weights, bias, COUT, 3, 1, if (comptime std.mem.eql(u8, act, "prelu")) .prelu else if (comptime std.mem.eql(u8, act, "relu")) .relu else if (comptime std.mem.eql(u8, act, "sigmoid")) .sigmoid else .none, slopes);
        defer alloc.free(expect);

        try assertClose(actual, expect, 1e-6);
    }
}

test "fused conv1x1 fastpath and multi kc" {
    const alloc = std.testing.allocator;
    const W = 8;
    const H = 6;
    const CIN = 140;
    const COUT = 5;
    const inp = try testInputChw(alloc, W, H, CIN);
    defer alloc.free(inp);

    const weights = try alloc.alloc(f32, COUT * CIN);
    defer alloc.free(weights);
    for (weights, 0..) |*v, i| v.* = testPrng(0x1234 +% i, i, -0.7, 0.7);

    const bias = try alloc.alloc(f32, COUT);
    defer alloc.free(bias);
    for (bias, 0..) |*v, i| v.* = testPrng(0x77, i, -0.2, 0.2);

    const slopes = try alloc.alloc(f32, COUT);
    defer alloc.free(slopes);
    for (slopes, 0..) |*v, i| v.* = 0.05 + 0.02 * @as(f32, @floatFromInt(i % 4));

    var bb = BinBuilder.init(alloc);
    defer bb.deinit();
    try bb.tagged(weights);
    try bb.raw(bias);
    try bb.raw(slopes);
    const bin = try bb.finish();
    defer alloc.free(bin);

    const param = try std.fmt.allocPrint(alloc, "7767517\n3 3\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nConvolution conv0 1 1 in0 c1 0={d} 1=1 11=1 2=1 12=1 3=1 13=1 4=0 14=0 15=0 16=0 5=1 6={d}\nPReLU act 1 1 c1 c2 0={d}\n", .{ W, H, CIN, COUT, COUT * CIN, COUT });
    defer alloc.free(param);

    var eng = WasmCpuEngine.init(alloc);
    defer eng.deinit();
    try eng.loadModelBytes("fused1x1", param, bin);

    const actual = try runGraphDirect(alloc, &eng, inp, W, H, CIN);
    defer alloc.free(actual);

    const expect = try refConv(alloc, inp, W, H, CIN, weights, bias, COUT, 1, 1, .prelu, slopes);
    defer alloc.free(expect);

    try assertClose(actual, expect, 1e-5);
}

test "depthwise conv matches reference" {
    const alloc = std.testing.allocator;
    const W = 7;
    const H = 6;
    const C = 4;
    const K = 3;
    const inp = try testInputChw(alloc, W, H, C);
    defer alloc.free(inp);

    const weights = try alloc.alloc(f32, C * K * K);
    defer alloc.free(weights);
    for (weights, 0..) |*v, i| v.* = testPrng(0x555 +% i, i, -0.5, 0.5);

    const bias = try alloc.alloc(f32, C);
    defer alloc.free(bias);
    for (bias, 0..) |*v, i| v.* = testPrng(0x88, i, -0.1, 0.1);

    var bb = BinBuilder.init(alloc);
    defer bb.deinit();
    try bb.tagged(weights);
    try bb.raw(bias);
    const bin = try bb.finish();
    defer alloc.free(bin);

    const param = try std.fmt.allocPrint(alloc, "7767517\n2 2\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nConvolutionDepthWise dw 1 1 in0 c1 0={d} 1=3 11=3 2=1 12=1 3=1 13=1 4=1 14=1 15=1 16=1 5=1 6={d} 7={d}\n", .{ W, H, C, C, C * K * K, C });
    defer alloc.free(param);

    var eng = WasmCpuEngine.init(alloc);
    defer eng.deinit();
    try eng.loadModelBytes("dwconv", param, bin);

    const actual = try runGraphDirect(alloc, &eng, inp, W, H, C);
    defer alloc.free(actual);

    const expect = try alloc.alloc(f32, C * H * W);
    defer alloc.free(expect);
    for (0..C) |c| {
        for (0..H) |oy| {
            for (0..W) |ox| {
                var s = bias[c];
                for (0..K) |ky| {
                    for (0..K) |kx| {
                        const sy: isize = @as(isize, @intCast(oy + ky)) - 1;
                        const sx: isize = @as(isize, @intCast(ox + kx)) - 1;
                        if (sy >= 0 and sx >= 0 and sy < H and sx < W) {
                            s += weights[c * 9 + ky * 3 + kx] * inp[c * H * W + @as(usize, @intCast(sy)) * W + @as(usize, @intCast(sx))];
                        }
                    }
                }
                expect[c * H * W + oy * W + ox] = s;
            }
        }
    }
    try assertClose(actual, expect, 1e-6);
}

test "deconv2x2s2 relu matches reference" {
    const alloc = std.testing.allocator;
    const W = 6;
    const H = 5;
    const CIN = 3;
    const COUT = 3;
    const inp = try testInputChw(alloc, W, H, CIN);
    defer alloc.free(inp);

    const maxk = 4;
    const weights = try alloc.alloc(f32, COUT * CIN * maxk);
    defer alloc.free(weights);
    for (weights, 0..) |*v, i| v.* = testPrng(0x4321 +% i, i, -0.7, 0.7);

    const bias = try alloc.alloc(f32, COUT);
    defer alloc.free(bias);
    for (bias, 0..) |*v, i| v.* = testPrng(0x99, i, -0.2, 0.2);

    var bb = BinBuilder.init(alloc);
    defer bb.deinit();
    try bb.tagged(weights);
    try bb.raw(bias);
    const bin = try bb.finish();
    defer alloc.free(bin);

    const param = try std.fmt.allocPrint(alloc, "7767517\n3 3\nInput in0 0 1 in0 0={d} 1={d} 2={d}\nDeconvolution deconv0 1 1 in0 c1 0={d} 1=2 11=2 3=2 13=2 5=1 6={d}\nReLU act 1 1 c1 c2 0=0\n", .{ W, H, CIN, COUT, COUT * CIN * maxk });
    defer alloc.free(param);

    var eng = WasmCpuEngine.init(alloc);
    defer eng.deinit();
    try eng.loadModelBytes("deconvtest", param, bin);

    const actual = try runGraphDirect(alloc, &eng, inp, W, H, CIN);
    defer alloc.free(actual);

    const ow = W * 2;
    const oh = H * 2;
    const expect = try alloc.alloc(f32, COUT * oh * ow);
    defer alloc.free(expect);
    for (0..COUT) |oc| {
        for (0..oh) |oy| {
            for (0..ow) |ox| {
                var s = bias[oc];
                for (0..CIN) |ic| {
                    for (0..2) |ky| {
                        for (0..2) |kx| {
                            if (oy >= ky and ox >= kx) {
                                const iy = oy - ky;
                                const ix = ox - kx;
                                if (iy % 2 == 0 and ix % 2 == 0) {
                                    const iy2 = iy / 2;
                                    const ix2 = ix / 2;
                                    if (iy2 < H and ix2 < W) {
                                        s += weights[(oc * CIN + ic) * 4 + ky * 2 + kx] * inp[ic * H * W + iy2 * W + ix2];
                                    }
                                }
                            }
                        }
                    }
                }
                expect[oc * oh * ow + oy * ow + ox] = @max(s, 0.0);
            }
        }
    }
    try assertClose(actual, expect, 1e-6);
}

test "binaryop pixelshuffle interp and multi-consumer split" {
    const alloc = std.testing.allocator;
    const W = 4;
    const H = 4;
    const C = 4;
    const inp = try testInputChw(alloc, W, H, C);
    defer alloc.free(inp);

    const param =
        \\7767517
        \\6 7
        \\Input in0 0 1 in0 0=4 1=4 2=4
        \\Split sp 1 2 in0 b0 b1
        \\BinaryOp add 2 1 b0 b1 sum 0=0
        \\PixelShuffle ps 1 1 sum ps_out 0=2 1=0
        \\Interp up 1 1 ps_out up_out 0=1 1=2.0 2=2.0
        \\Crop cr 1 1 up_out out 0=0 1=0 2=0 3=16 4=16 5=1
        \\
    ;

    var eng = WasmCpuEngine.init(alloc);
    defer eng.deinit();
    try eng.loadModelBytes("combo", param, &.{});

    const out = try runGraphDirect(alloc, &eng, inp, W, H, C);
    defer alloc.free(out);
    try std.testing.expectEqual(@as(usize, 16 * 16), out.len);
    // Input pixel (0,0) across 4 channels is doubled by BinaryOp add, shuffled to 2x2x1, then nearest-upsampled 2x to 4x4.
    const v00 = inp[0] * 2.0;
    try std.testing.expectApproxEqAbs(v00, out[0], 1e-6);
    try std.testing.expectApproxEqAbs(v00, out[1], 1e-6);
    try std.testing.expectApproxEqAbs(v00, out[16], 1e-6);
}

test "processImageU8Single end-to-end on Real-ESRGANv3-anime-x2 if present" {
    const alloc = std.testing.allocator;
    const param_path = "/home/xihale/.local/share/upix/models/Real-ESRGANv3-anime-x2.param";
    const bin_path = "/home/xihale/.local/share/upix/models/Real-ESRGANv3-anime-x2.bin";
    const io = std.testing.io;

    const param_bytes = std.Io.Dir.cwd().readFileAlloc(io, param_path, alloc, .limited(4 * 1024 * 1024)) catch return;
    defer alloc.free(param_bytes);
    const bin_bytes = std.Io.Dir.cwd().readFileAlloc(io, bin_path, alloc, .limited(64 * 1024 * 1024)) catch return;
    defer alloc.free(bin_bytes);

    var eng = WasmCpuEngine.init(alloc);
    defer eng.deinit();
    try eng.loadModelBytes("Real-ESRGANv3-anime-x2", param_bytes, bin_bytes);

    const W: i32 = 32;
    const H: i32 = 32;
    const C: i32 = 3;
    const in_u8 = try alloc.alloc(u8, @intCast(W * H * C));
    defer alloc.free(in_u8);
    for (in_u8, 0..) |*p, i| p.* = @intCast((i * 37) & 0xFF);

    const out_u8 = try processImageU8Single(alloc, in_u8, W, H, C, &eng, 32, 4, false);
    defer alloc.free(out_u8);
    try std.testing.expectEqual(@as(usize, 64 * 64 * 3), out_u8.len);

    // Also test RGBA with alpha bicubic bypass.
    const in_rgba = try alloc.alloc(u8, @intCast(W * H * 4));
    defer alloc.free(in_rgba);
    for (0..@as(usize, @intCast(W * H))) |p| {
        in_rgba[p * 4] = in_u8[p * 3];
        in_rgba[p * 4 + 1] = in_u8[p * 3 + 1];
        in_rgba[p * 4 + 2] = in_u8[p * 3 + 2];
        in_rgba[p * 4 + 3] = @intCast((p * 13) & 0xFF);
    }
    const out_rgba = try processImageU8Single(alloc, in_rgba, W, H, 4, &eng, 32, 4, true);
    defer alloc.free(out_rgba);
    try std.testing.expectEqual(@as(usize, 64 * 64 * 4), out_rgba.len);
}

test "pooling groupnorm slice concat reorg padding inner_product and interp modes" {
    const alloc = std.testing.allocator;
    // Graph: Input(4x4x4) -> Reorg(stride=2 -> 2x2x16) -> GroupNorm(4 groups, 16 ch)
    //        -> Slice(2 branches of 8ch) -> Concat(axis=0 -> 16ch)
    //        -> GlobalAvgPool(1x1x16) -> InnerProduct(16 -> 4)
    const W = 4;
    const H = 4;
    const C = 4;
    const inp = try testInputChw(alloc, W, H, C);
    defer alloc.free(inp);

    var gamma: [16]f32 = undefined;
    var beta: [16]f32 = undefined;
    for (0..16) |i| {
        gamma[i] = 1.0 + 0.05 * @as(f32, @floatFromInt(i));
        beta[i] = -0.1 + 0.01 * @as(f32, @floatFromInt(i));
    }
    var ip_w: [4 * 16]f32 = undefined;
    for (0..ip_w.len) |i| ip_w[i] = testPrng(0x2026, i, -0.5, 0.5);
    var ip_b = [_]f32{ 0.1, -0.2, 0.3, -0.4 };

    var bb = BinBuilder.init(alloc);
    defer bb.deinit();
    try bb.raw(&gamma);
    try bb.raw(&beta);
    try bb.tagged(&ip_w);
    try bb.raw(&ip_b);
    const bin = try bb.finish();
    defer alloc.free(bin);

    const param =
        \\7767517
        \\7 8
        \\Input in0 0 1 in0 0=4 1=4 2=4
        \\Reorg rg 1 1 in0 b1 0=2 1=0
        \\GroupNorm gn 1 1 b1 b2 0=4 1=16 2=0.001 3=1
        \\Slice sl 1 2 b2 s0 s1 -23300=2,8,8 1=0
        \\Concat cc 2 1 s0 s1 b3 0=0
        \\Pooling pool 1 1 b3 b4 0=1 4=1
        \\InnerProduct ip 1 1 b4 out 0=4 1=1 2=64 9=1
        \\
    ;

    var eng = WasmCpuEngine.init(alloc);
    defer eng.deinit();
    try eng.loadModelBytes("layers_combo", param, bin);

    const out = try runGraphDirect(alloc, &eng, inp, W, H, C);
    defer alloc.free(out);
    try std.testing.expectEqual(@as(usize, 4), out.len);
    for (out) |v| try std.testing.expect(v >= 0.0);

    // Also test Padding (reflect) + Interp (bilinear & bicubic) + Eltwise (sum/prod/max) + BinaryOp broadcast.
    const pad_param =
        \\7767517
        \\7 8
        \\Input in0 0 1 in0 0=4 1=4 2=4
        \\Padding pd 1 1 in0 p0 0=1 1=1 2=1 3=1 4=2 5=0.0
        \\Interp bi 1 1 p0 b1 0=2 1=0.5 2=0.5 6=0
        \\Interp bc 1 1 b1 b2 0=3 1=2.0 2=2.0 6=0
        \\Split sp 1 2 b2 x0 x1
        \\Eltwise ew 2 1 x0 x1 x2 0=1 -23301=2,0.5,0.5
        \\BinaryOp mul 2 1 x2 b2 out 0=2
        \\
    ;
    // Wait: x2 = 0.5*b2 + 0.5*b2 = b2, and b2 is consumed by sp and mul; let's split b2 cleanly via 3-way Split.
    _ = pad_param;
    const pad_param_clean =
        \\7767517
        \\7 9
        \\Input in0 0 1 in0 0=4 1=4 2=4
        \\Padding pd 1 1 in0 p0 0=1 1=1 2=1 3=1 4=2 5=0.0
        \\Interp bi 1 1 p0 b1 0=2 1=0.5 2=0.5 6=0
        \\Interp bc 1 1 b1 b2 0=3 1=2.0 2=2.0 6=0
        \\Split sp 1 3 b2 x0 x1 x_ref
        \\Eltwise ew 2 1 x0 x1 x2 0=1 -23301=2,0.5,0.5
        \\BinaryOp sub 2 1 x2 x_ref out 0=1
        \\
    ;
    var eng2 = WasmCpuEngine.init(alloc);
    defer eng2.deinit();
    try eng2.loadModelBytes("pad_interp_elt", pad_param_clean, &.{});
    const out2 = try runGraphDirect(alloc, &eng2, inp, W, H, C);
    defer alloc.free(out2);
    // `x2` == `x_ref`, so `x2 - x_ref` == 0.0 everywhere.
    for (out2) |v| try std.testing.expectApproxEqAbs(@as(f32, 0.0), v, 1e-6);
}

test "real models SPAN, Real-CUGAN, RealPLKSR infer cleanly if present" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;
    const cases = .{
        .{ "NomosUni-SPAN-x2", 2 },
        .{ "Real-CUGAN-denoise-x2", 2 },
        .{ "DeNoise-RealPLKSR-x1", 1 },
    };

    inline for (cases) |c| {
        const name = c[0];
        const expected_scale: i32 = c[1];
        const param_path = "/home/xihale/.local/share/upix/models/" ++ name ++ ".param";
        const bin_path = "/home/xihale/.local/share/upix/models/" ++ name ++ ".bin";

        if (std.Io.Dir.cwd().readFileAlloc(io, param_path, alloc, .limited(4 * 1024 * 1024))) |param_bytes| {
            defer alloc.free(param_bytes);
            if (std.Io.Dir.cwd().readFileAlloc(io, bin_path, alloc, .limited(64 * 1024 * 1024))) |bin_bytes| {
                defer alloc.free(bin_bytes);
                var eng = WasmCpuEngine.init(alloc);
                defer eng.deinit();
                try eng.loadModelBytes(name, param_bytes, bin_bytes);
                try std.testing.expectEqual(expected_scale, eng.getScaleFactor());

                const w: usize = 16;
                const h: usize = 16;
                const ch: usize = 3;
                const s: usize = @intCast(expected_scale);
                const inp = try alloc.alloc(f32, w * h * ch);
                defer alloc.free(inp);
                for (inp, 0..) |*slot, i| slot.* = @as(f32, @floatFromInt(i % 255)) / 255.0;

                const out = try alloc.alloc(f32, (w * s) * (h * s) * ch);
                defer alloc.free(out);
                try eng.process(inp, out, w, h, ch);
                for (out) |v| try std.testing.expect(!std.math.isNan(v));
            } else |_| {}
        } else |_| {}
    }
}
