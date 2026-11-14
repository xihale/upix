//! Oracle parity tests for Upix Zig WASM runtime (ported from `oracle_parity.rs`).

const std = @import("std");
const upix = @import("upix");
const abi = upix.abi;

fn getModelsDirPath(allocator: std.mem.Allocator) !?[]u8 {
    const home = std.testing.environ.getAlloc(allocator, "HOME") catch |err| switch (err) {
        error.EnvironmentVariableMissing => return null,
        else => return err,
    };
    defer allocator.free(home);
    return try std.fmt.allocPrint(allocator, "{s}/.local/share/upix/models", .{home});
}

pub fn makeOracleInput(allocator: std.mem.Allocator, size: usize) ![]u8 {
    var img = try allocator.alloc(u8, size * size * 3);
    var x: u64 = 20261014;
    for (img) |*slot| {
        x = (1103515245 * x + 12345) % (@as(u64, 1) << 31);
        slot.* = @intCast((x >> 16) & 0xFF);
    }

    const block = @max(size / 8, 6);
    var yy: usize = 6;
    while (yy < 6 + block) : (yy += 1) {
        var xx: usize = 6;
        while (xx < 6 + block) : (xx += 1) {
            const idx = (yy * size + xx) * 3;
            img[idx] = 0;
            img[idx + 1] = 0;
            img[idx + 2] = 0;
        }
        xx = size - 6 - block;
        while (xx < size - 6) : (xx += 1) {
            const idx = (yy * size + xx) * 3;
            img[idx] = 255;
            img[idx + 1] = 255;
            img[idx + 2] = 255;
        }
    }

    for (0..size) |y| {
        for (0..size) |x_pos| {
            const idx = (y * size + x_pos) * 3;
            img[idx] = @intCast((x_pos * 255) / (size - 1));
            const val: f64 = 128.0 + 110.0 * (@sin(@as(f64, @floatFromInt(x_pos)) / 7.0) * @cos(@as(f64, @floatFromInt(y)) / 9.0));
            const trunc_val: i64 = @intFromFloat(val);
            img[idx + 2] = @intCast(@as(u8, @truncate(@as(u64, @bitCast(trunc_val)))));
        }
    }
    return img;
}

pub fn computePsnrAndMaxdiff(a: []const u8, b: []const u8) struct { psnr: f64, maxdiff: f64 } {
    std.debug.assert(a.len == b.len);
    var mse: f64 = 0.0;
    var maxdiff: f64 = 0.0;
    for (a, b) |xa, xb| {
        const d = @abs(@as(f64, @floatFromInt(xa)) - @as(f64, @floatFromInt(xb)));
        if (d > maxdiff) maxdiff = d;
        mse += d * d;
    }
    mse /= @as(f64, @floatFromInt(a.len));
    const psnr = if (mse == 0.0) 99.99 else 10.0 * std.math.log10((255.0 * 255.0) / mse);
    return .{ .psnr = psnr, .maxdiff = maxdiff };
}

test "parses all installed ncnn models" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;
    const dir_path = (try getModelsDirPath(alloc)) orelse return;
    defer alloc.free(dir_path);

    var dir = std.Io.Dir.openDirAbsolute(io, dir_path, .{ .iterate = true }) catch |err| switch (err) {
        error.FileNotFound, error.NotDir => return,
        else => return err,
    };
    defer dir.close(io);

    var it = dir.iterate();
    var count: usize = 0;
    while (try it.next(io)) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".param")) continue;

        const stem = entry.name[0 .. entry.name.len - ".param".len];
        const bin_name = try std.fmt.allocPrint(alloc, "{s}.bin", .{stem});
        defer alloc.free(bin_name);

        const param_bytes = try dir.readFileAlloc(io, entry.name, alloc, .limited(16 * 1024 * 1024));
        defer alloc.free(param_bytes);

        const bin_bytes = dir.readFileAlloc(io, bin_name, alloc, .limited(128 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer alloc.free(bin_bytes);

        var graph = try upix.ncnn.parseNcnnModel(alloc, stem, param_bytes, bin_bytes);
        defer graph.deinit(alloc);

        try std.testing.expect(graph.meta.scale_factor >= 1);
        try std.testing.expect(graph.meta.layers.len > 0);
        try std.testing.expect(graph.weights.len > 0);

        const meta_json = try graph.exportMetaJson(alloc);
        defer alloc.free(meta_json);
        try std.testing.expect(meta_json.len > 10);

        count += 1;
    }
    try std.testing.expect(count > 0);
}

