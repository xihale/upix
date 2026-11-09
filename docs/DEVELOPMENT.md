# 开发与构建指南

> 2026-11-09 起 Rust 栈为产品主线；旧 C++ 栈（CLI / ImGui 桌面端 / 共享核心）已迁入
> `legacy/` 仅作存档参考。推理引擎孤岛（ncnn/Vulkan C++ 子集）留在
> `legacy/share/core/`，由 Rust `upix-engine-ffi` 与 Android JNI 共同就地编译。

## 1. 环境依赖

- **构建工具**：`cargo / rustc`（版本见 `rust-toolchain.toml`）、`xmake >= 3.0`（Android Native；ncnn 静态库缓存在 `~/.xmake/packages`，engine-ffi 自动发现，非 linux/x86_64 需 `UPIX_NCNN_DIR`）
- **C++ 编译器**：GCC 14+ / Clang 18+（编译引擎孤岛；cargo 构建经由 `cc` crate 调用）
- **Android 依赖**：JDK 17+、Android SDK & NDK（版本见 `apps/app/build.gradle.kts`）
- **Legacy C++ 栈**：仅存档，构建入口见第 2.4 节

## 2. 构建命令

### 2.1 PC CLI 与 Desktop GUI（Rust，产品主线）
```bash
cargo build --release -p upix-cli -p upix-gui     # 产物: target/release/upix-cli
cargo run -p upix-gui                             # nsh 桌面端
```

### 2.2 Android App
```bash
cd apps
./gradlew assembleDebug                          # 自动调用 xmake 编译 libupix-native.so（引擎孤岛 = legacy/share/core）
```

### 2.3 Legacy C++ 栈（仅存档参考）
```bash
legacy/scripts/build_cli.sh release              # 旧 C++ CLI → build/linux/x86_64/release/upix-cli
cd legacy/desktop && ./build.sh                  # Dear ImGui 桌面端
legacy/scripts/build_host_tests.sh && legacy/scripts/run_host_tests.sh
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
