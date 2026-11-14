# 开发与构建指南

## 1. 环境依赖

### Native（CLI / Desktop / Android）
- **构建工具**：`xmake >= 3.0`（统一调度 Host / Android 构建与测试）
- **C++ 编译器**：GCC 14+ / Clang 18+ / MSVC 2022+（支持 C++23）
- **Linux 桌面额外系统依赖**：`libvulkan-dev`、`libdeflate-dev` 以及 X11/Wayland 开发头文件（`libx11-dev`、`libxcb1-dev`、`libxext-dev`、`libxcursor-dev`、`libxfixes-dev`、`libxi-dev`、`libxrandr-dev`、`libxrender-dev`、`libxss-dev`、`libwayland-dev`）；`libsdl3`、`imgui`、`ncnn` 等由 xmake 自动拉取
- **Android 额外依赖**：JDK 21+、Android SDK & NDK（版本定义见 [apps/app/build.gradle.kts](../apps/app/build.gradle.kts)）

### Web & Zig WASM
- **Zig**：`0.17.0`（用于编译 `wasm/` 下的 SIMD128 / scalar WASM 与单元测试）
- **Node.js**：`22+`（或兼容包管理器 `npm` / `bun`）

## 2. Native 构建命令

`xmake` 自定义任务会自动将 Host 与 Android 配置隔离在 `.xmake/host` 与 `.xmake/android` 中，交叉编译则隔离在 `.xmake/cross-<plat>-<arch>`，切换目标无需重新配置。

### Desktop GUI
```bash
xmake gui                     # Release 编译并运行 (desktop/build/upix)
xmake gui -m debug            # Debug + ASan 编译并运行
xmake gui --build --avif      # 仅编译，内置静态 libavif 编解码
```

### PC CLI
```bash
xmake cli                     # 默认 Release 构建 (build/<plat>/<arch>/release/upix-cli)
xmake cli -m debug            # Debug + ASan 构建
xmake cli --avif              # 内置静态 libavif 编解码
xmake cli --native-isa=n      # 禁用宿主机 AVX2 调优（用于可分发产物）
xmake cli --upx               # 使用 UPX 压缩 Linux Release 二进制
xmake cli -p windows          # 在非 Windows 宿主机通过 zigcc 交叉编译 MinGW 产物
```

### Android App / JNI
```bash
xmake apk                     # 编译 debug APK（Gradle 自动触发 xmake native）
xmake apk -m release          # 编译 release APK（需配置签名环境变量或 -Pupix.allowDebugSignedRelease）
xmake native                  # 仅交叉编译 libupix-native.so 到 apps/app/src/main/jniLibs/
```

### 预编译 SPIR-V 着色器包
```bash
xmake spirv                   # 若 share/core/spirv_pack.bin 缺失则生成（--force 强制重生成）
```

## 3. Web 与 WASM 构建

```bash
cd web
npm install                   # 首次安装开发依赖（Vite / ESLint）
npm run build:wasm            # 编译 wasm/ 输出 upix_wasm.wasm 与 upix_wasm_scalar.wasm 到 web/public/wasm/
npm run dev                   # 启动 Vite 开发服务器（带 COOP/COEP 头以启用 SharedArrayBuffer）
npm run lint                  # ESLint 检查
npm run build                 # 构建静态发布包到 web/dist/
```

## 4. 测试

```bash
# Native C++ 单元测试（share / cli / desktop，自动编译并运行）
xmake tests
xmake tests -m debug

# Zig WASM 单元测试与算子一致性测试
cd wasm && zig build test

# Android JVM 单元测试（不触发 NDK 编译）
cd apps && ./gradlew testDebugUnitTest -x xmakeNative --no-daemon

# 常驻热引擎 E2E、高压测试与模型效果回归（需先下载对应测试模型）
python3 tests/serve/run.py
python3 tests/serve_stress/run.py --quick
python3 tests/model_oracle/run.py
```
