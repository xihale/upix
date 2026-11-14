//! Complete ncnn `.param` + `.bin` parser, weight loader (fp32 / fp16 / int8 /
//! 256-lut quantized), param-graph scale analyzer (`model_graph.rs`), and
//! unified graph IR + JSON metadata export shared by the Zig CPU engine,
//! WebGPU WGSL runtime, and WebNN runtime.

const std = @import("std");
const catalog = @import("catalog.zig");

pub const MAX_GRAPH_SCALE_FACTOR: i32 = 16;
const MAX_REORG_STRIDE: i32 = 16;
const MAX_BLOB_COUNT: i32 = 1024;

pub const WeightSpan = struct {
    offset: usize = 0,
    len: usize = 0,
};

pub const LayerOp = union(enum) {
    input: struct {
        w: i32,
        h: i32,
        c: i32,
    },
    memory_data: struct {
        w: i32,
        h: i32,
        d: i32,
        c: i32,
        data: WeightSpan,
    },
    split: void,
    convolution: struct {
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
        bias_term: bool,
        activation_type: i32,
        activation_params: []const f32,
        weight: WeightSpan,
        bias: WeightSpan,
    },
    deconvolution: struct {
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
        output_w: i32,
        output_h: i32,
        group: usize,
        bias_term: bool,
        activation_type: i32,
        activation_params: []const f32,
        weight: WeightSpan,
        bias: WeightSpan,
    },
    inner_product: struct {
        num_output: usize,
        num_input: usize,
        bias_term: bool,
        activation_type: i32,
        activation_params: []const f32,
        weight: WeightSpan,
        bias: WeightSpan,
    },
    pooling: struct {
        pooling_type: i32, // 0 = max, 1 = avg
        kernel_w: usize,
        kernel_h: usize,
        stride_w: usize,
        stride_h: usize,
        pad_left: i32,
        pad_right: i32,
        pad_top: i32,
        pad_bottom: i32,
        global_pooling: bool,
        pad_mode: i32,
        avgpool_count_include_pad: bool,
    },
    p_re_lu: struct {
        num_slope: usize,
        slope: WeightSpan,
    },
    re_lu: struct {
        slope: f32,
    },
    clip: struct {
        min: f32,
        max: f32,
    },
    swish: void,
    sigmoid: void,
    binary_op: struct {
        op_type: i32,
        with_scalar: bool,
        scalar: f32,
    },
    eltwise: struct {
        op_type: i32, // 0 = prod, 1 = sum, 2 = max
        coeffs: []const f32,
    },
    concat: struct {
        axis: i32,
    },
    slice: struct {
        slices: []const i32,
        indices: []const i32,
        axis: i32,
    },
    group_norm: struct {
        group: usize,
        channels: usize,
        eps: f32,
        affine: bool,
        gamma: WeightSpan,
        beta: WeightSpan,
    },
    interp: struct {
        resize_type: i32, // 1 = nearest, 2 = bilinear, 3 = bicubic
        height_scale: f32,
        width_scale: f32,
        output_height: i32,
        output_width: i32,
        align_corner: bool,
    },
    pixel_shuffle: struct {
        upscale_factor: usize,
        mode: i32,
    },
    reorg: struct {
        stride: usize,
        mode: i32,
    },
    crop: struct {
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
    },
    padding: struct {
        top: usize,
        bottom: usize,
        left: usize,
        right: usize,
        pad_type: i32, // 0 = constant, 1 = replicate, 2 = reflect
        value: f32,
    },

    pub fn deinit(self: *const LayerOp, allocator: std.mem.Allocator) void {
        switch (self.*) {
            .convolution => |c| if (c.activation_params.len > 0) allocator.free(c.activation_params),
            .deconvolution => |d| if (d.activation_params.len > 0) allocator.free(d.activation_params),
            .inner_product => |ip| if (ip.activation_params.len > 0) allocator.free(ip.activation_params),
            .eltwise => |e| if (e.coeffs.len > 0) allocator.free(e.coeffs),
            .slice => |s| {
                if (s.slices.len > 0) allocator.free(s.slices);
                if (s.indices.len > 0) allocator.free(s.indices);
            },
            .crop => |cr| {
                if (cr.starts.len > 0) allocator.free(cr.starts);
                if (cr.ends.len > 0) allocator.free(cr.ends);
                if (cr.axes.len > 0) allocator.free(cr.axes);
            },
            else => {},
        }
    }
};

pub const GraphLayer = struct {
    name: []const u8,
    bottoms: []const usize,
    tops: []const usize,
    op: LayerOp,

    pub fn deinit(self: *const GraphLayer, allocator: std.mem.Allocator) void {
        allocator.free(self.name);
        if (self.bottoms.len > 0) allocator.free(self.bottoms);
        if (self.tops.len > 0) allocator.free(self.tops);
        self.op.deinit(allocator);
    }
};

pub const ExportedGraphMeta = struct {
    model_name: []const u8,
    scale_factor: i32,
    scale_detected: bool,
    input_channels: i32,
    prepadding: i32,
    input_align: i32,
    has_border_crop: bool,
    blob_count: usize,
    input_blob: usize,
    output_blob: usize,
    blob_consumers: []const usize,
    layers: []const GraphLayer,
    weight_floats: usize,
};

pub const NcnnGraph = struct {
    meta: ExportedGraphMeta,
    weights: []f32,

    pub inline fn slice(self: *const NcnnGraph, span: WeightSpan) []const f32 {
        return self.weights[span.offset .. span.offset + span.len];
    }

    pub fn deinit(self: *NcnnGraph, allocator: std.mem.Allocator) void {
        allocator.free(self.meta.model_name);
        allocator.free(self.meta.blob_consumers);
        for (self.meta.layers) |*layer| {
            layer.deinit(allocator);
        }
        allocator.free(self.meta.layers);
        allocator.free(self.weights);
        self.* = undefined;
    }

    /// Serialize `self.meta` to JSON matching `ExportedGraphMeta` in `ncnn.rs`
    /// (`serde_json` schema consumed by `webgpu.js` and `webnn.js`).
    /// Caller owns the returned slice.
    pub fn exportMetaJson(self: *const NcnnGraph, allocator: std.mem.Allocator) ![]u8 {
        var out: std.Io.Writer.Allocating = .init(allocator);
        errdefer out.deinit();
        var ws: std.json.Stringify = .{ .writer = &out.writer };

        const m = &self.meta;
        try ws.beginObject();
        try ws.objectField("model_name");
        try ws.write(m.model_name);
        try ws.objectField("scale_factor");
        try ws.write(m.scale_factor);
        try ws.objectField("scale_detected");
        try ws.write(m.scale_detected);
        try ws.objectField("input_channels");
        try ws.write(m.input_channels);
        try ws.objectField("prepadding");
        try ws.write(m.prepadding);
        try ws.objectField("input_align");
        try ws.write(m.input_align);
        try ws.objectField("has_border_crop");
        try ws.write(m.has_border_crop);
        try ws.objectField("blob_count");
        try ws.write(m.blob_count);
        try ws.objectField("input_blob");
        try ws.write(m.input_blob);
        try ws.objectField("output_blob");
        try ws.write(m.output_blob);

        try ws.objectField("blob_consumers");
        try ws.beginArray();
        for (m.blob_consumers) |c| try ws.write(c);
        try ws.endArray();

        try ws.objectField("layers");
        try ws.beginArray();
        for (m.layers) |layer| {
            try ws.beginObject();
            try ws.objectField("name");
            try ws.write(layer.name);
            try ws.objectField("bottoms");
            try ws.beginArray();
            for (layer.bottoms) |b| try ws.write(b);
            try ws.endArray();
            try ws.objectField("tops");
            try ws.beginArray();
            for (layer.tops) |t| try ws.write(t);
            try ws.endArray();
            try ws.objectField("op");
            try writeLayerOpJson(&ws, layer.op);
            try ws.endObject();
        }
        try ws.endArray();

        try ws.objectField("weight_floats");
        try ws.write(m.weight_floats);
        try ws.endObject();

        return out.toOwnedSlice();
    }
};

pub const ModelGraphInfo = struct {
    scale_factor: i32 = 2,
    scale_detected: bool = false,
    input_width: i32 = 0,
    input_height: i32 = 0,
    input_channels: i32 = 0,
    has_border_crop: bool = false,
};

pub const ParseError = error{
    EmptyParam,
    InvalidMagic,
    MissingHeader,
    InvalidLayerCount,
    MissingLayerName,
    InvalidBlobCount,
    MissingBlobName,
    InvalidParamKey,
    MissingArrayLength,
    InvalidArrayLength,
    InvalidLayerParams,
    UnsupportedLayer,
    NoLayers,
    NoOutputBlob,
    OffsetOverflow,
    UnexpectedEof,
    UnsupportedWeightTag,
} || std.mem.Allocator.Error;

