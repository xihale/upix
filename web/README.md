# Upix Web（实验通道）

浏览器端超分前端 —— issue [#2](https://github.com/xihale/upix/issues/2) **路径 B1**（pnnx→ONNX + onnxruntime-web）的落地，内置**路径 C**（WebNN）实验插桩。与 CLI / Desktop / Android 共享同一套 tile 语义（引擎 `tile_processor` / blend shader 的逐行移植），现有三端不动。

## 运行

```sh
cd web
npm install            # 首次需批准 onnxruntime-node / esbuild 的 install scripts
npm run dev            # http://localhost:5175 （dev server 已带 COOP/COEP 头）
npm test               # 数值门：onnxruntime-node 跑 layout/blend/e2e（无需浏览器）
npm run build          # tsc --noEmit + vite build → dist/
```

拖入图片 → 选模型/后端/分块 → 超分 → 对比滑块 → 下载 PNG。全程本地推理，图片不上传。

## 执行后端

| 后端 | 说明 |
|---|---|
| WebGPU | 默认（`navigator.gpu` 可用时）；ORT-Web WebGPU EP |
| wasm | 回退；`crossOriginIsolated` 时启用多线程（需要 COOP/COEP 响应头） |
| WebNN（实验） | 路径 C 插桩：自动切换到 PixelShuffle 拆解版模型（单分区图），`?bench=1` 出三后端对比表。WebNN 只有 Chromium 支持，GPU/NPU 执行仍 limited，实测需 Windows NPU / Android 旗舰真机 |

## 模型

- `public/models/`：开发用种子固定 fixture（`tools/make_dev_model.py` 生成，SPAN 风格图结构，复刻 traiNNer 的 in-graph 归一化约定 `(x−mean)·255`）
- 正式模型：`scripts/convert_onnx.py`（仓库根）按 `models/CONVERTING.md` 全流程出 ONNX（fp16/fp32 双版 + §5 验证门），产物随后续 `models-onnx` release 发布；`public/models/manifest.json` 与目录 `manifest.json` 同 schema（name/scale/license/sha256/url），web 端 fetch 后按 sha256 校验

## 数值门（`npm test`，对应 CONVERTING.md §5 的口径）

- 单 tile 退化布局 tiled==whole ≥99 dB（float 域，结构门）
- 多 tile 接缝 ≥30 dB（感受野边界地板：conv 边界污染 ~RF 深度像素，平方 ramp 压低污染侧权重——与引擎同一设计取舍，所以地板相同）
- 8-bit 输出 ≥60 dB（量化舍入翻转的噪声底）
- 奇数尺寸精确（127×125 → 254×250）
- layout/blend 与引擎逐行对拍（`compute_tile_layout`、`blend_weight` 平方 ramp、`overlap×scale`）

## 已知限制

- **LAN 协同不可用**：HTTPS 页面连 `ws://内网IP` 是 mixed content 被禁（issue #2）
- **wasm 多线程**：需要 COOP/COEP 响应头；GitHub Pages 不支持自定义响应头，生产若需多线程要迁 Cloudflare Pages 等（WebGPU EP 不受影响）
- `dist/assets` 里的 28MB asyncify wasm 是 ort bundle 的懒加载附带产物（实际只用 `public/ort/` 里的 jsep 运行时），精简打包是后续优化
- WebNN：API 仍在漂移（Chrome 153 改过枚举名），本实验代码已做特性探测 + 失败回退，不保证跨版本稳定
