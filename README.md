# CrossScale

Distributed image super-resolution system: standalone CLI + Android app, powered by ncnn.

## 🚧 Work in Progress

This project is under active development. Source code is not published yet.
This repository currently hosts the **model files** only.

项目正在开发中，源码稍后发布，当前仅提供模型文件。

## Models

ncnn models live under `models/<name>/` (`x2.param`/`x2.bin`, ...):

| Model | Scales |
| --- | --- |
| `Real-ESRGANv3-anime` | x2 |
| `Real-CUGAN-conservative` | x2, x3 |
| `Real-CUGAN-denoise` | x2, x3 |
| `Real-CUGAN-no-denoise` | x2, x3 |

Model weights come from the upstream [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) and [Real-CUGAN](https://github.com/bilibili/ailab) projects, converted to ncnn format.
