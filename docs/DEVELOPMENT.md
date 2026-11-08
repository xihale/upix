# 开发与构建指南

## 1. 环境依赖

- **构建工具**：`cargo / rustc 1.85+`（Rust Workspace）、`xmake >= 3.0`（CLI / C++ Desktop / Android Native）
- **C++ 编译器**：GCC 14+ / Clang 18+ / MSVC 2022+（支持 C++20/23）
- **Desktop 依赖**：Rust `nsh`（`crates/upix-gui/`）或 Dear ImGui + SDL3 + Vulkan（`desktop/`）
- **Android 依赖**：JDK 17+、Android SDK & NDK（版本见 `apps/app/build.gradle.kts`）

## 2. 构建命令

### PC CLI
```bash
scripts/build_cli.sh release                     # 默认构建
scripts/build_cli.sh debug                       # 调试构建
scripts/build_cli.sh release --avif-encoder=y --avif-decoder=y
```

### Desktop GUI
```bash
# C++ Desktop (Dear ImGui + SDL3 + Vulkan)
cd desktop
./build.sh                                       # 编译至 desktop/build/upix

# Rust Desktop (nsh)
cargo run -p upix-gui
```

### Android App
```bash
cd apps
./gradlew assembleDebug                          # 自动调用 xmake 编译 libupix-native.so
```

## 3. 测试

```bash
# C++ 核心与协议单元测试
scripts/build_host_tests.sh && scripts/run_host_tests.sh

# 常驻热引擎 E2E 测试
scripts/run_serve_tests.sh

# Desktop 组件与交互回归测试
ctest --test-dir desktop/build --output-on-failure
```
