# Upix 模型目录与选型指南

Upix 使用 ncnn 格式的超分辨率模型（`.param` + `.bin`）。模型文件**不随二进制安装包内置**，而是在首次使用时按需从远端仓库（HuggingFace / HF-Mirror / GitHub `models` Release）下载，或由用户自行导入。

## 1. 获取与管理模型

### CLI / Desktop
```bash
upix-cli model list-remote                 # 查看远端可用目录
upix-cli model download Real-ESRGANv3-anime-x2
upix-cli model list                        # 查看本地已安装模型与场景标签
upix-cli model import ./my.param ./my.bin  # 导入自定义 ncnn 模型
```
- **存储路径**：`$UPIX_MODELS_DIR` > `$XDG_DATA_HOME/upix/models` > `~/.local/share/upix/models`（Windows 回退 `%USERPROFILE%/upix/models`）。
- **目录布局**：支持递归扫描与符号链接（最多 8 层），扁平与嵌套均可：
  - 扁平：`Real-ESRGANv3-anime-x2.param` + `Real-ESRGANv3-anime-x2.bin`
  - 嵌套：`Real-ESRGANv3-anime/x2.param` + `x2.bin`（识别为 `Real-ESRGANv3-anime-x2`）
- **下载源**：CLI / Desktop / Web 在中国大陆默认优先使用 `hf-mirror.com/xihale/upix-models`，海外默认 `huggingface.co/xihale/upix-models`；CLI / Desktop 亦支持 GitHub Release（`xihale/upix` `models` tag）。

### Android 与 Web
- **Android**：「模型」页 →「下载模型」直接拉取清单下载；支持导入本地 `.param` / `.bin` / `.zip`。
- **Web**：模型缓存于浏览器 IndexedDB，可从模型页切换下载源（HF-Mirror / Hugging Face）或拖入本地 `.param` / `.bin` / `.zip`。

---

## 2. 场景速选指南

| 场景 | 轻量首选（手机 / 批处理） | 高画质首选（PC / 桌面 / 协同卸载） |
|---|---|---|
| **动漫 / 插画 / 截图** | `Real-ESRGANv3-anime-x2` ★ / `-x3` / `-x4`<br>`HFA2k-LUDVAE-SPAN-x2`（重压缩源）<br>`ModernSpanimation-x2`（现代番剧） | `AnimeSharp-x4` ★（线条锐利）<br>`Real-ESRGAN-x4plus-anime`（细节密集大图）<br>`HFA2k-x4`（粗糙压缩源修复） |
| **动漫强降噪** | `Real-CUGAN-pro-denoise-x2/-x3`（优先 `pro`） | 同左 / `Real-CUGAN-denoise-x2/-x3` |
| **照片 2x** | `NomosUni-Compact-x2` / `Parimg-Compact-x2` / `NomosUni-SPAN-x2` | `Real-ESRGAN-x2plus` / `BSRGAN-x2` |
| **照片 4x** | `Real-ESRGANv3-general-x4` ★ / `LSDIR-Compact-v2-x4` / `NomosUni-SPAN-x4` | `Real-ESRGAN-x4plus` ★ / `NomosWebPhoto-RealPLKSR-x4` / `Nomos2-OTF-x4` / `Nomos8kSC-x4` / `BSRGAN-x4` |
| **综合写实（全能）** | `LSDIR-Compact-v2-x4` | `UltraSharp-x4` ★ / `Remacri-x4` ★ / `NMKD-Siax-x4` |
| **1x 原尺寸修复** | `Archivist-Medium-Compact-x1` ★（动漫/视频帧） | `DeJPG-RealPLKSR-x1`（JPEG 块）/ `DeNoise-RealPLKSR-x1`（噪点）/ `DeH264-RealPLKSR-x1`（视频压痕） |
| **复古主机采集 1x** | `NES-Composite-RGB-Compact-x1` / `Genesis-Cleanup-Compact-x1` | — |

> 带 `★` 为对应场景默认推荐项（与 CLI、Desktop、Android、Web 徽标一致）。

---

## 3. 收录模型目录

> 注：表格中体积为打包后大约体量。C++ 核心（`share/core/model_catalog.hpp`）与 WASM 端（`wasm/src/catalog.zig`）内置了 34 个常用离线目录条目；完整 Release 清单包含下述全部分支。

### Real-ESRGAN v3（轻量全能，SRVGGNetCompact）

| 模型 | 体积 | 适用说明 |
|---|---|---|
| `Real-ESRGANv3-anime-x2` / `-x3` / `-x4` | 1.2 MB | 动漫、插画、游戏截图；保持线条锐利并清理压缩噪点，双端默认动漫首选 |
| `Real-ESRGANv3-general-x4` | 2.3 MB | 实拍照片 4x；对 JPEG/WebP 压缩鲁棒，移动端照片默认首选 |

### Real-ESRGAN x4plus / x2plus（重量级 RRDB）

| 模型 | 体积 | 适用说明 |
|---|---|---|
| `Real-ESRGAN-x4plus` | 31 MB | 照片 4x 经典重建模型，纹理还原最强，适合 PC 端或协同卸载 |
| `Real-ESRGAN-x2plus` | 31 MB | 照片 2x 参考画质（含 Pixel-Unshuffle 头，适配 2x 高保真放大） |
| `Real-ESRGAN-x4plus-anime` | 8.3 MB | 动漫 / 插画 4x 参考画质，适合高密度细节大图 |

### Real-CUGAN（动漫专精，bilibili）

针对真实压制劣化（高压缩 JPEG / H.264 番剧截图）训练；`pro`（第二代）泛化通常优于同变体初代 `se`：