test "matches all golden oracle models" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;
    const dir_path = (try getModelsDirPath(alloc)) orelse return;
    defer alloc.free(dir_path);

    var models_dir = std.Io.Dir.openDirAbsolute(io, dir_path, .{}) catch |err| switch (err) {
        error.FileNotFound, error.NotDir => return,
        else => return err,
    };
    defer models_dir.close(io);

    const input = try makeOracleInput(alloc, 64);
    defer alloc.free(input);

    const cases = [_]struct {
        case_id: []const u8,
        model_name: []const u8,
        expected_scale: i32,
    }{
        .{ .case_id = "span-x2", .model_name = "NomosUni-SPAN-x2", .expected_scale = 2 },
        .{ .case_id = "compact-anime-x2", .model_name = "Real-ESRGANv3-anime-x2", .expected_scale = 2 },
        .{ .case_id = "compact-archivist-x1", .model_name = "Archivist-Medium-Compact-x1", .expected_scale = 1 },
        .{ .case_id = "realplksr-x4", .model_name = "NomosWebPhoto-RealPLKSR-x4", .expected_scale = 4 },
        .{ .case_id = "rrdb-x2plus", .model_name = "Real-ESRGAN-x2plus", .expected_scale = 2 },
        .{ .case_id = "cugan-x2", .model_name = "Real-CUGAN-no-denoise-x2", .expected_scale = 2 },
    };

    for (cases) |c| {
        const param_name = try std.fmt.allocPrint(alloc, "{s}.param", .{c.model_name});
        defer alloc.free(param_name);
        const bin_name = try std.fmt.allocPrint(alloc, "{s}.bin", .{c.model_name});
        defer alloc.free(bin_name);

        const param_bytes = models_dir.readFileAlloc(io, param_name, alloc, .limited(16 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer alloc.free(param_bytes);

        const bin_bytes = models_dir.readFileAlloc(io, bin_name, alloc, .limited(128 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer alloc.free(bin_bytes);

        const golden_path = try std.fmt.allocPrint(alloc, "../tests/model_oracle/golden/{s}.png", .{c.case_id});
        defer alloc.free(golden_path);
        const golden_png = try std.Io.Dir.cwd().readFileAlloc(io, golden_path, alloc, .limited(32 * 1024 * 1024));
        defer alloc.free(golden_png);

        var golden = try upix.codec.decodePng(alloc, golden_png);
        defer golden.deinit(alloc);

        // Exercise full C ABI path (load -> meta -> process_image_u8 -> free)
        const handle = abi.upix_model_load(
            c.model_name.ptr,
            c.model_name.len,
            param_bytes.ptr,
            param_bytes.len,
            bin_bytes.ptr,
            bin_bytes.len,
        );
        try std.testing.expect(handle != 0);
        defer abi.upix_model_free(handle);

        try std.testing.expectEqual(@as(i32, 0), abi.upix_model_meta_json(handle));
        try std.testing.expect(abi.upix_model_weights_ptr(handle) != null);
        try std.testing.expect(abi.upix_model_weights_len(handle) > 0);

        try std.testing.expectEqual(@as(i32, 0), abi.upix_model_process_image_u8(
            handle,
            input.ptr,
            input.len,
            64,
            64,
            3,
            256,
            16,
            1,
        ));
        const out_ptr = abi.upix_result_ptr() orelse return error.MissingResult;
        const out_data = out_ptr[0..abi.upix_result_len()];

        const expected_dim: u32 = @intCast(64 * c.expected_scale);
        try std.testing.expectEqual(expected_dim, golden.width);
        try std.testing.expectEqual(expected_dim, golden.height);
        try std.testing.expectEqual(@as(usize, expected_dim) * @as(usize, expected_dim) * 3, out_data.len);

        const metrics = computePsnrAndMaxdiff(out_data, golden.data);
        std.debug.print("oracle {s} ({s}): PSNR = {d:.2} dB, maxdiff = {d:.1}\n", .{
            c.case_id,
            c.model_name,
            metrics.psnr,
            metrics.maxdiff,
        });
        try std.testing.expect(metrics.psnr >= 45.0);
        try std.testing.expect(metrics.maxdiff <= 3.0);
    }
}

test "handles odd dimensions and rgba alpha bypass" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;
    const dir_path = (try getModelsDirPath(alloc)) orelse return;
    defer alloc.free(dir_path);

    var models_dir = std.Io.Dir.openDirAbsolute(io, dir_path, .{}) catch |err| switch (err) {
        error.FileNotFound, error.NotDir => return,
        else => return err,
    };
    defer models_dir.close(io);

    const cases = [_]struct { name: []const u8, scale: usize }{
        .{ .name = "Real-CUGAN-no-denoise-x2", .scale = 2 },
        .{ .name = "Real-CUGAN-denoise-x3", .scale = 3 },
        .{ .name = "Real-ESRGAN-x2plus", .scale = 2 },
    };

    const w: usize = 29;
    const h: usize = 31;
    const rgba = try alloc.alloc(u8, w * h * 4);
    defer alloc.free(rgba);
    @memset(rgba, 120);
    for (0..(w * h)) |i| {
        rgba[i * 4 + 3] = @intCast((i * 7) % 256);
    }

    for (cases) |c| {
        const param_name = try std.fmt.allocPrint(alloc, "{s}.param", .{c.name});
        defer alloc.free(param_name);
        const bin_name = try std.fmt.allocPrint(alloc, "{s}.bin", .{c.name});
        defer alloc.free(bin_name);

        const param_bytes = models_dir.readFileAlloc(io, param_name, alloc, .limited(16 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer alloc.free(param_bytes);

        const bin_bytes = models_dir.readFileAlloc(io, bin_name, alloc, .limited(128 * 1024 * 1024)) catch |err| switch (err) {
            error.FileNotFound => continue,
            else => return err,
        };
        defer alloc.free(bin_bytes);

        var eng = upix.cpu_engine.WasmCpuEngine.init(alloc);
        defer eng.deinit();
        try eng.loadModelBytes(c.name, param_bytes, bin_bytes);

        const out = try upix.cpu_engine.processImageU8Single(
            alloc,
            rgba,
            @intCast(w),
            @intCast(h),
            4,
            &eng,
            64,
            8,
            true,
        );
        defer alloc.free(out);
        try std.testing.expectEqual((w * c.scale) * (h * c.scale) * 4, out.len);
    }
}

test "codecs and sr marker roundtrip parity" {
    const alloc = std.testing.allocator;

    const model_name = "Real-ESRGANv3-anime-x2";
    try std.testing.expectEqual(@as(i32, 0), abi.upix_build_sr_xmp(model_name.ptr, model_name.len, 2, 1730000000));
    const xmp_ptr = abi.upix_result_ptr() orelse return error.MissingResult;
    const xmp = try alloc.dupe(u8, xmp_ptr[0..abi.upix_result_len()]);
    defer alloc.free(xmp);

    const px = try alloc.alloc(u8, 16 * 12 * 3);
    defer alloc.free(px);
    @memset(px, 180);

    try std.testing.expectEqual(@as(i32, 0), abi.upix_encode_png(px.ptr, px.len, 16, 12, 3, 90, -1, xmp.ptr, xmp.len));
    const png_bytes = try alloc.dupe(u8, abi.upix_result_ptr().?[0..abi.upix_result_len()]);
    defer alloc.free(png_bytes);

    var m1 = (try upix.codec.probeSrMarker(alloc, png_bytes)) orelse return error.MissingMarker;
    defer m1.deinit(alloc);
    try std.testing.expectEqualStrings(model_name, m1.model);
    try std.testing.expectEqual(@as(i32, 2), m1.scale);
    try std.testing.expectEqual(@as(i64, 1730000000), m1.ts);

    var dec_png = try upix.codec.decodePng(alloc, png_bytes);
    defer dec_png.deinit(alloc);
    try std.testing.expectEqual(@as(u32, 16), dec_png.width);
    try std.testing.expectEqual(@as(u32, 12), dec_png.height);
    try std.testing.expectEqual(@as(u32, 3), dec_png.channels);

    try std.testing.expectEqual(@as(i32, 0), abi.upix_encode_jpeg(px.ptr, px.len, 16, 12, 3, 92, 444, xmp.ptr, xmp.len));
    const jpg_bytes = try alloc.dupe(u8, abi.upix_result_ptr().?[0..abi.upix_result_len()]);
    defer alloc.free(jpg_bytes);

    var m2 = (try upix.codec.probeSrMarker(alloc, jpg_bytes)) orelse return error.MissingMarker;
    defer m2.deinit(alloc);
    try std.testing.expectEqualStrings(m1.model, m2.model);
    try std.testing.expectEqual(m1.scale, m2.scale);
    try std.testing.expectEqual(m1.ts, m2.ts);

    var dec_jpg = try upix.codec.decodeJpeg(alloc, jpg_bytes);
    defer dec_jpg.deinit(alloc);
    try std.testing.expectEqual(@as(u32, 16), dec_jpg.width);
    try std.testing.expectEqual(@as(u32, 12), dec_jpg.height);
    try std.testing.expectEqual(@as(u32, 3), dec_jpg.channels);

    const simple_webp = [_]u8{
        'R', 'I', 'F', 'F',
        18,  0,   0,   0,
        'W', 'E', 'B', 'P',
        'V', 'P', '8', 'L',
        6,   0,   0,   0,
        0x2F, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    try std.testing.expectEqual(@as(i32, 0), abi.upix_webp_attach_xmp(&simple_webp, simple_webp.len, xmp.ptr, xmp.len));
    const muxed = try alloc.dupe(u8, abi.upix_result_ptr().?[0..abi.upix_result_len()]);
    defer alloc.free(muxed);

    var m3 = (try upix.codec.probeSrMarker(alloc, muxed)) orelse return error.MissingMarker;
    defer m3.deinit(alloc);
    try std.testing.expectEqualStrings(m1.model, m3.model);
    try std.testing.expectEqual(m1.scale, m3.scale);
    try std.testing.expectEqual(m1.ts, m3.ts);
}
