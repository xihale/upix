# Upix 架构概览

Upix 的 Native 三端（**CLI**、**Desktop**、**Android**）采用「**单一 C++23 共享核（`share/`）+ 对等前端**」架构；**Web 端**则通过 **Zig WASM（`wasm/`）+ WebGPU / WebNN（`web/`）** 在浏览器内独立实现相同模型目录与推理管线。

## 1. 目录结构

```text
upix/
├── share/          # Native C++23 共享核心（推理管线、模型管理、局域网协同协议、常驻服务）
│   ├── core/       # ncnn/Vulkan 引擎封装、分块调度、批量管线、serve daemon、distributed
│   ├── utils/      # 图像编解码、XMP 超分标记、加密握手、本地与网络套接字
│   └── perf/       # 性能基准工具库
├── cli/            # 命令行前端 (upix-cli)
├── desktop/        # 桌面端 GUI (SDL3 + Vulkan + Dear ImGui)，进程内直链 share/
├── apps/           # Android 客户端 (Kotlin Compose + JNI 直链 share/)
├── web/            # Web 前端（Vite 原生 JS，WebGPU / WebNN / WASM 调度与 IndexedDB 模型库）
├── wasm/           # Zig WASM 内核与编解码/目录元数据模块（SIMD128 与 scalar 双构建）
├── tools/          # 构建期工具（dump-spirv 生成预编译 SPIR-V 包、upix-perf 基准入口）
└── models/         # 模型目录说明、许可证归属与 pth → ncnn 转换规范
```

## 2. Native 共享核心职责（`share/`）

- **推理与分块管线（`share/core/`）**
  - 基于 ncnn（Vulkan GPU / CPU）执行超分推理，支持自动分块（Auto-Tile）以及手动分块尺寸、重叠像素与批大小控制。
  - 编译期通过 `tools/dump_spirv.cpp` 将 Vulkan 着色器打包为 `share/core/spirv_pack.bin` 并内嵌（`spirv_pack.S`），运行时无需链接 glslang。
  - 输出前自动写入 XMP 超分标记（`sr_marker`），再次输入时默认幂等跳过，可通过 `force` 覆盖。
- **常驻热引擎（`share/core/serve/`）**
  - 通过本地套接字（Linux/macOS `AF_UNIX`，Windows 命名管道）提供单图热推理服务，保持 GPU 上下文与模型权重常驻内存，空闲超时自动退出。
- **模型管理与分发（`share/core/model_*`）**
  - 统一维护离线内置目录（`model_catalog.hpp`）、家族分组（`model_families.hpp`）与场景标签（`model_metadata.cpp`）。
  - 下载客户端（`model_downloader.cpp`）支持 `hf-mirror`（中国大陆默认）、`hf`（HuggingFace）与 GitHub Release（`xihale/upix` `models` tag），并校验 SHA-256。
- **局域网加密协同（`share/core/distributed/`）**
  - PC 端运行 `Coordinator` 监听局域网连接并生成二维码连接卡（支持多网卡地址切换）。
  - 手机端扫码后通过 X25519 + Ed25519 握手与设备指纹核对建立加密通道（信任库持久化），支持远端任务执行与模型同步。

## 3. 前端实现边界

- **CLI（`cli/`）与 Desktop（`desktop/`）**：均直接链接 `share/`（`upix-core`）。Desktop 单图推理优先委派本地热引擎，批量目录、模型下载与配对协调直接在工作线程调用 `share/`，不依赖外部 `upix-cli` 进程。
- **Android（`apps/`）**：Kotlin Compose UI 通过 JNI（`apps/app/src/main/cpp/jni_bridge.cpp`）直链 `share/`，独立维护场景对照表（`ModelScenes.kt`）与无 SNI 直连下载客户端（`ModelReleaseRepository.kt`）。
- **Web（`web/` + `wasm/`）**：
  - 推理路由在 `web/src/runtime/index.js` 根据硬件能力与模型族在 **WebGPU**、**WebNN**（`gpu`/`npu`/`cpu`）和 **Zig WASM SIMD128 多 Worker** 间自动选择或回退。
  - 模型权重缓存在浏览器 IndexedDB，默认从 `hf-mirror` 或 `hf` 拉取（GitHub Release 因跳转缺少 CORS 头在 Web 端锁定禁用）。
  - `wasm/src/catalog.zig` 与 `share/core/model_catalog.hpp` / `model_metadata.cpp` 保持同构的目录与场景分类规则。
