# 模型许可与来源归属

Upix 代码主体采用项目根目录 [LICENSE](../LICENSE)（AGPL-3.0）。模型权重按 ncnn 格式重新打包分发，**各模型均保留其上游原始许可证**；重新分发模型包时请保留下列归属声明。

> **商用提示**：`UltraSharp-x4`、`AnimeSharp-x4`、`AnimeJaNai-v2-Compact-x2`、`Remacri-x4` 权重采用 **CC-BY-NC-SA-4.0**（非商业性使用），商业用途请勿携带这些权重，可改用 BSD-3-Clause / CC-BY-4.0 / Apache-2.0 模型。

## 归属与许可表

| 模型 / 系列 | 上游来源 | 许可证 | 转换与校验溯源 |
|---|---|---|---|
| **Real-ESRGAN v3 anime & x4plus / x4plus-anime** | [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)（`realesrgan-ncnn-vulkan`） | BSD-3-Clause © Xintao Wang et al. | 官方 ncnn 包 |
| **`Real-ESRGAN-x2plus`** | [Real-ESRGAN v0.2.1](https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.1) | BSD-3-Clause © Xintao Wang, Kaiwen Zhou et al. | 官网 `.pth` sha256 校验后以 pnnx 转换（Pixel-Unshuffle 头降为 Reorg），与 spandrel 对齐 0 LSB |
| **`Real-ESRGANv3-general-x4`** | `realesr-general-x4v3`；ncnn 来自 [tumuyan2/realsr-models](https://huggingface.co/tumuyan2/realsr-models) | BSD-3-Clause（权重） | 复用开源 ncnn 转换版 |
| **Real-CUGAN（se / pro）** | [bilibili ailab Real-CUGAN](https://github.com/bilibili/ailab/tree/main/Real-CUGAN)；[realcugan-ncnn-vulkan](https://github.com/nihui/realcugan-ncnn-vulkan) | MIT（ncnn 转换）；权重遵循上游 Real-CUGAN 条款 | 官方 ncnn 包 |
| **`Nomos8kSC-x4`** | [Phhofm/models](https://github.com/Phhofm/models)；ncnn 来自 [upscayl/custom-models](https://github.com/upscayl/custom-models) | CC-BY-4.0（权重） | 复用 upscayl ncnn 包 |
| **`NomosUni-Compact-x2`** | [Phhofm/models](https://github.com/Phhofm/models)（`2x-NomosUni-compact-multijpg`） | CC-BY-4.0 | pnnx 转换，与 fp32 PyTorch 参考对齐 |
| **`Parimg-Compact-x2`** | [Phhofm/models](https://github.com/Phhofm/models)（`2x-ParimgCompact`） | CC-BY-4.0 | OpenModelDB sha256 核验 + pnnx，与 spandrel 对齐 0 LSB |
| **`LSDIR-Compact-v2-x4`** | [Phips/4xLSDIRCompact2](https://huggingface.co/Phips/4xLSDIRCompact2) | CC-BY-4.0 | 作者 fp16 safetensors 转 pnnx，与 fp32 参考对齐 |
| **`NomosUni-SPAN-x2` / `-x4`** | [Phhofm/models](https://github.com/Phhofm/models)（`NomosUni_span_multijpg`） | CC-BY-4.0 | 复现 Conv3XC eval 重参数化与归一化语义，与 spandrel fp32 对齐 |
| **`HFA2k-LUDVAE-SPAN-x2`** | [Phhofm/models](https://github.com/Phhofm/models) | CC-BY-4.0 | OpenModelDB sha256 核验 + pnnx，与 spandrel fp32 对齐 |
| **`NomosWebPhoto-RealPLKSR-x4`** | [Phhofm/models](https://github.com/Phhofm/models) | CC-BY-4.0 | 通道重复残差改写为逐位等价最近邻插值，pnnx 转换后与 fp32 对齐 |
| **`HFA2k-x4` / `Nomos2-OTF-x4`** | [Phhofm/models](https://github.com/Phhofm/models) | CC-BY-4.0 | OpenModelDB sha256 核验 + pnnx，与 spandrel 对齐 0 LSB |
| **`DeJPG` / `DeNoise` / `DeH264-RealPLKSR-x1`** | [Phhofm/models](https://github.com/Phhofm/models) | CC-BY-4.0 | OpenModelDB sha256 核验 + pnnx（scale=1 恒等上采样），与 spandrel 对齐 0 LSB |
| **`ModernSpanimation-x2`** | [TNTwise](https://github.com/TNTwise)（`2x-ModernSpanimationV1`） | MIT | OpenModelDB sha256 核验 + pnnx，与 spandrel fp32 对齐 |
| **`Archivist-Medium` / `Archivist-Rough-Compact-x1`** | [Archivist-Project-Denoiser](https://github.com/Loganavter/Archivist-Project-Denoiser) | MIT | OpenModelDB sha256 核验 + pnnx，与 spandrel 对齐 0 LSB |
| **`NES-Composite-RGB-Compact-x1` / `Genesis-Cleanup-Compact-x1`** | [pokepress](https://github.com/pokepress) | MIT | OpenModelDB sha256 核验 + pnnx，与 spandrel 对齐 0 LSB |
| **`NMKD-Siax-x4`** | [NMKD](https://nmkd.de)（`4x_NMKD-Siax_200k`） | WTFPL | OpenModelDB sha256 核验 + pnnx，与 spandrel fp32 对齐 |
| **`BSRGAN-x4` / `BSRGAN-x2`** | [cszn/KAIR](https://github.com/cszn/KAIR)（ICCV 2021） | Apache-2.0 | OpenModelDB sha256 核验 + pnnx，与 spandrel 对齐 0 LSB |
| **`UltraSharp-x4` / `AnimeSharp-x4`** | [Kim2091](https://github.com/Kim2091)（HuggingFace 镜像） | **CC-BY-NC-SA-4.0** | pnnx 转换，与 fp32 PyTorch 参考对齐 |
| **`AnimeJaNai-v2-Compact-x2`** | [the-database/AnimeJaNai](https://github.com/the-database/AnimeJaNai) | **CC-BY-NC-SA-4.0** | pnnx 转换（`params_ema`），与 fp32 参考对齐 |
| **`Remacri-x4`** | foolhardy（[HuggingFace 镜像](https://huggingface.co/FacehugmanIII/4x_foolhardy_Remacri)） | **CC-BY-NC-SA-4.0** | OpenModelDB sha256 核验 + pnnx，与 fp32 参考对齐 |