/// Exact IEEE 754 binary16 -> binary32 conversion (matches `ncnn::float16_to_float32`).
pub inline fn f16ToF32(h: u16) f32 {
    const sign: u32 = (@as(u32, h) & 0x8000) << 16;
    const exp: u32 = (h >> 10) & 0x1F;
    const mant: u32 = h & 0x03FF;

    const bits: u32 = if (exp == 0) blk: {
        if (mant == 0) {
            break :blk sign;
        }
        var m = mant;
        var e: u32 = 127 - 14;
        while ((m & 0x0400) == 0) {
            m <<= 1;
            e -= 1;
        }
        m &= 0x03FF;
        break :blk sign | (e << 23) | (m << 13);
    } else if (exp == 31) blk: {
        break :blk sign | 0x7F80_0000 | (mant << 13);
    } else blk: {
        break :blk sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    };
    return @bitCast(bits);
}

fn writeJsonF32(ws: *std.json.Stringify, v: f32) !void {
    if (std.math.isFinite(v)) {
        try ws.write(v);
    } else {
        try ws.write(null);
    }
}

fn writeWeightSpanJson(ws: *std.json.Stringify, name: []const u8, span: WeightSpan) !void {
    try ws.objectField(name);
    try ws.beginObject();
    try ws.objectField("offset");
    try ws.write(span.offset);
    try ws.objectField("len");
    try ws.write(span.len);
    try ws.endObject();
}

fn writeF32SliceJson(ws: *std.json.Stringify, name: []const u8, vals: []const f32) !void {
    try ws.objectField(name);
    try ws.beginArray();
    for (vals) |v| try writeJsonF32(ws, v);
    try ws.endArray();
}

fn writeI32SliceJson(ws: *std.json.Stringify, name: []const u8, vals: []const i32) !void {
    try ws.objectField(name);
    try ws.beginArray();
    for (vals) |v| try ws.write(v);
    try ws.endArray();
}

fn writeLayerOpJson(ws: *std.json.Stringify, op: LayerOp) !void {
    try ws.beginObject();
    try ws.objectField("op");
    switch (op) {
        .input => |v| {
            try ws.write("input");
            try ws.objectField("w");
            try ws.write(v.w);
            try ws.objectField("h");
            try ws.write(v.h);
            try ws.objectField("c");
            try ws.write(v.c);
        },
        .memory_data => |v| {
            try ws.write("memory_data");
            try ws.objectField("w");
            try ws.write(v.w);
            try ws.objectField("h");
            try ws.write(v.h);
            try ws.objectField("d");
            try ws.write(v.d);
            try ws.objectField("c");
            try ws.write(v.c);
            try writeWeightSpanJson(ws, "data", v.data);
        },
        .split => {
            try ws.write("split");
        },
        .convolution => |v| {
            try ws.write("convolution");
            try ws.objectField("num_output");
            try ws.write(v.num_output);
            try ws.objectField("num_input");
            try ws.write(v.num_input);
            try ws.objectField("kernel_w");
            try ws.write(v.kernel_w);
            try ws.objectField("kernel_h");
            try ws.write(v.kernel_h);
            try ws.objectField("dilation_w");
            try ws.write(v.dilation_w);
            try ws.objectField("dilation_h");
            try ws.write(v.dilation_h);
            try ws.objectField("stride_w");
            try ws.write(v.stride_w);
            try ws.objectField("stride_h");
            try ws.write(v.stride_h);
            try ws.objectField("pad_left");
            try ws.write(v.pad_left);
            try ws.objectField("pad_right");
            try ws.write(v.pad_right);
            try ws.objectField("pad_top");
            try ws.write(v.pad_top);
            try ws.objectField("pad_bottom");
            try ws.write(v.pad_bottom);
            try ws.objectField("pad_value");
            try writeJsonF32(ws, v.pad_value);
            try ws.objectField("group");
            try ws.write(v.group);
            try ws.objectField("bias_term");
            try ws.write(v.bias_term);
            try ws.objectField("activation_type");
            try ws.write(v.activation_type);
            try writeF32SliceJson(ws, "activation_params", v.activation_params);
            try writeWeightSpanJson(ws, "weight", v.weight);
            try writeWeightSpanJson(ws, "bias", v.bias);
        },
        .deconvolution => |v| {
            try ws.write("deconvolution");
            try ws.objectField("num_output");
            try ws.write(v.num_output);
            try ws.objectField("num_input");
            try ws.write(v.num_input);
            try ws.objectField("kernel_w");
            try ws.write(v.kernel_w);
            try ws.objectField("kernel_h");
            try ws.write(v.kernel_h);
            try ws.objectField("dilation_w");
            try ws.write(v.dilation_w);
            try ws.objectField("dilation_h");
            try ws.write(v.dilation_h);
            try ws.objectField("stride_w");
            try ws.write(v.stride_w);
            try ws.objectField("stride_h");
            try ws.write(v.stride_h);
            try ws.objectField("pad_left");
            try ws.write(v.pad_left);
            try ws.objectField("pad_right");
            try ws.write(v.pad_right);
            try ws.objectField("pad_top");
            try ws.write(v.pad_top);
            try ws.objectField("pad_bottom");
            try ws.write(v.pad_bottom);
            try ws.objectField("output_pad_right");
            try ws.write(v.output_pad_right);
            try ws.objectField("output_pad_bottom");
            try ws.write(v.output_pad_bottom);
            try ws.objectField("output_w");
            try ws.write(v.output_w);
            try ws.objectField("output_h");
            try ws.write(v.output_h);
            try ws.objectField("group");
            try ws.write(v.group);
            try ws.objectField("bias_term");
            try ws.write(v.bias_term);
            try ws.objectField("activation_type");
            try ws.write(v.activation_type);
            try writeF32SliceJson(ws, "activation_params", v.activation_params);
            try writeWeightSpanJson(ws, "weight", v.weight);
            try writeWeightSpanJson(ws, "bias", v.bias);
        },
        .inner_product => |v| {
            try ws.write("inner_product");
            try ws.objectField("num_output");
            try ws.write(v.num_output);
            try ws.objectField("num_input");
            try ws.write(v.num_input);
            try ws.objectField("bias_term");
            try ws.write(v.bias_term);
            try ws.objectField("activation_type");
            try ws.write(v.activation_type);
            try writeF32SliceJson(ws, "activation_params", v.activation_params);
            try writeWeightSpanJson(ws, "weight", v.weight);
            try writeWeightSpanJson(ws, "bias", v.bias);
        },
        .pooling => |v| {
            try ws.write("pooling");
            try ws.objectField("pooling_type");
            try ws.write(v.pooling_type);
            try ws.objectField("kernel_w");
            try ws.write(v.kernel_w);
            try ws.objectField("kernel_h");
            try ws.write(v.kernel_h);
            try ws.objectField("stride_w");
            try ws.write(v.stride_w);
            try ws.objectField("stride_h");
            try ws.write(v.stride_h);
            try ws.objectField("pad_left");
            try ws.write(v.pad_left);
            try ws.objectField("pad_right");
            try ws.write(v.pad_right);
            try ws.objectField("pad_top");
            try ws.write(v.pad_top);
            try ws.objectField("pad_bottom");
            try ws.write(v.pad_bottom);
            try ws.objectField("global_pooling");
            try ws.write(v.global_pooling);
            try ws.objectField("pad_mode");
            try ws.write(v.pad_mode);
            try ws.objectField("avgpool_count_include_pad");
            try ws.write(v.avgpool_count_include_pad);
        },
        .p_re_lu => |v| {
            try ws.write("p_re_lu");
            try ws.objectField("num_slope");
            try ws.write(v.num_slope);
            try writeWeightSpanJson(ws, "slope", v.slope);
        },
        .re_lu => |v| {
            try ws.write("re_lu");
            try ws.objectField("slope");
            try writeJsonF32(ws, v.slope);
        },
        .clip => |v| {
            try ws.write("clip");
            try ws.objectField("min");
            try writeJsonF32(ws, v.min);
            try ws.objectField("max");
            try writeJsonF32(ws, v.max);
        },
        .swish => try ws.write("swish"),
        .sigmoid => try ws.write("sigmoid"),
        .binary_op => |v| {
            try ws.write("binary_op");
            try ws.objectField("op_type");
            try ws.write(v.op_type);
            try ws.objectField("with_scalar");
            try ws.write(v.with_scalar);
            try ws.objectField("scalar");
            try writeJsonF32(ws, v.scalar);
        },
        .eltwise => |v| {
            try ws.write("eltwise");
            try ws.objectField("op_type");
            try ws.write(v.op_type);
            try writeF32SliceJson(ws, "coeffs", v.coeffs);
        },
        .concat => |v| {
            try ws.write("concat");
            try ws.objectField("axis");
            try ws.write(v.axis);
        },
        .slice => |v| {
            try ws.write("slice");
            try writeI32SliceJson(ws, "slices", v.slices);
            try writeI32SliceJson(ws, "indices", v.indices);
            try ws.objectField("axis");
            try ws.write(v.axis);
        },
        .group_norm => |v| {
            try ws.write("group_norm");
            try ws.objectField("group");
            try ws.write(v.group);
            try ws.objectField("channels");
            try ws.write(v.channels);
            try ws.objectField("eps");
            try writeJsonF32(ws, v.eps);
            try ws.objectField("affine");
            try ws.write(v.affine);
            try writeWeightSpanJson(ws, "gamma", v.gamma);
            try writeWeightSpanJson(ws, "beta", v.beta);
        },
        .interp => |v| {
            try ws.write("interp");
            try ws.objectField("resize_type");
            try ws.write(v.resize_type);
            try ws.objectField("height_scale");
            try writeJsonF32(ws, v.height_scale);
            try ws.objectField("width_scale");
            try writeJsonF32(ws, v.width_scale);
            try ws.objectField("output_height");
            try ws.write(v.output_height);
            try ws.objectField("output_width");
            try ws.write(v.output_width);
            try ws.objectField("align_corner");
            try ws.write(v.align_corner);
        },
        .pixel_shuffle => |v| {
            try ws.write("pixel_shuffle");
            try ws.objectField("upscale_factor");
            try ws.write(v.upscale_factor);
            try ws.objectField("mode");
            try ws.write(v.mode);
        },
        .reorg => |v| {
            try ws.write("reorg");
            try ws.objectField("stride");
            try ws.write(v.stride);
            try ws.objectField("mode");
            try ws.write(v.mode);
        },
        .crop => |v| {
            try ws.write("crop");
            try ws.objectField("woffset");
            try ws.write(v.woffset);
            try ws.objectField("hoffset");
            try ws.write(v.hoffset);
            try ws.objectField("coffset");
            try ws.write(v.coffset);
            try ws.objectField("outw");
            try ws.write(v.outw);
            try ws.objectField("outh");
            try ws.write(v.outh);
            try ws.objectField("outc");
            try ws.write(v.outc);
            try ws.objectField("woffset2");
            try ws.write(v.woffset2);
            try ws.objectField("hoffset2");
            try ws.write(v.hoffset2);
            try ws.objectField("coffset2");
            try ws.write(v.coffset2);
            try writeI32SliceJson(ws, "starts", v.starts);
            try writeI32SliceJson(ws, "ends", v.ends);
            try writeI32SliceJson(ws, "axes", v.axes);
        },
        .padding => |v| {
            try ws.write("padding");
            try ws.objectField("top");
            try ws.write(v.top);
            try ws.objectField("bottom");
            try ws.write(v.bottom);
            try ws.objectField("left");
            try ws.write(v.left);
            try ws.objectField("right");
            try ws.write(v.right);
            try ws.objectField("pad_type");
            try ws.write(v.pad_type);
            try ws.objectField("value");
            try writeJsonF32(ws, v.value);
        },
    }
    try ws.endObject();
}

