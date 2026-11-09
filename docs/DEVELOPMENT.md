# 开发与构建指南

> 2026-11-09 起 Rust 栈为产品主线；旧 C++ 栈（CLI / ImGui 桌面端 / 共享核心）已整体移入
> **`legacy` git 分支** 仅作存档，不在本工作树。产品仍在编译的 C++ 是引擎孤岛
> `engine/core/`（由 Rust `upix-engine-ffi` 与 Android JNI 共同就地编译）和
> `tools/dump_spirv.cpp`（SPIR-V 包再生成）。

## 1. 环境依赖

- **构建工具**：`cargo / rustc`（版本见 `rust-toolchain.toml`）、`xmake >= 3.0`（Android Native；ncnn 静态库缓存在 `~/.xmake/packages`，engine-ffi 自动发现，非 linux/x86_64 需 `UPIX_NCNN_DIR`）
- **C++ 编译器**：GCC 14+ / Clang 18+（编译引擎孤岛；cargo 构建经由 `cc` crate 调用）
- **Android 依赖**：JDK 17+、Android SDK & NDK（版本见 `apps/app/build.gradle.kts`）
- **旧 C++ 栈**：仅存档于 `legacy` 分支，需要时 `git checkout legacy`

## 2. 构建命令

### 2.1 PC CLI 与 Desktop GUI（Rust，产品主线）
```bash
cargo build --release -p upix-cli -p upix-gui     # 产物: target/release/upix-cli
cargo run -p upix-gui                             # nsh 桌面端
```

### 2.2 Android App
```bash
cd apps
./gradlew assembleDebug                          # 自动调用 xmake 编译 libupix-native.so（引擎孤岛 = engine/core）
```

### 2.3 旧 C++ 栈（仅存档）
```bash
git checkout legacy                              # C++ 完整快照（share/ cli/ desktop/ 在根路径）
# 构建方式见该分支的 README；本分支根 xmake 仅覆盖 Android + engine/ + tools/
git checkout rust-rewrite                        # 回到 Rust 主线
```

## 3. 测试

```bash
# Rust 全量测试（单元 + 协议金标 + 协调器/serve 状态机 + 引擎验收）
cargo test --workspace

# 常驻热引擎 E2E（Python 套件，默认二进制 target/release/upix-cli）
scripts/run_serve_tests.sh

# 模型效果 oracle（金标图像对拍）
scripts/run_model_oracle.sh
```
