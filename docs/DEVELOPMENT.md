# 开发与构建指南

## 1. 环境依赖

- **构建工具**：`xmake >= 3.0`（统一调度 Host / Android 构建与测试）
- **C++ 编译器**：GCC 14+ / Clang 18+ / MSVC 2022+（支持 C++23）
- **Desktop 依赖**：系统 Vulkan 驱动与头文件、`libdeflate`（`libsdl3`、`imgui`、`ncnn` 等由 xmake 自动管理）
- **Android 依赖**：JDK 21+、Android SDK & NDK（版本见 `apps/app/build.gradle.kts`）

## 2. 构建命令

`xmake` 自定义任务会自动将 Host 与 Android 的配置隔离在 `.xmake/host` 和 `.xmake/android` 中，切换构建目标无需重新配置：

### Desktop GUI
```bash
xmake gui                     # Release 编译并运行 (desktop/build/upix)
xmake gui -m debug            # Debug + ASan 编译并运行
xmake gui --build --avif      # 仅编译，内置静态 libavif
```

### PC CLI
```bash
xmake cli                     # 默认 Release 构建 (build/<plat>/<arch>/release/upix-cli)
xmake cli -m debug            # Debug + ASan 构建
xmake cli --avif              # 内置静态 libavif
```

### Android App / JNI
```bash
xmake apk                     # 编译 debug APK（Gradle 自动触发 xmake native）
xmake apk -m release          # 编译 release APK
xmake native                  # 仅交叉编译 libupix-native.so
```

## 3. 测试

```bash
# C++ 核心、CLI 与 Desktop 单元测试（自动编译并运行）
xmake tests

# 常驻热引擎 E2E 与模型回归测试
python3 tests/serve/run.py
python3 tests/model_oracle/run.py
```
