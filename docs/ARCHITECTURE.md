# Upix 架构概览

Upix 采用「**单一 C++ 共享核（`share/`）+ 三端对等前端（`cli/`、`desktop/`、`apps/`）**」架构。所有业务规则、模型元数据与协议均收敛在 `share/`，前端只负责交互与状态展示。

## 1. 目录结构

```text
upix/
├── share/          # C++ 共享核心（推理管线、模型管理、局域网协同协议、常驻服务）
│   ├── core/       # 引擎封装 (ncnn/Vulkan)、分块调度、批量管线、serve daemon、distributed
│   └── utils/      # 图像编解码、XMP 超分标记、加密握手、本地与网络套接字
├── cli/            # 命令行前端 (C++23)
├── desktop/        # 桌面端 GUI (Qt 6 Quick + C++)，进程内直链 share/
├── apps/           # Android 客户端 (Kotlin Compose + JNI 直链 share/)
└── models/         # 模型说明与转换规范（模型权重按需从 GitHub Release 下载）
```

## 2. 核心模块职责

- **推理与分块管线（`share/core/`）**
  - 基于 ncnn（Vulkan GPU / CPU）执行超分推理，支持自动分块（Auto-Tile）与手动指定分块尺寸、重叠像素及批大小。
  - 输出前自动写入 XMP 超分标记（`sr_marker`），再次输入时默认幂等跳过，可通过 `force` 覆盖。
- **常驻热引擎（`share/core/serve/`）**
  - 通过本地套接字提供单图热推理服务，保持 GPU 上下文与模型权重在内存中，消除重复初始化开销，空闲超时自动退出。
- **模型管理（`share/core/model_*`）**
  - 统一维护模型家族分组、适用场景标签（动漫 / 照片 / 综合 / 修复）与推荐标记，支持本地扫描、远端清单拉取、校验下载、导入与移除。
- **局域网加密协同（`share/core/distributed/`）**
  - PC 端运行 `Coordinator` 监听局域网连接并生成二维码连接卡（支持多网卡地址切换）。
  - 手机端扫码后通过 X25519 + Ed25519 握手与设备指纹核对建立加密通道（信任库持久化），支持远端任务执行与模型同步。

## 3. 前端对等原则

- **CLI (`cli/`)**、**Desktop (`desktop/`)** 与 **Android (`apps/`)** 均为 `share/` 的一等公民调用方。
- Desktop 直接链接 `share/`（单图优先委派本地热引擎，批量目录、模型管理与配对协调均在工作线程内直接调用 `share/`），不依赖外部 `upix-cli` 进程。