// ============================================================================
// Param-graph analyzer (port of `upix-core/src/model_graph.rs`)
// ============================================================================

fn stoiPrefix(s: []const u8) ?i32 {
    const t = std.mem.trimStart(u8, s, " \t\r\n");
    if (t.len == 0) return null;
    var sign: i64 = 1;
    var idx: usize = 0;
    if (t[0] == '+') {
        idx = 1;
    } else if (t[0] == '-') {
        sign = -1;
        idx = 1;
    }
    const digits_start = idx;
    while (idx < t.len and std.ascii.isDigit(t[idx])) : (idx += 1) {}
    if (idx == digits_start) return null;

    var value: i64 = 0;
    for (t[digits_start..idx]) |b| {
        value = value * 10 + @as(i64, b - '0');
        if (value > std.math.maxInt(i32) + 1) return null;
    }
    value *= sign;
    if (value > std.math.maxInt(i32) or value < std.math.minInt(i32)) return null;
    return @intCast(value);
}

fn stodPrefix(s: []const u8) ?f64 {
    const t = std.mem.trimStart(u8, s, " \t\r\n");
    if (t.len == 0) return null;
    var idx: usize = 0;
    if (t[0] == '+' or t[0] == '-') idx += 1;

    var seen_digit = false;
    var seen_dot = false;
    while (idx < t.len) {
        const b = t[idx];
        if (std.ascii.isDigit(b)) {
            seen_digit = true;
            idx += 1;
        } else if (b == '.' and !seen_dot) {
            seen_dot = true;
            idx += 1;
        } else {
            break;
        }
    }
    if (!seen_digit) return null;
    const mantissa_end = idx;
    if (mantissa_end < t.len and (t[mantissa_end] == 'e' or t[mantissa_end] == 'E')) {
        var e = mantissa_end + 1;
        if (e < t.len and (t[e] == '+' or t[e] == '-')) e += 1;
        const exp_start = e;
        while (e < t.len and std.ascii.isDigit(t[e])) : (e += 1) {}
        if (e > exp_start) {
            if (std.fmt.parseFloat(f64, t[0..e])) |v| {
                return v;
            } else |_| {}
        }
    }
    return std.fmt.parseFloat(f64, t[0..mantissa_end]) catch null;
}

fn paramI(text: []const u8, key: []const u8) ?i32 {
    const pos = std.mem.indexOf(u8, text, key) orelse return null;
    return stoiPrefix(text[pos + key.len ..]);
}

fn paramD(text: []const u8, key: []const u8, default_val: f64) f64 {
    const pos = std.mem.indexOf(u8, text, key) orelse return default_val;
    return stodPrefix(text[pos + key.len ..]) orelse default_val;
}

const ParsedLayerLine = struct {
    line: []const u8,
    type_: []const u8,
    bottoms: [][]const u8,
    tops: [][]const u8,

    fn deinit(self: *const ParsedLayerLine, allocator: std.mem.Allocator) void {
        allocator.free(self.bottoms);
        allocator.free(self.tops);
    }
};

fn parseLayerLine(allocator: std.mem.Allocator, line: []const u8) !?ParsedLayerLine {
    var tokens = std.mem.tokenizeAny(u8, line, " \t\r");
    const type_ = tokens.next() orelse return null;
    _ = tokens.next() orelse return null; // name
    const bc_str = tokens.next() orelse return null;
    const tc_str = tokens.next() orelse return null;
    const bottom_count = std.fmt.parseInt(i32, bc_str, 10) catch return null;
    const top_count = std.fmt.parseInt(i32, tc_str, 10) catch return null;
    if (bottom_count < 0 or bottom_count > MAX_BLOB_COUNT or top_count < 0 or top_count > MAX_BLOB_COUNT) {
        return null;
    }

    const bc: usize = @intCast(bottom_count);
    const tc: usize = @intCast(top_count);
    const bottoms = try allocator.alloc([]const u8, bc);
    errdefer allocator.free(bottoms);
    for (0..bc) |i| {
        bottoms[i] = tokens.next() orelse {
            allocator.free(bottoms);
            return null;
        };
    }
    const tops = try allocator.alloc([]const u8, tc);
    errdefer allocator.free(tops);
    for (0..tc) |i| {
        tops[i] = tokens.next() orelse {
            allocator.free(tops);
            allocator.free(bottoms);
            return null;
        };
    }
    return .{
        .line = line,
        .type_ = type_,
        .bottoms = bottoms,
        .tops = tops,
    };
}

const BlobRatio = struct {
    top: []const u8,
    val: i32,
};

const ProducerInfo = struct {
    type_: []const u8,
    ratio: i32,
    bottoms: [][]const u8,
};

