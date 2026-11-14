# 模型转换与接入规范（pth → ncnn）

本文档面向维护者，规范新模型从来源核验、PyTorch 重实现、`pnnx` 转换、数值验证到多端元数据同步的完整流程。开始前请先阅读 [模型目录与排除项](README.md) 与 [许可归属](LICENSES.md)。

> **核心原则**：以 [spandrel](https://github.com/kohya-ss/spandrel) 作为结构、键名语义与数值真值的唯一基准。OpenModelDB 的架构标签偶有误差，二者冲突时以 spandrel 为准。

---

## 1. 核验权重来源

1. 在 [OpenModelDB](https://openmodeldb.info) 查找模型记录，确认上游许可证、权重 SHA-256 与作者原始下载链接。
2. 下载权重后**首先逐字节比对 SHA-256**。
3. 注意 fp16 存储的检查点（例如 `Nomos2-OTF-x4` 为 64 通道全量 RRDB，fp16 存储约 33.7 MB），不要仅凭文件体积误判网络宽度。
4. 确认可明确归类 SPDX 许可证；非商用协议（如 CC-BY-NC-SA-4.0）允许收录，但必须在 [LICENSES.md](LICENSES.md) 与客户端提示中显式标注。

## 2. 识别架构与键名重映射

使用 `spandrel` 加载 `.pth` 检查 `state_dict` 键名、卷积形状与检测出的倍率（若环境无 `torchvision`，仅需 stub `torchvision.transforms.functional.{InterpolationMode,rotate,normalize}`）：

- **SRVGGNetCompact**（v3 compacts、Archivist、pokepress）：traiNNer 训练的 compact 通常带 `learn_residual=True`（`body(x) + x`）；`scale=1` 时 PixelShuffle 退化为恒等映射。
- **ESRGAN RRDB**：存在旧式键名（`model.0`、`model.1.sub.N` 含 `.0.` 包装）与 **KAIR（BSRGAN）** 自有布局（`conv_first / RRDB_trunk.N.RDB*.convK / trunk_conv / upconv* / HRconv / conv_last`），需分别映射到纯 `nn.Module`。
- **SPAN**：必须按 spandrel `eval` 语义对 `Conv3XC` 做重参数化（`1x1-sk + bottleneck` 折叠为单层 `3x3`；忽略死键 `eval_conv`）。traiNNer SPAN 会在图内执行 `(x - mean) * 255` 归一化并返回 `[0, 1]` 尺度输出，ncnn 图必须完全复现该行为。
- **RealPLKSR**：部分大卷积核结构；通道重复残差可改写为逐位等价的最近邻插值拼接，`pnnx` 可直接无损降级。

> **常见陷阱**：`spandrel.ImageModelDescriptor.__call__` 会执行 `output.clamp_(0, 1)`。做 0 LSB 对齐时必须直接调用裸模型 `m.model(x)`，或两侧采用相同的 clamp。

## 3. 纯 PyTorch 锚定（0.0 最大绝对误差）

将重映射后的权重加载到纯 `nn.Module`，在真实图像裁剪块上与 spandrel 裸输出比对，要求最大绝对误差 **严格为 `0.0`**。若存在偏差，依次排查：
1. 重映射后的逐张量权重差值；
2. 逐层前向输出差值定位分歧层；
3. 是否误与 clamp 后的 descriptor 输出对比。

导出参考输出 PNG 与 TorchScript（`.ts`）。

## 4. 使用 `pnnx` 转换

- 从 `github.com/pnnx/pnnx` 获取二进制（Tencent/ncnn 发布页已不再随包附带 pnnx）。
- **必须先用小尺寸（`64x64`）重新 trace TorchScript**：pnnx 会以 trace 时的输入尺寸跑全图（忽略不匹配的 `inputshape=` 参数）；256×256 trace 的 RRDB 会导致 pnnx 内存暴涨 OOM，64×64 则轻量且对纯卷积网络图结构完全等价。
- 转换时建议串行（并发 ≤ 2）。
- 同时导出默认 fp16 图（`fp16=1`）与 fp32 对照图（`fp16=0`），用于下一步交叉验证。
- 输出文件名中的 `-` 会被 pnnx 替换为 `_`，部署前重命名为目录规范名（末尾 `-xN`）。
- **当前锁定 ncnn（`20260526`）已知行为**：
  - Pixel-Unshuffle 头（如 `Real-ESRGAN-x2plus`）会降级为 `Reorg` 层，引擎已在通道与倍率推导中做补偿。
  - `Real-CUGAN se x4` 的 PixelShuffle 主干拓扑在当前版本下 CPU/Vulkan 均输出错误，属于已知排除项。

## 5. 验证门禁（新模型必过）

1. **SHA-256**：与 OpenModelDB 记录逐字节一致。
2. **PyTorch 重实现 vs spandrel 裸输出**：最大绝对误差为 `0.0`。
3. **ncnn CPU vs PyTorch 参考**：在 `[0, 1]` 浮点域计算 PSNR，通常位于 `44–51 dB` 区间（注意不要误用 `0–255` 域计算）。
4. **fp16 / fp32 图 A/B 对照**：`fp16=1` 与 `fp16=0` 输出必须字节级一致（`>= 99 dB`），证明误差仅来自执行数值而非量化转换。
5. **Vulkan GPU vs CPU**：处于同家族 fp16 基线内（RRDB 约 `~32 dB` 为正常 fp16 漂移，轻量模型更紧）。
6. **边界与分块一致性**：奇数输入尺寸（如 `127x125`）输出分辨率精确匹配；分块推理与整图推理一致（`>= 99 dB`）。
7. **同进程热切换**：在守护进程或同进程内连续切换多个模型推理，验证 `unload` 与新模型无 blob 索引串扰。
8. **发布包回环测试**：打包 `<name>.zip`（内含 `<name>.param` + `<name>.bin`），更新 `manifest.json` 校验 `sha256`、`size_bytes` 与 `scale`，重下载解压后校验字节一致。

## 6. 仓库同步清单

文件名倍率解析在 **C++ 引擎（`share/core/model_graph.hpp`）**、**Android（`ModelManager`）** 与 **Zig WASM（`wasm/src/catalog.zig`）** 三处独立实现，模型名**必须**以 `-xN`（`N` 为 `1..8`）结尾。

新增或调整收录模型时需同步以下位置：

- [ ] **离线内置目录（若纳入离线回退清单）**：`share/core/model_catalog.hpp` 与 `wasm/src/catalog.zig`（`CATALOG`）。
- [ ] **场景分类表与推荐星标**：`share/core/model_metadata.cpp`、`wasm/src/catalog.zig`（`SCENE_RULES`）、`apps/app/src/main/java/io/upix/data/model/ModelScenes.kt`，以及单测 `cli/tests/test_model_scene.cpp`。
- [ ] **Android 帮助文案**：`apps/app/src/main/res/values/strings.xml` 与 `values-zh-rCN/strings.xml` 中的 `help_family_*` / `help_scenario_*`。
- [ ] **模型文档**：[models/README.md](README.md) 与 [models/LICENSES.md](LICENSES.md)。
- [ ] **架构文档（仅当新增引擎层兼容逻辑/拓扑规则时）**：[docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)。
- [ ] **模型发布仓库**：上传 `<name>.zip` 并更新 GitHub Release（`models` tag）与 HuggingFace 仓库（`xihale/upix-models`）的 `manifest.json`。
