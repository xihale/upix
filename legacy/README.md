# Legacy C++ 栈（存档）

2026-11-09 起，Rust workspace（`../crates/`）为产品主线。本目录是旧 C++23 栈的
完整存档，不再随产品演进：

- `legacy/share/` — 旧共享核心（引擎、分块管线、serve daemon、distributed 协议、模型管理）。
  其中 `core/` 的引擎子集（`ncnn_*`、`tile_processor`、`model_graph`、`spirv_pack`、
  `vulkan_shaders` 等）**仍是活代码**：Rust `upix-engine-ffi` 与 Android JNI
  （`../apps/app/src/main/cpp/jni_bridge.cpp`）就地编译这一份源码，勿在两处复制。
- `cli/` — 旧 C++ CLI。
- `desktop/` — Dear ImGui + SDL3 + Vulkan 桌面端。
- `tools/` — dump-spirv（SPIR-V pack 生成）与 perf 工具。
- `scripts/` — legacy 构建/测试脚本。

构建入口仍是仓库根部 `xmake.lua`（Android 与 legacy 目标）。SPIR-V pack
（`legacy/share/core/spirv_pack.bin`）为已跟踪文件；仅当 `vulkan_shaders.hpp` 变更时用
`legacy/scripts/gen_spirv_pack.sh` 重新生成。移植期语义契约见
`../docs/rewrite/CONTRACT.md`。