pub fn detectFromParamStr(allocator: std.mem.Allocator, content: []const u8) !ModelGraphInfo {
    var info: ModelGraphInfo = .{};

    var lines_it = std.mem.splitScalar(u8, content, '\n');
    while (lines_it.next()) |raw_line| {
        const line = std.mem.trimEnd(u8, raw_line, "\r");
        if (std.mem.startsWith(u8, line, "Input")) {
            if (paramI(line, " 0=")) |w| info.input_width = w;
            if (paramI(line, " 1=")) |h| info.input_height = h;
            if (paramI(line, " 2=")) |c| info.input_channels = c;
        }
    }

    var interp_product: f64 = 1.0;
    var found_interp = false;
    var head_reorg_stride: i32 = 0;
    var upsampler_seen = false;
    var deconv_scale: i32 = 0;

    var deconvs: std.ArrayList(BlobRatio) = .empty;
    defer deconvs.deinit(allocator);
    var pixelshuffles: std.ArrayList(BlobRatio) = .empty;
    defer pixelshuffles.deinit(allocator);

    var net_output_blob: []const u8 = "";
    var blob_consumers = std.StringHashMap(u8).init(allocator);
    defer blob_consumers.deinit();

    var lines_it2 = std.mem.splitScalar(u8, content, '\n');
    while (lines_it2.next()) |raw_line| {
        const line = std.mem.trimEnd(u8, raw_line, "\r");
        if (try parseLayerLine(allocator, line)) |l| {
            defer l.deinit(allocator);
            if (l.tops.len > 0) {
                net_output_blob = l.tops[l.tops.len - 1];
            }
            for (l.bottoms) |b| {
                const gop = try blob_consumers.getOrPut(b);
                if (!gop.found_existing) gop.value_ptr.* = 0;
                gop.value_ptr.* |= 1;
            }
            if (std.mem.eql(u8, l.type_, "Split")) {
                for (l.bottoms) |b| {
                    const gop = try blob_consumers.getOrPut(b);
                    if (!gop.found_existing) gop.value_ptr.* = 0;
                    gop.value_ptr.* |= 2;
                }
            }
            if (std.mem.eql(u8, l.type_, "Reorg")) {
                const stride = paramI(l.line, " 0=") orelse -1;
                const mode = paramI(l.line, " 1=") orelse -1;
                if (!upsampler_seen and stride > 1 and stride <= MAX_REORG_STRIDE and mode <= 0) {
                    head_reorg_stride = stride;
                }
            } else if (std.mem.eql(u8, l.type_, "Deconvolution")) {
                const stride = paramI(l.line, " 3=") orelse -1;
                if (stride >= 1 and stride <= MAX_GRAPH_SCALE_FACTOR) {
                    const top = if (l.tops.len > 0) l.tops[l.tops.len - 1] else "";
                    try deconvs.append(allocator, .{ .top = top, .val = stride });
                    upsampler_seen = upsampler_seen or (stride > 1);
                }
            } else if (std.mem.eql(u8, l.type_, "PixelShuffle")) {
                const ratio = paramI(l.line, " 0=") orelse -1;
                if (ratio >= 1 and ratio <= MAX_GRAPH_SCALE_FACTOR) {
                    const top = if (l.tops.len > 0) l.tops[l.tops.len - 1] else "";
                    try pixelshuffles.append(allocator, .{ .top = top, .val = ratio });
                    upsampler_seen = upsampler_seen or (ratio > 1);
                }
            }
        }

        if (std.mem.startsWith(u8, line, "Interp")) {
            if (paramD(line, " 0=", 1.0) == 2.0) continue;
            if (std.mem.indexOf(u8, line, " 1=") == null) continue;
            const h_scale = paramD(line, " 1=", 1.0);
            if (!(h_scale > 0.0)) continue;
            found_interp = true;
            interp_product *= h_scale;
            if (h_scale > 1.0) upsampler_seen = true;
        }
        if (std.mem.startsWith(u8, line, "Crop")) {
            info.has_border_crop = true;
        }
        if (deconv_scale == 0 and std.mem.startsWith(u8, line, "Deconvolution")) {
            if (paramI(line, " 0=") == 3) {
                if (paramI(line, " 3=")) |stride| {
                    if (stride >= 1 and stride <= MAX_GRAPH_SCALE_FACTOR) {
                        deconv_scale = stride;
                    }
                }
            }
        }
    }

    var interp_scale: i32 = 0;
    if (found_interp) {
        const detected = interp_product + 0.5;
        if (detected >= 1.0 and detected <= @as(f64, @floatFromInt(MAX_GRAPH_SCALE_FACTOR))) {
            interp_scale = @intFromFloat(detected);
        }
    }

    var ps_trunk_detected = false;
    if (interp_scale >= 1) {
        info.scale_factor = interp_scale;
    } else if (deconv_scale >= 1) {
        info.scale_factor = deconv_scale;
    } else if (pixelshuffles.items.len > 0) {
        var trunk: i64 = 1;
        var found_trunk = false;
        for (deconvs.items) |d| {
            const c = blob_consumers.get(d.top) orelse 0;
            if ((c & 1) != 0 and (c & 2) != 0) {
                trunk *= d.val;
                found_trunk = true;
            }
        }
        if (found_trunk) {
            for (pixelshuffles.items) |ps| {
                if (std.mem.eql(u8, ps.top, net_output_blob)) {
                    trunk *= ps.val;
                    if (trunk >= 1 and trunk <= MAX_GRAPH_SCALE_FACTOR) {
                        info.scale_factor = @intCast(trunk);
                        ps_trunk_detected = true;
                    }
                    break;
                }
            }
        } else {
            var producers = std.StringHashMap(ProducerInfo).init(allocator);
            defer {
                var it = producers.valueIterator();
                while (it.next()) |p| allocator.free(p.bottoms);
                producers.deinit();
            }

            var lines_it3 = std.mem.splitScalar(u8, content, '\n');
            while (lines_it3.next()) |raw_line| {
                const line = std.mem.trimEnd(u8, raw_line, "\r");
                const l = (try parseLayerLine(allocator, line)) orelse continue;
                defer allocator.free(l.tops);
                // Transfer ownership of l.bottoms only if at least one top inserts it;
                // otherwise free l.bottoms. If multiple tops, duplicate bottoms for each.
                var ratio: i32 = 0;
                if (std.mem.eql(u8, l.type_, "PixelShuffle")) {
                    const r = paramI(l.line, " 0=") orelse 0;
                    if (r >= 1 and r <= MAX_GRAPH_SCALE_FACTOR) ratio = r;
                }
                if (l.tops.len == 0) {
                    allocator.free(l.bottoms);
                    continue;
                }
                for (l.tops, 0..) |t, ti| {
                    const bottoms_copy = if (ti == 0) l.bottoms else try allocator.dupe([]const u8, l.bottoms);
                    const gop = try producers.getOrPut(t);
                    if (gop.found_existing) {
                        allocator.free(gop.value_ptr.bottoms);
                    }
                    gop.value_ptr.* = .{
                        .type_ = l.type_,
                        .ratio = ratio,
                        .bottoms = bottoms_copy,
                    };
                }
            }

            var frontier: std.ArrayList([]const u8) = .empty;
            defer frontier.deinit(allocator);
            try frontier.append(allocator, net_output_blob);

            var seen_blob = std.StringHashMap(void).init(allocator);
            defer seen_blob.deinit();

            var ps_scale: i64 = 1;
            var any_trunk_ps = false;
            while (frontier.pop()) |blob| {
                const sgop = try seen_blob.getOrPut(blob);
                if (sgop.found_existing) continue;

                const prod = producers.get(blob) orelse continue;
                if (std.mem.eql(u8, prod.type_, "PixelShuffle") and prod.ratio >= 1) {
                    ps_scale *= prod.ratio;
                    any_trunk_ps = true;
                    if (ps_scale > MAX_GRAPH_SCALE_FACTOR) break;
                }
                for (prod.bottoms) |b| {
                    try frontier.append(allocator, b);
                }
            }
            if (any_trunk_ps and ps_scale >= 1 and ps_scale <= MAX_GRAPH_SCALE_FACTOR) {
                info.scale_factor = @intCast(ps_scale);
                ps_trunk_detected = true;
            }
        }
    }

    if (head_reorg_stride > 1 and @rem(info.scale_factor, head_reorg_stride) == 0) {
        info.scale_factor = @divTrunc(info.scale_factor, head_reorg_stride);
    }
    info.scale_detected = (interp_scale >= 1) or (deconv_scale >= 1) or ps_trunk_detected;
    return info;
}

// ============================================================================
// ParamDict & BinReader
// ============================================================================

const ParamDict = struct {
    scalars: std.AutoHashMap(i32, []const u8),
    arrays: std.AutoHashMap(i32, [][]const u8),
    allocator: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) ParamDict {
        return .{
            .scalars = std.AutoHashMap(i32, []const u8).init(allocator),
            .arrays = std.AutoHashMap(i32, [][]const u8).init(allocator),
            .allocator = allocator,
        };
    }

    fn deinit(self: *ParamDict) void {
        self.scalars.deinit();
        var it = self.arrays.valueIterator();
        while (it.next()) |arr| self.allocator.free(arr.*);
        self.arrays.deinit();
    }

    fn parse(allocator: std.mem.Allocator, tokens: *std.mem.TokenIterator(u8, .any)) !ParamDict {
        var pd = ParamDict.init(allocator);
        errdefer pd.deinit();

        while (tokens.next()) |tok| {
            const eq = std.mem.indexOfScalar(u8, tok, '=') orelse continue;
            const k_str = tok[0..eq];
            const v_str = tok[eq + 1 ..];
            const key = std.fmt.parseInt(i32, k_str, 10) catch return ParseError.InvalidParamKey;
            if (key <= -23300) {
                const real_id = -key - 23300;
                var parts = std.mem.splitScalar(u8, v_str, ',');
                const len_str = parts.next() orelse return ParseError.MissingArrayLength;
                const len = std.fmt.parseInt(usize, len_str, 10) catch return ParseError.InvalidArrayLength;
                var vals: std.ArrayList([]const u8) = .empty;
                errdefer vals.deinit(allocator);
                try vals.ensureTotalCapacity(allocator, len);
                var count: usize = 0;
                while (count < len) : (count += 1) {
                    const p = parts.next() orelse break;
                    vals.appendAssumeCapacity(p);
                }
                const owned = try vals.toOwnedSlice(allocator);
                const gop = try pd.arrays.getOrPut(real_id);
                if (gop.found_existing) allocator.free(gop.value_ptr.*);
                gop.value_ptr.* = owned;
            } else {
                try pd.scalars.put(key, v_str);
            }
        }
        return pd;
    }

    fn i(self: *const ParamDict, id: i32, default_val: i32) i32 {
        const s = self.scalars.get(id) orelse return default_val;
        if (std.fmt.parseInt(i32, s, 10)) |v| {
            return v;
        } else |_| {
            const fv = std.fmt.parseFloat(f64, s) catch return default_val;
            if (!std.math.isFinite(fv)) return default_val;
            return @intFromFloat(fv);
        }
    }

    fn f(self: *const ParamDict, id: i32, default_val: f32) f32 {
        const s = self.scalars.get(id) orelse return default_val;
        return std.fmt.parseFloat(f32, s) catch default_val;
    }

    fn arrI(self: *const ParamDict, allocator: std.mem.Allocator, id: i32) ![]const i32 {
        const arr = self.arrays.get(id) orelse return &.{};
        if (arr.len == 0) return &.{};
        var out: std.ArrayList(i32) = .empty;
        errdefer out.deinit(allocator);
        for (arr) |s| {
            const v = std.fmt.parseInt(i32, s, 10) catch continue;
            try out.append(allocator, v);
        }
        return out.toOwnedSlice(allocator);
    }

    fn arrF(self: *const ParamDict, allocator: std.mem.Allocator, id: i32) ![]const f32 {
        const arr = self.arrays.get(id) orelse return &.{};
        if (arr.len == 0) return &.{};
        var out: std.ArrayList(f32) = .empty;
        errdefer out.deinit(allocator);
        for (arr) |s| {
            const v = std.fmt.parseFloat(f32, s) catch continue;
            try out.append(allocator, v);
        }
        return out.toOwnedSlice(allocator);
    }
};

