# CLI 使用参考

`upix-cli` 是 Upix 的命令行前端（构建产物位于 `build/<plat>/<arch>/release/upix-cli`）。

## 1. 超分处理（默认命令）

直接传入输入图像或目录即可执行超分，省略输出路径时会自动生成 `<name>_<model>.<ext>` 或 `<dir>_<model>/`：

```bash
upix-cli input.png
upix-cli input.png -o output.webp -m AnimeSharp-x4 --gpu
upix-cli ./photos ./photos_out -m Real-ESRGAN-x4plus -j 4 --auto-tile
```

### 常用参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| `-m, --model <name>` | 指定模型名称 | 第一个可用本地模型 |
| `--models <dir>` | 指定模型目录 | 见第 5 节默认路径规则 |
| `--gpu` / `--cpu` | 强制使用 GPU 或纯 CPU（互斥；`--cpu` 不走 GPU 热引擎） | 自动（优先 GPU） |
| `--no-daemon` | 单图模式禁用常驻热引擎，直接进程内推理 | `false` |
| `-f, --force` | 强制重处理已带 `sr_marker` XMP 标记的图像 | `false`（默认跳过） |
| `--tile-size <N>` | 分块尺寸（16–8192） | `256` |
| `--tile-overlap <N>` | 分块重叠像素（0–1024） | `16` |
| `--auto-tile` | 根据显存自动选择分块尺寸（范围 `--min-tile` 64 至 `--max-tile` 512） | `false` |
| `--batch-size <N>` | 单次推理批大小（1–64） | `1` |
| `-j, --jobs <N>` | 目录模式并发工作数（1–1024） | `1`（GPU 目录未指定时按显存估算） |
| `--output-format <fmt>` | 输出格式：`png` / `jpg` / `jpeg` / `webp` / `avif` | 按扩展名自动推断 |
| `--output-quality <0-100>` | 有损编码质量 | `90` |
| `--config <path>` | 从 JSON 配置文件加载默认参数（显式 CLI 标志优先覆盖） | 无 |

### 格式专属编码参数

- **PNG**：`--png-level <-1..9>`（`-1` 为根据 `--output-quality` 自动映射）
- **JPEG**：`--jpeg-subsampling <420|422|444>`（默认 `420`）
- **WebP**：`--webp-method <-1..6>`（默认 `-1`，即 libwebp 默认 4）、`--webp-lossless`
- **AVIF**：`--avif-speed <0-10>`（默认 `6`）、`--avif-subsampling <420|444>`（默认 `420`）、`--avif-threads <0-64>`（默认 `0` 自动）

### JSON 配置文件示例（`--config`）

```json
{
  "model_name": "Real-ESRGANv3-anime-x2",
  "use_gpu": true,
  "tile_size": 256,
  "tile_overlap": 16,
  "auto_tile_size": false,
  "min_tile_size": 64,
  "max_tile_size": 512,
  "batch_size": 1,
  "output_format": "webp",
  "output_quality": 90
}
```

## 2. 模型管理（`model`）

```bash
upix-cli model list [--models-dir DIR] [--names-only] [--json]
upix-cli model list-remote [--repo owner/name] [--json]
upix-cli model download <NAME...> [--models-dir DIR] [--repo owner/name] [--json]
upix-cli model import <FILE...> [--models-dir DIR] [--json]
```

- `model import` 接受本地成对的 `.param` 与 `.bin` 文件路径。
- 名称匹配忽略大小写以及 `-` / `_` / 空格分隔符差异。

## 3. 局域网协同服务（`host`）

启动与 Android 端扫码配对的加密协调服务（省略子命令时默认执行 `start`）：

```bash
upix-cli host start [-p 9527] [--models-dir DIR] [--auto-approve] [--json]
upix-cli host info [-p 9527] [--json]
```

## 4. 常驻热引擎（`daemon`）

管理本地热推理守护进程（省略子命令时默认执行 `start`）：

```bash
upix-cli daemon start [--model NAME] [--models-dir DIR] [--socket PATH] [--cpu] [--fg] [--idle-timeout 600]
upix-cli daemon status [--socket PATH] [--json]
upix-cli daemon stop [--socket PATH]
```

## 5. Shell 自动补全（`completions`）

```bash
upix-cli completions <bash|zsh|fish> [-i|--install]
```

## 6. 默认模型目录与环境变量

模型目录解析优先级：`--models` / `--models-dir` > `$UPIX_MODELS_DIR` > `$XDG_DATA_HOME/upix/models` > `~/.local/share/upix/models`（Windows 回退 `%USERPROFILE%/upix/models`）。

| 环境变量 | 说明 |
|---|---|
| `UPIX_LOG_LEVEL` | 日志级别：`debug` \| `info` \| `warn` \| `error`（默认 `info`） |
| `UPIX_PROGRESS` | `1` 输出逐分块日志；`json` 在 stdout 输出逐行 JSON 进度事件 |
| `UPIX_MODELS_DIR` | 覆盖默认模型存储目录 |
| `NO_COLOR` | 非空时禁用终端彩色输出 |