- **型号**：`Real-CUGAN-{conservative,denoise,no-denoise}-x2` / `-x3` 及 `Real-CUGAN-pro-{conservative,denoise,no-denoise}-x2` / `-x3`（均约 2.4 MB）
- **变体差异**：
  - `conservative`：尽量少改动原图，保留原始颗粒与细节
  - `denoise`：强力去噪（对应上游 denoise3x），适合老番压制与低码率流媒体截图
  - `no-denoise`：不做平滑去噪，适合干净片源

### 社区照片模型（OpenModelDB，Phhofm）

| 模型 | 体积 | 适用说明 |
|---|---|---|
| `NomosUni-Compact-x2` | 1.1 MB | 照片 2x（SRVGG Compact，`multijpg` 训练），适合修复社交软件压缩图 |
| `Parimg-Compact-x2` | 1.1 MB | 照片 2x，与前者同量级但清理力度更强 |
| `NomosUni-SPAN-x2` | 0.8 MB | 照片 2x（SPAN 架构），全目录最轻量模型之一 |
| `LSDIR-Compact-v2-x4` | 1.2 MB | 照片 4x 轻量网络，同速下比 `v3-general-x4` 更干净 |
| `NomosUni-SPAN-x4` | 0.8 MB | 照片 4x 最轻量模型，适合低算力设备快速批处理 |
| `NomosWebPhoto-RealPLKSR-x4` | 13.7 MB | 照片 4x（RealPLKSR 架构），体积仅为 RRDB 一半，网络图画质/速度均衡之选 |
| `Nomos8kSC-x4` | 31 MB | 照片 4x（RRDB），强去压缩与去模糊，风格偏锐利通透 |
| `Nomos2-OTF-x4` | 32 MB | 照片 4x（RRDB，fp16 存储权重），2024 真实相机与网络劣化配方 |

### 社区 1x 原尺寸修复模型（不放大，直接清理劣化）

| 模型 | 体积 | 适用说明 |
|---|---|---|
| `DeJPG-RealPLKSR-x1` | 14 MB | 原尺寸去除 JPEG 8×8 块效应（针对低至 Q40 多次转存图） |
| `DeNoise-RealPLKSR-x1` | 14 MB | 原尺寸照片降噪，适合手机高 ISO 夜景与传感器噪点 |
| `DeH264-RealPLKSR-x1` | 14 MB | 原尺寸去除 H.264 视频压缩块与振铃效应 |
| `Archivist-Medium-Compact-x1` | 0.6 MB | 动漫 / 视频帧均衡修复，去除常见胶片颗粒与污损（推荐起步） |
| `Archivist-Rough-Compact-x1` | 0.6 MB | 针对重度损毁片源的激进修复（易产生脑补细节，建议先试 Medium） |
| `NES-Composite-RGB-Compact-x1` | 4 MB | NES / FC 复合视频采集画面还原干净 RGB（去除点蠕动与色溢） |
| `Genesis-Cleanup-Compact-x1` | 4 MB | Mega Drive / Genesis 采集画面去模糊与载波噪点 |

### 社区动漫修复与经典模型

| 模型 | 体积 | 适用说明 |
|---|---|---|
| `HFA2k-LUDVAE-SPAN-x2` | 0.8 MB | 粗糙动漫源 2x 修复，最轻量的动漫修复网 |
| `ModernSpanimation-x2` | 1.4 MB | 现代动画 2x（64 通道 SPAN），兼顾轻量与清晰度 |
| `HFA2k-x4` | 32 MB | 粗糙动漫源 4x 重建（RRDB），偏重劣化恢复 |
| `AnimeJaNai-v2-Compact-x2` | 1.2 MB | 动漫 2x 轻量网络（源自 mpv 实时播放生态） |
| `AnimeSharp-x4` | 32 MB | 动漫 4x 锐化经典，线条极其利落 |
| `UltraSharp-x4` | 32 MB | 照片 / 综合 4x 社区经典，锐利保纹理 |
| `Remacri-x4` | 31 MB | 照片 / 综合 4x 社区经典，观感自然温和 |
| `NMKD-Siax-x4` | 32 MB | 适合干净至轻度压缩源的细节保留 4x 模型 |
| `BSRGAN-x4` / `BSRGAN-x2` | 32 MB | ICCV 2021 实用多劣化模型（Apache-2.0） |

---

## 4. 已知排除项

- **`Real-CUGAN se x4`（`up4x-*`）**：其 PixelShuffle 主干拓扑在当前锁定的 ncnn（`20260526`）下 CPU 与 Vulkan 均会输出异常花屏；x2 / x3 不受影响。导入此类模型时引擎会打印警告。
- **`Real-CUGAN se x2 denoise1x/2x`**：默认目录仅提供强降噪（`denoise3x`）变体；如需温和档位可自行用 `upix-cli model import` 导入。
- **`Anime4K` / `ArtCNN`**：属于内置权重的 mpv GLSL 着色器管线，并非可导入的神经网络权重文件。
- **`waifu2x` / `SRMD`**：画质已被上述现代模型全面超越，不再列入官方目录。

---

## 5. 元数据约定与延伸文档

- **文件名规则**：倍率必须由末尾 `-xN`（如 `-x2`、`-x4`、`-x1`）显式声明；特别是 `1x` 修复模型图内无上采样算子，依赖文件名后缀识别倍率。
- **场景与许可详情**：
  - 上游作者、许可证与转换校验溯源见 [LICENSES.md](LICENSES.md)（注意部分社区经典模型为 `CC-BY-NC-SA-4.0` 非商用协议）。
  - 添加新模型的转换与代码同步规范见 [CONVERTING.md](CONVERTING.md)。