const BinReader = struct {
    data: []const u8,
    pos: usize = 0,

    inline fn align4(n: usize) usize {
        return (n + 3) & ~@as(usize, 3);
    }

    fn readBytes(self: *BinReader, len: usize) ParseError![]const u8 {
        const end = std.math.add(usize, self.pos, len) catch return ParseError.OffsetOverflow;
        if (end > self.data.len) return ParseError.UnexpectedEof;
        const s = self.data[self.pos..end];
        self.pos = end;
        return s;
    }

    fn loadF32Raw(
        self: *BinReader,
        allocator: std.mem.Allocator,
        count: usize,
        bank: *std.ArrayList(f32),
    ) ParseError!WeightSpan {
        if (count == 0) return .{ .offset = bank.items.len, .len = 0 };
        const byte_len = std.math.mul(usize, count, 4) catch return ParseError.OffsetOverflow;
        const bytes = try self.readBytes(byte_len);
        const offset = bank.items.len;
        try bank.ensureUnusedCapacity(allocator, count);
        for (0..count) |k| {
            const b = bytes[k * 4 ..][0..4];
            const bits = std.mem.readInt(u32, b, .little);
            bank.appendAssumeCapacity(@bitCast(bits));
        }
        return .{ .offset = offset, .len = count };
    }

    fn loadTagged(
        self: *BinReader,
        allocator: std.mem.Allocator,
        count: usize,
        bank: *std.ArrayList(f32),
    ) ParseError!WeightSpan {
        if (count == 0) return .{ .offset = bank.items.len, .len = 0 };
        const tag_bytes = try self.readBytes(4);
        const tag = std.mem.readInt(u32, tag_bytes[0..4], .little);
        const flag_sum: u32 = @as(u32, tag_bytes[0]) + tag_bytes[1] + tag_bytes[2] + tag_bytes[3];

        const offset = bank.items.len;
        try bank.ensureUnusedCapacity(allocator, count);

        if (tag == 0x0130_6B47) {
            // IEEE 754 float16 weights, 4-byte padded
            const raw_len = std.math.mul(usize, count, 2) catch return ParseError.OffsetOverflow;
            const aligned = align4(raw_len);
            const raw = try self.readBytes(aligned);
            for (0..count) |k| {
                const h = std.mem.readInt(u16, raw[k * 2 ..][0..2], .little);
                bank.appendAssumeCapacity(f16ToF32(h));
            }
        } else if (tag == 0x000D_4B38) {
            // int8 weights, 4-byte padded
            const aligned = align4(count);
            const raw = try self.readBytes(aligned);
            for (raw[0..count]) |b| {
                const signed: i8 = @bitCast(b);
                bank.appendAssumeCapacity(@floatFromInt(signed));
            }
        } else if (flag_sum != 0) {
            // 256-entry float32 quantization LUT + uint8 indices
            const lut_bytes = try self.readBytes(256 * 4);
            var lut: [256]f32 = undefined;
            for (0..256) |k| {
                const bits = std.mem.readInt(u32, lut_bytes[k * 4 ..][0..4], .little);
                lut[k] = @bitCast(bits);
            }
            const aligned = align4(count);
            const idx_bytes = try self.readBytes(aligned);
            for (idx_bytes[0..count]) |idx| {
                bank.appendAssumeCapacity(lut[idx]);
            }
        } else if (tag_bytes[0] == 0) {
            // Raw float32 weights
            const byte_len = std.math.mul(usize, count, 4) catch return ParseError.OffsetOverflow;
            const raw = try self.readBytes(byte_len);
            for (0..count) |k| {
                const bits = std.mem.readInt(u32, raw[k * 4 ..][0..4], .little);
                bank.appendAssumeCapacity(@bitCast(bits));
            }
        } else {
            return ParseError.UnsupportedWeightTag;
        }

        return .{ .offset = offset, .len = count };
    }
};

fn nonNegUsize(v: i32) usize {
    return if (v > 0) @intCast(v) else 0;
}

