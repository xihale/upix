# Android target (nsh) — upix-gui

> **真机验证不在本期范围。** 本文只记录构建步骤与现状约束；没有经过真机
> 安装/运行验证。

## 现状约束

- **minSdk：nsh 的 android target 要求 API 28；upix 现状是 26**
  （`apps/gradle/libs.versions.toml`: `minSdk = "26"`，`targetSdk = "37"`）。
  接入 nsh android 时需把 app 工程的 minSdk 提到 ≥ 28，或等 nsh 放宽。
- nsh 的 android target 走 `nsh run --target android` / `nsh package --target
  android`：Rust 代码编成 `libnsh.so` 由 runtime 的 activity 拉起，UI 全部
  来自本 crate（无 QuickJS）。
- `src/main.rs`（桌面入口）在非桌面平台是 `compile_error!`；android 走的是
  nsh 的 runner，不经 `main.rs`，二者不冲突。

## 一次性环境准备

1. JDK 17+、Android Studio（SDK Manager 装 Platform 34+、Build-Tools、
   Platform-Tools），并接受许可证（`sdkmanager --licenses`）。
2. NDK（r25+）。Android Studio 的 SDK Manager → SDK Tools → NDK (Side by
   side) 即可。
3. Rust 目标（64 位真机/模拟器常用三个）：

   ```sh
   rustup target add aarch64-linux-android armv7-linux-androideabi x86_64-linux-android
   ```

4. `ANDROID_HOME` / `NDK_HOME` 指向 SDK/NDK 根目录（`~/.android/sdk` 或
   Android Studio 默认路径），确保 `sdkmanager`、`adb` 在 PATH。
5. `adb devices` 能看到设备（或启动模拟器）。

## 构建 / 运行

```sh
export PATH="$HOME/.nsh/bin:$PATH"
nsh run crates/upix-gui --target android        # 开发构建 + 安装到设备
nsh package crates/upix-gui --target android    # 出包（release）
```

`nsh.json` 已含 `"rust": "."` 与 `"root": "App"`；cargo 侧依赖（nsh-core /
nsh-base / 本 workspace 的 upix-* crates）按同一 manifest 解析，无需单独的
Android 工程。

## 与桌面的差异注意

- 文件对话框（`pick_paths`）在 Android 不可用（返回错误）：首页的
  drag-drop 与路径输入兜底同样适用；后续如需选图可接 nsh 的照片选择器
  （`pick_images`）。
- 本地放大默认走 CPU 引擎（`integration`）；GPU 只在主机 Coordinator 上
  先试再回落。模型目录仍是 `UPIX_MODELS_DIR`，Android 上要按设备约定落地。

## 验证清单（后续真机期）

- [ ] `nsh run --target android` 安装并启动到首页（无崩溃）。
- [ ] 五个页面（首页/进度/对比/主机/模型）切换正常，亮暗主题跟随系统与
      `UPIX_THEME`。
- [ ] MockEngine 全流程：选图 → 进度 → 对比（拖动分割线）。
- [ ] 主机页 QR 渲染与审批弹窗（模拟 host）。
