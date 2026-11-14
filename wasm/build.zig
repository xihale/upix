const std = @import("std");

fn buildWasmExe(
    b: *std.Build,
    name: []const u8,
    optimize: std.builtin.Optimize,
    simd: bool,
) *std.Build.Step.Compile {
    const query: std.Target.Query = .{
        .cpu_arch = .wasm32,
        .os_tag = .freestanding,
        .abi = .none,
        .cpu_model = .{ .explicit = &std.Target.wasm.cpu.generic },
        .cpu_features_add = if (simd)
            std.Target.wasm.featureSet(&.{.simd128})
        else
            .empty,
    };
    const target = b.resolveTargetQuery(query);

    const mod = b.createModule(.{
        .root_source_file = b.path("src/wasm_abi.zig"),
        .target = target,
        .optimize = optimize,
        .single_threaded = true,
        .strip = optimize == .fast or optimize == .small,
    });

    const exe = b.addExecutable(.{
        .name = name,
        .root_module = mod,
    });
    exe.entry = .disabled;
    exe.rdynamic = true;
    exe.export_memory = true;
    exe.stack_size = 2 * 1024 * 1024;
    return exe;
}

pub fn build(b: *std.Build) void {
    const host_target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const wasm_debug = b.option(bool, "wasm-debug", "Use debug mode for WASM when -Doptimize is not set") orelse false;
    const wasm_optimize: std.builtin.Optimize = if (optimize == .debug and !wasm_debug) .fast else optimize;

    const simd_opt = b.option(bool, "simd", "Build only SIMD128 (true) or scalar (false); omit to build both");

    const wasm_step = b.step("wasm", "Build WebAssembly binaries");

    if (simd_opt == null or simd_opt.? == true) {
        const simd_exe = buildWasmExe(b, "upix_wasm", wasm_optimize, true);
        const install_simd = b.addInstallArtifact(simd_exe, .{});
        wasm_step.dependOn(&install_simd.step);
        b.getInstallStep().dependOn(&install_simd.step);
    }

    if (simd_opt == null or simd_opt.? == false) {
        const scalar_exe = buildWasmExe(b, "upix_wasm_scalar", wasm_optimize, false);
        const install_scalar = b.addInstallArtifact(scalar_exe, .{});
        wasm_step.dependOn(&install_scalar.step);
        b.getInstallStep().dependOn(&install_scalar.step);
    }

    const root_mod = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = host_target,
        .optimize = optimize,
    });

    const root_tests = b.addTest(.{
        .name = "upix-unit-tests",
        .root_module = root_mod,
    });

    const parity_mod = b.createModule(.{
        .root_source_file = b.path("tests/parity.zig"),
        .target = host_target,
        .optimize = optimize,
        .imports = &.{
            .{ .name = "upix", .module = root_mod },
        },
    });
    const parity_tests = b.addTest(.{
        .name = "upix-parity-tests",
        .root_module = parity_mod,
    });

    const test_step = b.step("test", "Run unit and oracle parity tests");
    test_step.dependOn(&b.addRunArtifact(root_tests).step);
    test_step.dependOn(&b.addRunArtifact(parity_tests).step);
}
