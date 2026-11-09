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

## 3. 跨平台交叉验证（Linux 本机）

产品目标四平台：linux-x86_64（本机全量验证）、android-arm64-v8a（CI/本机）、
x86_64-pc-windows-gnu 与 aarch64-apple-darwin（交叉检查 + CI 真实构建）。

```bash
# Windows（无 mingw 时用 zig 作 cc/cxx：UPIX 交叉工具链见仓库外 ~/.local/upix-cross/）
UPIX_ENGINE_FFI_SKIP_NATIVE=1 cargo check --target x86_64-pc-windows-gnu --workspace

# macOS（检查无需苹果工具链；真实构建/链接由 CI macos runner 完成）
UPIX_ENGINE_FFI_SKIP_NATIVE=1 cargo check --target aarch64-apple-darwin --workspace
```

`UPIX_ENGINE_FFI_SKIP_NATIVE=1` 跳过引擎孤岛的 C++ 编译（build.rs 不产链接产物，
真实链接仍会大声失败）——只服务传输层/状态机的编译检查；ncnn 静态库在各平台
由 CI 提供（windows 用 mingw 工具链编译，manifest plat 记为 `mingw`，build.rs 两种都认）。

### Windows GUI 的构建期依赖（仅编译检查需要，不进产物）

nsh-platform-windows 0.4.0 无条件编译 WebView2 宿主模块（`#[link(WebView2LoaderStatic)]`），
即使应用不含任何 `<web>` 元素（upix-gui 就不含）。Linux 上交叉检查需补一个静态库：

```bash
# 官方 NuGet 包取 WebView2LoaderStatic.lib（约 10MB，仅链接期解析符号）
curl -sL -o wv2.nupkg https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.2903.40/microsoft.web.webview2.1.0.2903.40.nupkg
unzip -o wv2.nupkg 'build/native/*' -d ~/.local/upix-cross/webview2
ln -s $(realpath ~/.local/upix-cross/webview2/build/native/x64/WebView2LoaderStatic.lib) \
      ~/.local/upix-cross/webview2/build/native/x64/libWebView2LoaderStatic.a   # rustc/gnu 按 lib<name>.a 搜索
RUSTFLAGS="-L native=$HOME/.local/upix-cross/webview2/build/native/x64" \
  UPIX_ENGINE_FFI_SKIP_NATIVE=1 cargo check --target x86_64-pc-windows-gnu -p upix-gui --no-default-features
```

真机运行零 WebView2 依赖：应用不渲染 web 视图，loader 只是链接期占位；
渲染走系统 Direct2D/DirectWrite。CI 的 windows runner（MSVC）可直接从同
一 NuGet 包取 lib。

### Windows serve 守护进程

unix 用 AF_UNIX socket 文件；Windows 端点为 127.0.0.1 回环 TCP（固定端口 47619，
`UPIX_SERVE_PORT` 可覆盖，绑定 loopback 不触发防火墙弹窗）。pid 文件、帧协议、
生命周期与 unix 完全一致；`daemon stop` 走 `taskkill /F`（分离进程收不到信号）；
pid 存活探测用 `tasklist`。文件传输路径、输出推导逻辑两端共享。

## 4. 测试

```bash
# Rust 全量测试（单元 + 协议金标 + 协调器/serve 状态机 + 引擎验收）
cargo test --workspace

# 常驻热引擎 E2E（Python 套件，默认二进制 target/release/upix-cli）
scripts/run_serve_tests.sh

# 模型效果 oracle（金标图像对拍）
scripts/run_model_oracle.sh
```