/// Parse an ncnn `.param` text + `.bin` weight buffer into an `NcnnGraph`
/// containing metadata, layer IR, and contiguous `f32` weights.
pub fn parseNcnnModel(
    allocator: std.mem.Allocator,
    model_name: []const u8,
    param_text: []const u8,
    bin_bytes: []const u8,
) ParseError!NcnnGraph {
    var line_it = std.mem.splitScalar(u8, param_text, '\n');

    const nextNonEmpty = struct {
        fn next(it: *std.mem.SplitIterator(u8, .scalar)) ?[]const u8 {
            while (it.next()) |raw| {
                const trimmed = std.mem.trim(u8, raw, " \t\r");
                if (trimmed.len > 0 and trimmed[0] != '#') return trimmed;
            }
            return null;
        }
    }.next;

    const magic = nextNonEmpty(&line_it) orelse return ParseError.EmptyParam;
    if (!std.mem.eql(u8, magic, "7767517")) return ParseError.InvalidMagic;

    const counts_line = nextNonEmpty(&line_it) orelse return ParseError.MissingHeader;
    var count_toks = std.mem.tokenizeAny(u8, counts_line, " \t\r");
    const layer_count_str = count_toks.next() orelse return ParseError.InvalidLayerCount;
    _ = std.fmt.parseInt(usize, layer_count_str, 10) catch return ParseError.InvalidLayerCount;
    const declared_blobs: u32 = if (count_toks.next()) |s|
        (std.fmt.parseInt(u32, s, 10) catch 64)
    else
        64;

    var blob_map = std.StringHashMap(usize).init(allocator);
    defer blob_map.deinit();
    try blob_map.ensureTotalCapacity(declared_blobs);

    var reader: BinReader = .{ .data = bin_bytes };
    var weights: std.ArrayList(f32) = .empty;
    errdefer weights.deinit(allocator);
    try weights.ensureTotalCapacity(allocator, bin_bytes.len / 2 + 64);

    var layers: std.ArrayList(GraphLayer) = .empty;
    errdefer {
        for (layers.items) |*l| l.deinit(allocator);
        layers.deinit(allocator);
    }

    var input_blob: ?usize = null;

    while (nextNonEmpty(&line_it)) |line| {
        var toks = std.mem.tokenizeAny(u8, line, " \t\r");
        const op_type = toks.next() orelse continue;
        const layer_name_raw = toks.next() orelse return ParseError.MissingLayerName;
        const bc_str = toks.next() orelse return ParseError.InvalidBlobCount;
        const tc_str = toks.next() orelse return ParseError.InvalidBlobCount;
        const bottom_count = std.fmt.parseInt(usize, bc_str, 10) catch return ParseError.InvalidBlobCount;
        const top_count = std.fmt.parseInt(usize, tc_str, 10) catch return ParseError.InvalidBlobCount;

        const bottoms = try allocator.alloc(usize, bottom_count);
        errdefer allocator.free(bottoms);
        for (0..bottom_count) |bi| {
            const b = toks.next() orelse return ParseError.MissingBlobName;
            const gop = try blob_map.getOrPut(b);
            if (!gop.found_existing) gop.value_ptr.* = blob_map.count() - 1;
            bottoms[bi] = gop.value_ptr.*;
        }

        const tops = try allocator.alloc(usize, top_count);
        errdefer allocator.free(tops);
        for (0..top_count) |ti| {
            const t = toks.next() orelse return ParseError.MissingBlobName;
            const gop = try blob_map.getOrPut(t);
            if (!gop.found_existing) gop.value_ptr.* = blob_map.count() - 1;
            tops[ti] = gop.value_ptr.*;
        }

        var pd = try ParamDict.parse(allocator, &toks);
        defer pd.deinit();

        const op: LayerOp = if (std.mem.eql(u8, op_type, "Input")) blk: {
            if (tops.len > 0 and input_blob == null) {
                input_blob = tops[0];
            }
            break :blk .{ .input = .{
                .w = pd.i(0, 0),
                .h = pd.i(1, 0),
                .c = pd.i(2, 0),
            } };
        } else if (std.mem.eql(u8, op_type, "MemoryData")) blk: {
            const w = pd.i(0, 0);
            const h = pd.i(1, 0);
            const d = pd.i(11, 0);
            const c = pd.i(2, 0);
            const load_type = pd.i(21, 1);
            const elem_count: usize = if (d != 0)
                nonNegUsize(@max(w, 1)) * nonNegUsize(@max(h, 1)) * nonNegUsize(d) * nonNegUsize(@max(c, 1))
            else if (c != 0)
                nonNegUsize(@max(w, 1)) * nonNegUsize(@max(h, 1)) * nonNegUsize(c)
            else if (h != 0)
                nonNegUsize(@max(w, 1)) * nonNegUsize(h)
            else if (w != 0)
                nonNegUsize(w)
            else
                1;
            const data = if (load_type == 1)
                try reader.loadF32Raw(allocator, elem_count, &weights)
            else
                try reader.loadTagged(allocator, elem_count, &weights);
            break :blk .{ .memory_data = .{ .w = w, .h = h, .d = d, .c = c, .data = data } };
        } else if (std.mem.eql(u8, op_type, "Split")) blk: {
            break :blk .split;
        } else if (std.mem.eql(u8, op_type, "Convolution") or std.mem.eql(u8, op_type, "ConvolutionDepthWise")) blk: {
            const num_output = nonNegUsize(pd.i(0, 0));
            const kernel_w = nonNegUsize(pd.i(1, 0));
            const kernel_h = nonNegUsize(pd.i(11, @intCast(kernel_w)));
            const dilation_w = nonNegUsize(@max(pd.i(2, 1), 1));
            const dilation_h = nonNegUsize(@max(pd.i(12, @intCast(dilation_w)), 1));
            const stride_w = nonNegUsize(@max(pd.i(3, 1), 1));
            const stride_h = nonNegUsize(@max(pd.i(13, @intCast(stride_w)), 1));
            const pad_left = pd.i(4, 0);
            const pad_top = pd.i(14, pad_left);
            const pad_right = pd.i(15, pad_left);
            const pad_bottom = pd.i(16, pad_top);
            const pad_value = pd.f(18, 0.0);
            const bias_term = pd.i(5, 0) != 0;
            const weight_data_size = nonNegUsize(pd.i(6, 0));
            const group: usize = if (std.mem.eql(u8, op_type, "ConvolutionDepthWise"))
                nonNegUsize(@max(pd.i(7, 1), 1))
            else
                1;
            const activation_type = pd.i(9, 0);
            const activation_params = try pd.arrF(allocator, 10);
            errdefer if (activation_params.len > 0) allocator.free(activation_params);

            const maxk = kernel_w * kernel_h;
            if (num_output == 0 or maxk == 0 or weight_data_size == 0) {
                return ParseError.InvalidLayerParams;
            }
            const num_input = (weight_data_size / num_output / maxk) * group;
            const weight = try reader.loadTagged(allocator, weight_data_size, &weights);
            const bias = if (bias_term)
                try reader.loadF32Raw(allocator, num_output, &weights)
            else
                WeightSpan{};

            break :blk .{ .convolution = .{
                .num_output = num_output,
                .num_input = num_input,
                .kernel_w = kernel_w,
                .kernel_h = kernel_h,
                .dilation_w = dilation_w,
                .dilation_h = dilation_h,
                .stride_w = stride_w,
                .stride_h = stride_h,
                .pad_left = pad_left,
                .pad_right = pad_right,
                .pad_top = pad_top,
                .pad_bottom = pad_bottom,
                .pad_value = pad_value,
                .group = group,
                .bias_term = bias_term,
                .activation_type = activation_type,
                .activation_params = activation_params,
                .weight = weight,
                .bias = bias,
            } };
        } else if (std.mem.eql(u8, op_type, "Deconvolution") or std.mem.eql(u8, op_type, "DeconvolutionDepthWise")) blk: {
            const num_output = nonNegUsize(pd.i(0, 0));
            const kernel_w = nonNegUsize(pd.i(1, 0));
            const kernel_h = nonNegUsize(pd.i(11, @intCast(kernel_w)));
            const dilation_w = nonNegUsize(@max(pd.i(2, 1), 1));
            const dilation_h = nonNegUsize(@max(pd.i(12, @intCast(dilation_w)), 1));
            const stride_w = nonNegUsize(@max(pd.i(3, 1), 1));
            const stride_h = nonNegUsize(@max(pd.i(13, @intCast(stride_w)), 1));
            const pad_left = pd.i(4, 0);
            const pad_top = pd.i(14, pad_left);
            const pad_right = pd.i(15, pad_left);
            const pad_bottom = pd.i(16, pad_top);
            const output_pad_right = pd.i(18, 0);
            const output_pad_bottom = pd.i(19, output_pad_right);
            const output_w = pd.i(20, 0);
            const output_h = pd.i(21, output_w);
            const bias_term = pd.i(5, 0) != 0;
            const weight_data_size = nonNegUsize(pd.i(6, 0));
            const group: usize = if (std.mem.eql(u8, op_type, "DeconvolutionDepthWise"))
                nonNegUsize(@max(pd.i(7, 1), 1))
            else
                1;
            const activation_type = pd.i(9, 0);
            const activation_params = try pd.arrF(allocator, 10);
            errdefer if (activation_params.len > 0) allocator.free(activation_params);

            const maxk = kernel_w * kernel_h;
            if (num_output == 0 or maxk == 0 or weight_data_size == 0) {
                return ParseError.InvalidLayerParams;
            }
            const num_input = (weight_data_size / num_output / maxk) * group;
            const weight = try reader.loadTagged(allocator, weight_data_size, &weights);
            const bias = if (bias_term)
                try reader.loadF32Raw(allocator, num_output, &weights)
            else
                WeightSpan{};

            break :blk .{ .deconvolution = .{
                .num_output = num_output,
                .num_input = num_input,
                .kernel_w = kernel_w,
                .kernel_h = kernel_h,
                .dilation_w = dilation_w,
                .dilation_h = dilation_h,
                .stride_w = stride_w,
                .stride_h = stride_h,
                .pad_left = pad_left,
                .pad_right = pad_right,
                .pad_top = pad_top,
                .pad_bottom = pad_bottom,
                .output_pad_right = output_pad_right,
                .output_pad_bottom = output_pad_bottom,
                .output_w = output_w,
                .output_h = output_h,
                .group = group,
                .bias_term = bias_term,
                .activation_type = activation_type,
                .activation_params = activation_params,
                .weight = weight,
                .bias = bias,
            } };
        } else if (std.mem.eql(u8, op_type, "InnerProduct")) blk: {
            const num_output = nonNegUsize(pd.i(0, 0));
            const bias_term = pd.i(1, 0) != 0;
            const weight_data_size = nonNegUsize(pd.i(2, 0));
            const activation_type = pd.i(9, 0);
            const activation_params = try pd.arrF(allocator, 10);
            errdefer if (activation_params.len > 0) allocator.free(activation_params);
            if (num_output == 0 or weight_data_size == 0) {
                return ParseError.InvalidLayerParams;
            }
            const num_input = weight_data_size / num_output;
            const weight = try reader.loadTagged(allocator, weight_data_size, &weights);
            const bias = if (bias_term)
                try reader.loadF32Raw(allocator, num_output, &weights)
            else
                WeightSpan{};

            break :blk .{ .inner_product = .{
                .num_output = num_output,
                .num_input = num_input,
                .bias_term = bias_term,
                .activation_type = activation_type,
                .activation_params = activation_params,
                .weight = weight,
                .bias = bias,
            } };
        } else if (std.mem.eql(u8, op_type, "Pooling")) blk: {
            const pooling_type = pd.i(0, 0);
            const kernel_w = nonNegUsize(pd.i(1, 0));
            const kernel_h = nonNegUsize(pd.i(11, @intCast(kernel_w)));
            const stride_w = nonNegUsize(@max(pd.i(2, 1), 1));
            const stride_h = nonNegUsize(@max(pd.i(12, @intCast(stride_w)), 1));
            const pad_left = pd.i(3, 0);
            const pad_top = pd.i(13, pad_left);
            const pad_right = pd.i(14, pad_left);
            const pad_bottom = pd.i(15, pad_top);
            const global_pooling = pd.i(4, 0) != 0;
            const pad_mode = pd.i(5, 0);
            const avgpool_count_include_pad = pd.i(6, 0) != 0;

            break :blk .{ .pooling = .{
                .pooling_type = pooling_type,
                .kernel_w = kernel_w,
                .kernel_h = kernel_h,
                .stride_w = stride_w,
                .stride_h = stride_h,
                .pad_left = pad_left,
                .pad_right = pad_right,
                .pad_top = pad_top,
                .pad_bottom = pad_bottom,
                .global_pooling = global_pooling,
                .pad_mode = pad_mode,
                .avgpool_count_include_pad = avgpool_count_include_pad,
            } };
        } else if (std.mem.eql(u8, op_type, "PReLU")) blk: {
            const num_slope = nonNegUsize(pd.i(0, 0));
            const slope = try reader.loadF32Raw(allocator, num_slope, &weights);
            break :blk .{ .p_re_lu = .{ .num_slope = num_slope, .slope = slope } };
        } else if (std.mem.eql(u8, op_type, "ReLU")) blk: {
            break :blk .{ .re_lu = .{ .slope = pd.f(0, 0.0) } };
        } else if (std.mem.eql(u8, op_type, "Clip")) blk: {
            break :blk .{ .clip = .{
                .min = pd.f(0, -std.math.inf(f32)),
                .max = pd.f(1, std.math.inf(f32)),
            } };
        } else if (std.mem.eql(u8, op_type, "Swish")) blk: {
            break :blk .swish;
        } else if (std.mem.eql(u8, op_type, "HardSwish")) {
            return ParseError.UnsupportedLayer;
        } else if (std.mem.eql(u8, op_type, "Sigmoid")) blk: {
            break :blk .sigmoid;
        } else if (std.mem.eql(u8, op_type, "BinaryOp")) blk: {
            break :blk .{ .binary_op = .{
                .op_type = pd.i(0, 0),
                .with_scalar = pd.i(1, 0) != 0,
                .scalar = pd.f(2, 0.0),
            } };
        } else if (std.mem.eql(u8, op_type, "Eltwise")) blk: {
            const coeffs = try pd.arrF(allocator, 1);
            break :blk .{ .eltwise = .{
                .op_type = pd.i(0, 0),
                .coeffs = coeffs,
            } };
        } else if (std.mem.eql(u8, op_type, "Concat")) blk: {
            break :blk .{ .concat = .{ .axis = pd.i(0, 0) } };
        } else if (std.mem.eql(u8, op_type, "Slice")) blk: {
            const slices = try pd.arrI(allocator, 0);
            errdefer if (slices.len > 0) allocator.free(slices);
            const axis = pd.i(1, 0);
            const indices = try pd.arrI(allocator, 2);
            break :blk .{ .slice = .{
                .slices = slices,
                .indices = indices,
                .axis = axis,
            } };
        } else if (std.mem.eql(u8, op_type, "GroupNorm")) blk: {
            const group = nonNegUsize(@max(pd.i(0, 1), 1));
            const channels = nonNegUsize(pd.i(1, 0));
            const eps = pd.f(2, 0.001);
            const affine = pd.i(3, 1) != 0;
            const gamma = if (affine) try reader.loadF32Raw(allocator, channels, &weights) else WeightSpan{};
            const beta = if (affine) try reader.loadF32Raw(allocator, channels, &weights) else WeightSpan{};
            break :blk .{ .group_norm = .{
                .group = group,
                .channels = channels,
                .eps = eps,
                .affine = affine,
                .gamma = gamma,
                .beta = beta,
            } };
        } else if (std.mem.eql(u8, op_type, "Interp")) blk: {
            break :blk .{ .interp = .{
                .resize_type = pd.i(0, 1),
                .height_scale = pd.f(1, 1.0),
                .width_scale = pd.f(2, 1.0),
                .output_height = pd.i(3, 0),
                .output_width = pd.i(4, 0),
                .align_corner = pd.i(6, 0) != 0,
            } };
        } else if (std.mem.eql(u8, op_type, "PixelShuffle")) blk: {
            break :blk .{ .pixel_shuffle = .{
                .upscale_factor = nonNegUsize(@max(pd.i(0, 1), 1)),
                .mode = pd.i(1, 0),
            } };
        } else if (std.mem.eql(u8, op_type, "Reorg")) blk: {
            break :blk .{ .reorg = .{
                .stride = nonNegUsize(@max(pd.i(0, 1), 1)),
                .mode = pd.i(1, 0),
            } };
        } else if (std.mem.eql(u8, op_type, "Crop")) blk: {
            const starts = try pd.arrI(allocator, 9);
            errdefer if (starts.len > 0) allocator.free(starts);
            const ends = try pd.arrI(allocator, 10);
            errdefer if (ends.len > 0) allocator.free(ends);
            const axes = try pd.arrI(allocator, 11);
            break :blk .{ .crop = .{
                .woffset = pd.i(0, 0),
                .hoffset = pd.i(1, 0),
                .coffset = pd.i(2, 0),
                .outw = pd.i(3, 0),
                .outh = pd.i(4, 0),
                .outc = pd.i(5, 0),
                .woffset2 = pd.i(6, 0),
                .hoffset2 = pd.i(7, 0),
                .coffset2 = pd.i(8, 0),
                .starts = starts,
                .ends = ends,
                .axes = axes,
            } };
        } else if (std.mem.eql(u8, op_type, "Padding")) blk: {
            break :blk .{ .padding = .{
                .top = nonNegUsize(pd.i(0, 0)),
                .bottom = nonNegUsize(pd.i(1, 0)),
                .left = nonNegUsize(pd.i(2, 0)),
                .right = nonNegUsize(pd.i(3, 0)),
                .pad_type = pd.i(4, 0),
                .value = pd.f(5, 0.0),
            } };
        } else {
            return ParseError.UnsupportedLayer;
        };

        // Ensure op is cleaned up if layer_name dupe or layers.append fails
        errdefer op.deinit(allocator);
        const layer_name = try allocator.dupe(u8, layer_name_raw);
        errdefer allocator.free(layer_name);

        try layers.append(allocator, .{
            .name = layer_name,
            .bottoms = bottoms,
            .tops = tops,
            .op = op,
        });
    }

    const blob_count = blob_map.count();
    if (blob_count == 0 or layers.items.len == 0) {
        return ParseError.NoLayers;
    }

    const resolved_input_blob = input_blob orelse 0;
    const blob_consumers = try allocator.alloc(usize, blob_count);
    errdefer allocator.free(blob_consumers);
    @memset(blob_consumers, 0);
    for (layers.items) |layer| {
        for (layer.bottoms) |b| {
            blob_consumers[b] += 1;
        }
    }

    var output_blob: ?usize = null;
    var ri: usize = layers.items.len;
    while (ri > 0) {
        ri -= 1;
        const tops = layers.items[ri].tops;
        if (tops.len > 0) {
            output_blob = tops[tops.len - 1];
            break;
        }
    }
    const resolved_output_blob = output_blob orelse return ParseError.NoOutputBlob;

    const graph_info = try detectFromParamStr(allocator, param_text);
    var scale_factor = graph_info.scale_factor;
    var scale_detected = graph_info.scale_detected;
    if (!scale_detected) {
        if (catalog.scaleFromFilename(model_name)) |from_name| {
            scale_factor = from_name;
            scale_detected = true;
        }
    }

    const prepadding: i32 = if (graph_info.has_border_crop) switch (scale_factor) {
        2 => 18,
        3 => 14,
        4 => 19,
        else => 0,
    } else 0;

    var head_reorg_align: i32 = 1;
    for (layers.items) |l| {
        switch (l.op) {
            .reorg => |r| if (r.stride > 1) {
                head_reorg_align = @intCast(r.stride);
                break;
            },
            .convolution, .deconvolution, .interp, .pixel_shuffle => {
                head_reorg_align = 1;
                break;
            },
            else => {},
        }
    }

    const input_align: i32 = if (prepadding > 0)
        (if (scale_factor == 3) @as(i32, 4) else @as(i32, 2))
    else
        head_reorg_align;

    const input_channels: i32 = if (graph_info.input_channels > 0) graph_info.input_channels else 3;

    const owned_name = try allocator.dupe(u8, model_name);
    errdefer allocator.free(owned_name);
    const owned_layers = try layers.toOwnedSlice(allocator);
    errdefer {
        for (owned_layers) |*l| l.deinit(allocator);
        allocator.free(owned_layers);
    }
    const owned_weights = try weights.toOwnedSlice(allocator);

    return .{
        .meta = .{
            .model_name = owned_name,
            .scale_factor = scale_factor,
            .scale_detected = scale_detected,
            .input_channels = input_channels,
            .prepadding = prepadding,
            .input_align = input_align,
            .has_border_crop = graph_info.has_border_crop,
            .blob_count = blob_count,
            .input_blob = resolved_input_blob,
            .output_blob = resolved_output_blob,
            .blob_consumers = blob_consumers,
            .layers = owned_layers,
            .weight_floats = owned_weights.len,
        },
        .weights = owned_weights,
    };
}

