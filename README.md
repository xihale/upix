# Upix

跨平台图像超分辨率工具，提供 **CLI**、**Desktop（Dear ImGui + SDL3 + Vulkan）** 与 **Android** 三端，共享同一套 C++ 核心（`share/`），并支持手机与 PC 局域网端到端加密协同算力卸载。

## 核心特性

- **单图与目录批处理**：支持自动分块（Auto-Tile）、自定义分块尺寸/重叠度及多并发处理。
- **常驻热引擎（Warm Daemon）**：后台保持 Vulkan 上下文与模型常驻，省去单图冷启动开销。
- **多格式精细编码**：支持 PNG、JPEG、WebP（含无损）、AVIF（可选）及色度抽样/压缩等级调节。
- **幂等跳过（SR Marker）**：输出自带 XMP 标记，默认自动跳过已超分图像，支持按需覆盖。
- **局域网加密卸载**：手机扫码配对 PC（X25519 + Ed25519 + XSalsa20-Poly1305），将超分任务卸载至桌面 GPU。

## 快速构建

### 1. PC CLI (`cli/`)

```bash
scripts/build_cli.sh release
# 产物: build/linux/x86_64/release/upix-cli
```

### 2. Desktop GUI (`desktop/`)

直接链接 `share/` 共享核心，无需外部 CLI 进程：

```bash
cd desktop
./build.sh
./build/upix
```

### 3. Android (`apps/`)

```bash
cd apps
./gradlew assembleDebug
```

## 常用 CLI 命令

```bash
# 单图与目录超分
upix-cli input.png
upix-cli input_dir -m AnimeSharp-x4 --gpu

# 模型管理
upix-cli model list
upix-cli model list-remote
upix-cli model download AnimeSharp-x4
upix-cli model import ./custom.param ./custom.bin

# 局域网协同服务与常驻守护进程
upix-cli host start --port 9527
upix-cli daemon status
```

## 文档索引

- [架构概览](docs/ARCHITECTURE.md)
- [构建与开发指南](docs/DEVELOPMENT.md)
- [Desktop 设计规范](desktop/DESIGN.md)
- [模型目录与转换说明](models/README.md)
