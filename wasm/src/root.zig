//! Upix Zig WASM library root module.

pub const bicubic = @import("bicubic.zig");
pub const catalog = @import("catalog.zig");
pub const codec = @import("codec.zig");
pub const cpu_engine = @import("cpu_engine.zig");
pub const ncnn = @import("ncnn.zig");
pub const tile = @import("tile.zig");
pub const abi = @import("wasm_abi.zig");

test {
    _ = bicubic;
    _ = catalog;
    _ = codec;
    _ = cpu_engine;
    _ = ncnn;
    _ = tile;
    _ = abi;
}
