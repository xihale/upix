# Upix 架构概览

Upix 采用「**单一 Rust 共享核（`crates/upix-core` + `upix-net`/`upix-host`/`upix-img`）+ 一等公民前端（`upix-cli`、`upix-gui` [nsh]）+ Android（`apps/`，Kotlin）**」架构。业务规则、模型元数据与协议均已 Rust 化；唯一保留的 C++ 是 ncnn/Vulkan 推理引擎孤岛（`legacy/share/core/` 子集），由 Rust FFI 与 Android JNI 就地编译、共用一份源码。旧 C++ 共享核与 C++ 前端已整体迁入 `legacy/` 仅作存档参考。

## 1. 目录结构

```text
upix/
├── crates/         # Rust workspace（产品主线）
│   ├── upix-core/      # 推理管线编排（Engine trait、分块调度、模型管理）
│   ├── upix-net/       # 协议 wire 类型 + 加密（X25519/Ed25519/secretbox，libsodium 金标兼容）
│   ├── upix-host/      # Coordinator / serve daemon / 配对 / 模型同步
│   ├── upix-img/       # 图像编解码、EXIF、XMP 超分标记
│   ├── upix-engine-ffi/# 唯一 C++ FFI 岛：ncnn 引擎子集（编译 legacy/share/core 就地）
│   ├── upix-cli/       # 命令行前端 (clap)
│   └── upix-gui/       # 桌面端 GUI (nsh)
├── legacy/         # 旧 C++ 栈存档（不再演进；构建入口仍是根部 xmake）
│   ├── share/      # C++ 共享核心（引擎孤岛仍被 Rust FFI 与 Android JNI 编译）
│   ├── cli/        # 命令行前端 (C++23)
│   ├── desktop/    # 桌面端 GUI (Dear ImGui + SDL3 + Vulkan)
│   ├── tools/      # dump-spirv、perf 工具
│   └── scripts/    # legacy 构建脚本
├── apps/           # Android 客户端 (Kotlin Compose + JNI 直链 legacy/share/core)
├── scripts/        # 产品脚本（serve/oracle 套件、Android 构建、基准）
├── tests/          # Python E2E 套件（serve 协议、模型 oracle 金标）
└── models/         # 模型说明与转换规范（模型权重按需从 GitHub Release 下载）
```

## 2. 核心模块职责

- **推理与分块管线（`upix-core`，引擎孤岛 `legacy/share/core/ncnn_*`）**
  - 基于 ncnn（Vulkan GPU / CPU）执行超分推理，支持自动分块（Auto-Tile）与手动指定分块尺寸、重叠像素及批大小。
  - 输出前自动写入 XMP 超分标记（`upix-img::sr_marker`），再次输入时默认幂等跳过，可通过 `force` 覆盖。
- **常驻热引擎（`upix-host::serve`）**
  - 通过本地套接字提供单图热推理服务，保持 GPU 上下文与模型权重在内存中，消除重复初始化开销，空闲超时自动退出。
- **模型管理（`upix-core::model_*`，CLI `upix-cli model …`）**
  - 统一维护模型家族分组、适用场景标签（动漫 / 照片 / 综合 / 修复）与推荐标记，支持本地扫描、远端清单拉取、校验下载、导入与移除。
- **局域网加密协同（`upix-host::coordinator` + `upix-net`）**
  - PC 端运行 `Coordinator` 监听局域网连接并生成二维码连接卡（支持多网卡地址切换）。
  - 手机端扫码后通过 X25519 + Ed25519 握手与设备指纹核对建立加密通道（信任库持久化），支持远端任务执行与模型同步。
  - 加密实现为 Rust 原生（`upix-net::crypto`），与 libsodium 线格式金标兼容（`crates/upix-net/tests/golden.rs`）。

## 3. 前端对等原则

- **CLI (`crates/upix-cli`)**、**Desktop (`crates/upix-gui`)** 与 **Android (`apps/`)** 均为共享核的一等公民调用方。
- Rust CLI 与 GUI 直连同一份 workspace crate；GUI 进程内直链引擎（单图优先委派本地热引擎，批量目录、模型管理与配对协调在工作线程内直接调用），不依赖外部 `upix-cli` 进程。
- Android 经 JNI 调用引擎孤岛与 distributed 协议栈（`legacy/share/core` + `apps/app/src/main/cpp/jni_bridge.cpp`）。
