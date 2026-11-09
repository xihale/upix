# Upix

跨平台图像超分辨率工具，提供 **CLI**、**Desktop（暗房仪器工作台）** 与 **Android** 三端，共享同一套核心，并支持手机与 PC 局域网端到端加密协同算力卸载。

## Rust 栈（产品主线）

桌面端核心、CLI 与 GUI 均为 Rust 实现（`crates/` workspace），推理引擎通过 `upix-engine-ffi` 封装 `legacy/share/core/` 中的 ncnn/Vulkan C++ 引擎孤岛（与 Android JNI 共用同一份源码），Desktop GUI 使用 `nsh` 实现「暗房仪器」设计：

```bash
cargo build --workspace                              # 编译全部 Rust crates（含 upix-cli 与 upix-gui）
cargo test --workspace                               # 运行全部 Rust 单元测试与协议金标测试
cargo run -p upix-gui                                # 启动 nsh 桌面端 GUI
./target/release/upix-cli input.png -m NomosUni-SPAN-x2 --cpu --no-daemon
```

## 核心特性

- **单图与目录批处理**：支持自动分块（Auto-Tile）、自定义分块尺寸/重叠度及多并发处理。
- **常驻热引擎（Warm Daemon）**：后台保持 Vulkan 上下文与模型常驻，省去单图冷启动开销。
- **多格式精细编码**：支持 PNG、JPEG、WebP（含无损）、AVIF（可选）及色度抽样/压缩等级调节。
- **幂等跳过（SR Marker）**：输出自带 XMP 标记，默认自动跳过已超分图像，支持按需覆盖。
- **局域网加密卸载**：手机扫码配对 PC（X25519 + Ed25519 + XSalsa20-Poly1305），将超分任务卸载至桌面 GPU。

## 快速构建

### 1. PC CLI 与 Desktop GUI（Rust，产品主线）

```bash
cargo build --release -p upix-cli -p upix-gui
# 产物: target/release/upix-cli，GUI: cargo run -p upix-gui
```

### 2. Android (`apps/`)

```bash
cd apps
./gradlew assembleDebug
```

### 3. Legacy C++ 栈（`legacy/`，仅存档参考）

旧 C++ CLI（`legacy/cli`）与 Dear ImGui 桌面端（`legacy/desktop`）已迁入
`legacy/`，不再随产品演进；构建入口仍是仓库根部的 xmake：

```bash
legacy/scripts/build_cli.sh release     # 产物: build/linux/x86_64/release/upix-cli
cd legacy/desktop && ./build.sh         # ImGui 桌面端
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
- [全局设计语言规范](docs/DESIGN_LANGUAGE.md)
- [模型目录与转换说明](models/README.md)