// ============================================================================
// Tests
// ============================================================================

test "f16ToF32 handles normal, subnormal, zero, inf, nan" {
    const testing = std.testing;
    try testing.expectEqual(@as(f32, 0.0), f16ToF32(0x0000));
    try testing.expectEqual(@as(f32, -0.0), f16ToF32(0x8000));
    try testing.expectEqual(@as(f32, 1.0), f16ToF32(0x3C00));
    try testing.expectEqual(@as(f32, -2.0), f16ToF32(0xC000));
    try testing.expectEqual(@as(f32, 65504.0), f16ToF32(0x7BFF));
    // Smallest positive subnormal fp16: 2^-24
    try testing.expectApproxEqAbs(@as(f32, 5.9604645e-8), f16ToF32(0x0001), 1e-12);
    try testing.expect(std.math.isPositiveInf(f16ToF32(0x7C00)));
    try testing.expect(std.math.isNan(f16ToF32(0x7E00)));
}

test "detectFromParamStr matches model_graph.rs test suite" {
    const testing = std.testing;

    // 1. Interp product 2x * 2x = 4x
    {
        const p =
            \\776 1
            \\Input data 0 1 data 0=64 1=64 2=3
            \\Convolution conv1 1 1 data out1 0=64 1=3 11=3 6=1728 7=1
            \\Interp up1 1 1 out1 up1out 0=1 1=2.000000e+00 2=2.000000e+00
            \\Interp up2 1 1 up1out up2out 0=1 1=2.000000e+00 2=2.000000e+00
            \\Convolution convout 1 1 up2out out 0=3 1=3 11=3 6=81 7=1
        ;
        const info = try detectFromParamStr(testing.allocator, p);
        try testing.expectEqual(@as(i32, 4), info.scale_factor);
        try testing.expect(info.scale_detected);
        try testing.expectEqual(@as(i32, 64), info.input_width);
        try testing.expectEqual(@as(i32, 64), info.input_height);
        try testing.expectEqual(@as(i32, 3), info.input_channels);
    }

    // 2. Interp 4x + 0.5x downsample = 2x
    {
        const p =
            \\10 1
            \\Input data 0 1 data 0=16 1=16 2=3
            \\Interp up 1 1 data upout 0=1 1=4.000000e+00 2=4.000000e+00
            \\Interp down 1 1 upout downout 0=1 1=5.000000e-01 2=5.000000e-01
            \\Convolution out 1 1 downout final 0=3 1=3 11=3 6=81 7=1
        ;
        const info = try detectFromParamStr(testing.allocator, p);
        try testing.expectEqual(@as(i32, 2), info.scale_factor);
        try testing.expect(info.scale_detected);
    }

    // 3. Deconv image-facing scale
    {
        const p =
            \\10 1
            \\Input data 0 1 data
            \\Convolution c1 1 1 data c1out 0=16 1=3 11=3 6=432 7=1
            \\Deconvolution up 1 1 c1out upout 0=3 1=4 11=4 3=2 6=768 7=1
        ;
        const info = try detectFromParamStr(testing.allocator, p);
        try testing.expectEqual(@as(i32, 2), info.scale_factor);
        try testing.expect(info.scale_detected);
    }

    // 4. Pure PixelShuffle BFS (SPAN)
    {
        const p =
            \\10 1
            \\Input data 0 1 data
            \\Convolution body 1 1 data bodyout 0=64 1=3 11=3 6=1728 7=1
            \\Convolution psmain 1 1 bodyout psinput 0=48 1=3 11=3 6=82944 7=1
            \\PixelShuffle ps 1 1 psinput psout 0=4
            \\Convolution tail 1 1 psout imgout 0=3 1=3 11=3 6=81 7=1
        ;
        const info = try detectFromParamStr(testing.allocator, p);
        try testing.expectEqual(@as(i32, 4), info.scale_factor);
        try testing.expect(info.scale_detected);
    }

    // 5. Head Reorg divides scale
    {
        const p =
            \\10 1
            \\Input data 0 1 data
            \\Reorg shuf 1 1 data shufout 0=2 1=0
            \\Convolution c1 1 1 shufout c1out 0=12 1=3 11=3 6=324 7=1
            \\Interp up 1 1 c1out upout 0=1 1=2.000000e+00 2=2.000000e+00
            \\Convolution out 1 1 upout final 0=3 1=3 11=3 6=81 7=1
        ;
        const info = try detectFromParamStr(testing.allocator, p);
        try testing.expectEqual(@as(i32, 1), info.scale_factor);
        try testing.expect(info.scale_detected);
    }

    // 6. Hostile blob count rejected safely
    {
        const p =
            \\10 1
            \\Split boom 1 2000000000 data out
        ;
        const info = try detectFromParamStr(testing.allocator, p);
        try testing.expectEqual(@as(i32, 2), info.scale_factor);
        try testing.expect(!info.scale_detected);
    }
}

