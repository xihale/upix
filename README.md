# Upix

跨平台图像超分辨率工具，提供 **CLI**、**Desktop（SDL3 + Vulkan + Dear ImGui）**、**Android** 与 **Web（WebGPU + WebNN + Zig WASM）** 四端，支持手机与 PC 局域网端到端加密协同算力卸载。

## 核心特性

- **单图与目录批处理**：支持自动分块（Auto-Tile）、自定义分块尺寸 / 重叠度及多并发处理。
- **常驻热引擎（Warm Daemon）**：后台保持 Vulkan 上下文与模型常驻，省去单图冷启动开销。
- **多格式精细编码**：支持 PNG、JPEG、WebP（含无损）、AVIF（可选）及色度抽样 / 压缩等级调节。
- **幂等跳过（SR Marker）**：输出自带 XMP 标记，默认自动跳过已超分图像，支持 `--force` 覆盖。
- **局域网加密卸载**：Android 扫码配对 PC（X25519 + Ed25519 + XSalsa20-Poly1305），将超分任务卸载至桌面 GPU。
- **浏览器端本地推理**：`web/` + `wasm/` 支持 WebGPU、WebNN 与 Zig WASM SIMD128 多 Worker 推理及 IndexedDB 模型缓存。

## 快速构建

Native 端构建与测试统一通过根目录 `xmake` 自定义任务触发（Host 与 Android 配置隔离在 `.xmake/host` 与 `.xmake/android`）：

```bash
# 1. PC CLI (产物: build/<plat>/<arch>/release/upix-cli)
xmake cli

# 2. Desktop GUI (直接链接 share/，产物: desktop/build/upix)
xmake gui              # 编译并启动
xmake gui --build      # 仅编译

# 3. Android (apps/)
xmake apk              # 组装 debug APK（自动触发 xmake native）
xmake native           # 仅编译 JNI 动态库到 apps/app/src/main/jniLibs/

# 4. Web & Zig WASM (web/ + wasm/)
cd web && npm install && npm run build:wasm && npm run dev
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

更多选项与子命令见 [CLI 使用参考](docs/CLI.md)。

## 文档索引

- [文档总览](docs/README.md)
- [架构概览](docs/ARCHITECTURE.md)
- [构建与开发指南](docs/DEVELOPMENT.md)
- [CLI 使用参考](docs/CLI.md)
- [模型目录与选型指南](models/README.md)
- [模型许可与来源归属](models/LICENSES.md)
- [模型转换与接入流程](models/CONVERTING.md)