test "parseNcnnModel parses synthetic fp32, fp16, int8 weights and meta JSON" {
    const testing = std.testing;
    const param =
        \\7767517
        \\3 3
        \\Input data 0 1 data 0=0 1=0 2=3
        \\Convolution conv1 1 1 data conv1 0=2 1=1 11=1 5=1 6=6 9=3 -23310=2,0.0,6.0
        \\ReLU relu1 1 1 conv1 out 0=0.1
    ;
    // Build .bin: fp16 tag 0x01306B47 + 6 fp16s (1.0 .. 6.0) + 2 raw f32 biases (0.5, -0.5)
    var bin: std.ArrayList(u8) = .empty;
    defer bin.deinit(testing.allocator);

    var tag_buf: [4]u8 = undefined;
    std.mem.writeInt(u32, &tag_buf, 0x0130_6B47, .little);
    try bin.appendSlice(testing.allocator, &tag_buf);

    // fp16 encodings for 1.0, 2.0, 3.0, 4.0, 5.0, 6.0
    const fp16_vals = [_]u16{ 0x3C00, 0x4000, 0x4200, 0x4400, 0x4500, 0x4600 };
    for (fp16_vals) |h| {
        var b: [2]u8 = undefined;
        std.mem.writeInt(u16, &b, h, .little);
        try bin.appendSlice(testing.allocator, &b);
    }
    // 6 * 2 = 12 bytes -> already 4-byte aligned! Now 2 raw f32 biases:
    const biases = [_]f32{ 0.5, -0.5 };
    for (biases) |bv| {
        var b: [4]u8 = undefined;
        std.mem.writeInt(u32, &b, @bitCast(bv), .little);
        try bin.appendSlice(testing.allocator, &b);
    }

    var graph = try parseNcnnModel(testing.allocator, "Synthetic-x2", param, bin.items);
    defer graph.deinit(testing.allocator);

    try testing.expectEqual(@as(i32, 2), graph.meta.scale_factor);
    try testing.expectEqual(@as(i32, 3), graph.meta.input_channels);
    try testing.expectEqual(@as(usize, 3), graph.meta.layers.len);
    try testing.expectEqual(@as(usize, 8), graph.weights.len);
    try testing.expectEqual(@as(f32, 1.0), graph.weights[0]);
    try testing.expectEqual(@as(f32, 6.0), graph.weights[5]);
    try testing.expectEqual(@as(f32, 0.5), graph.weights[6]);
    try testing.expectEqual(@as(f32, -0.5), graph.weights[7]);

    const json = try graph.exportMetaJson(testing.allocator);
    defer testing.allocator.free(json);
    try testing.expect(std.mem.indexOf(u8, json, "\"op\":\"convolution\"") != null);
    try testing.expect(std.mem.indexOf(u8, json, "\"op\":\"re_lu\"") != null);
    try testing.expect(std.mem.indexOf(u8, json, "\"activation_params\":[0,6]") != null);
}

test "parseNcnnModel parses real models on disk if present" {
    const testing = std.testing;
    const cases = [_]struct {
        name: []const u8,
        param_path: []const u8,
        bin_path: []const u8,
        expected_scale: i32,
        expected_crop: bool,
    }{
        .{
            .name = "Real-ESRGANv3-anime-x2",
            .param_path = "/home/xihale/.local/share/upix/models/Real-ESRGANv3-anime-x2.param",
            .bin_path = "/home/xihale/.local/share/upix/models/Real-ESRGANv3-anime-x2.bin",
            .expected_scale = 2,
            .expected_crop = false,
        },
        .{
            .name = "NomosUni-SPAN-x2",
            .param_path = "/home/xihale/.local/share/upix/models/NomosUni-SPAN-x2.param",
            .bin_path = "/home/xihale/.local/share/upix/models/NomosUni-SPAN-x2.bin",
            .expected_scale = 2,
            .expected_crop = false,
        },
        .{
            .name = "Real-CUGAN-denoise-x2",
            .param_path = "/home/xihale/.local/share/upix/models/Real-CUGAN-denoise-x2.param",
            .bin_path = "/home/xihale/.local/share/upix/models/Real-CUGAN-denoise-x2.bin",
            .expected_scale = 2,
            .expected_crop = true,
        },
        .{
            .name = "DeNoise-RealPLKSR-x1",
            .param_path = "/home/xihale/.local/share/upix/models/DeNoise-RealPLKSR-x1.param",
            .bin_path = "/home/xihale/.local/share/upix/models/DeNoise-RealPLKSR-x1.bin",
            .expected_scale = 1,
            .expected_crop = false,
        },
        .{
            .name = "Real-ESRGAN-x2plus",
            .param_path = "/home/xihale/.local/share/upix/models/Real-ESRGAN-x2plus.param",
            .bin_path = "/home/xihale/.local/share/upix/models/Real-ESRGAN-x2plus.bin",
            .expected_scale = 2,
            .expected_crop = false,
        },
    };

    const io = std.testing.io;
    for (cases) |c| {
        const param_bytes = std.Io.Dir.cwd().readFileAlloc(io, c.param_path, testing.allocator, .limited(16 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer testing.allocator.free(param_bytes);

        const bin_bytes = std.Io.Dir.cwd().readFileAlloc(io, c.bin_path, testing.allocator, .limited(128 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer testing.allocator.free(bin_bytes);

        var graph = try parseNcnnModel(testing.allocator, c.name, param_bytes, bin_bytes);
        defer graph.deinit(testing.allocator);

        try testing.expectEqual(c.expected_scale, graph.meta.scale_factor);
        try testing.expectEqual(c.expected_crop, graph.meta.has_border_crop);
        try testing.expect(graph.weights.len > 0);
        try testing.expectEqual(graph.weights.len, graph.meta.weight_floats);

        const meta_json = try graph.exportMetaJson(testing.allocator);
        defer testing.allocator.free(meta_json);
        var parsed = try std.json.parseFromSlice(std.json.Value, testing.allocator, meta_json, .{});
        defer parsed.deinit();
        try testing.expectEqualStrings(c.name, parsed.value.object.get("model_name").?.string);
    }
}
