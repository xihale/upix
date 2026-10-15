# 模型效果回归门 — 取舍记录

一句话：**换 ncnn、改自定义层、动 `model_graph` 分析之后，像素还是不是同一个
模型？** `tests/model_oracle/` 用 6 个发行模型跑真实单图 CPU 路径，和提交进仓库的
golden PNG 比 PSNR/maxdiff。本文记录每条用例锚定什么、阈值怎么定、哪些断言故意
不加。

## 这个门不管什么

| 问题 | 归谁 |
| --- | --- |
| 模型本身是否忠于原 pth（转换质量） | `models/CONVERTING.md` 的转换期验收 |
| daemon 协议/并发/生命周期/输入滥用 | `tests/serve`、`tests/serve_stress` |
| 跨引擎（GPU vs CPU）像素一致性阈值 | `docs/serve-tests-design.md`（C4，PSNR≥24） |
| 速度是否退化 | `scripts/bench_*.sh`、`upix-perf {list-probes,...}` |
| 输出容器/EXIF/XMP 标记 | `test_image_avif` / `test_exif` / `test_sr_marker` |

本门只回答一件事：同一条单图路径，给定模型 + 给定输入，输出的像素有没有变。

## 用例（每条锚定一个 arch 分支）

| case | 模型 | 锚定 |
| --- | --- | --- |
| `span-x2` | NomosUni-SPAN-x2 | SPAN 的 Conv3XC 重参数化（1x1-sk + bottleneck 必须精确折进单个 3x3） |
| `compact-anime-x2` | Real-ESRGANv3-anime-x2 | SRVGGNetCompact + PixelShuffle 尾巴（手机默认动漫网） |
| `compact-archivist-x1` | Archivist-Medium-Compact-x1 | x1：上采样器退化为恒等 + 全局残差 |
| `realplksr-x4` | NomosWebPhoto-RealPLKSR-x4 | RealPLKSR partial large-kernel 块 + x4 上采样 |
| `rrdb-x2plus` | Real-ESRGAN-x2plus | RRDB 主干 + pixel-unshuffle 头（名字 x2、干是 x4） |
| `cugan-x2` | Real-CUGAN-no-denoise-x2 | Real-CUGAN deconv 上采样尾巴 + Crop/pre-pad 图分析分支 |

六个模型都在发行目录里（`upix-cli model download <name>` 可复现，sha256 由发布
manifest 钉住），合计约 50 MB。用例集合覆盖的是**代码分支**，不是模型热度：加用例
的标准同 `docs/serve-tests-design.md` —— 必须锚定一个具体的层/图分析分支，否则不加。

## 输入与参数

输入是 64×64 RGB，由 `tests/model_oracle/cases.py` 的显式 LCG + 固定整数图案生成
（不用 `np.random`，那套流不保证跨版本稳定）：噪声底 + 两片纯黑/纯白块（抓归一化与
缩放错误）+ R 通道线性渐变（抓通道交换与错位）+ B 通道正弦（抓过度平滑与死输出）。
输入自身不进仓库，但 sha256 记在 `golden/meta.json`，改生成器会被门拦下。

跑 `--tile-size 256 --tile-overlap 16`，即整图单 tile（64² 输入 ≤ 256，无接缝）。

## 为什么不加 tile 一致性的断言

改 tile 确实会改像素，而且是**合法**的：SR 网在 tile 边界处缺少感受野上下文，重叠
区又是两次不同预测的混合。实测（同一构建、128² 输入、tile 64/16 vs tile 512）：

| case | PSNR | maxdiff |
| --- | --- | --- |
| span-x2 | 50.9 dB | 9 |
| compact-anime-x2 | 47.2 dB | 13 |
| compact-archivist-x1 | 46.3 dB | 12 |
| realplksr-x4 | 33.9 dB | 50 |
| rrdb-x2plus | 36.1 dB | 58 |
| cugan-x2 | 21.3 dB | 135 |

`docs/serve-tests-design.md` 里"tile 划分对输出无影响、位级一致"指的是
**256/384/auto 之间**（4-pass disjoint tile batching 的目标），不是 64 级小 tile；
跨路径/跨 tile 的位级比较必须先显式钉 tile。因此 golden 一律单 tile，tile 行为由
serve 套件的 C4 阈值覆盖，这里不再造一条注定要放宽的断言。

## 阈值依据

- **同机同构建：位级一致。** 实测 6 个用例 run-to-run 全部 `PSNR=inf, maxdiff=0`；
  3 个代表性模型在 `taskset -c 0` 与全核之间也位级一致（CPU 卷积按输出通道切分，
  不随线程数改结果）。
- **门限：PSNR ≥ 45 dB 且 maxdiff ≤ 3。** 这条留的是跨 CPU ISA（`--native_isa` 默认
  开，不同机器会选到不同 SIMD kernel）与跨 ncnn 版本的亚 LSB 漂移余量。坏掉的东西
  远低于此：错归一化/错上采样是 10–30 dB（实测把 golden 平移 1px 就是 15.6 dB）。
  跨机器漂移未实测（手头只有一台机器），45 dB 是判断值而非测值 —— 结论标
  `[INFERENCE]`。若 CI 报了亚阈值但显然是良性的漂移，**显式重建 golden 并提交**，
  不要静默放宽阈值（同 serve 套件对阈值定标的纪律）。
- **与 golden 无关的硬门**（任何机器都必须过）：输出尺寸 == 输入 × scale、
  `std ≥ 2`、`mean ∈ [8, 247]`。这三条在 golden 漂移之前先挡住"输出全黑/常量/糊成
  一团"这类崩坏。

## golden 与 provenance

`golden/<case>.png` + `golden/meta.json`（记录设备 `cpu`、输入 sha256、模型
`.param/.bin` 合并 sha256、golden 自身 sha256、tile/overlap/scale）。检查时来源不一致
会先失败并指名原因，避免把换后端、改输入生成器或静默换模误读成引擎漂移。
失败会写 x8 放大的差异图并保留工作目录，供人眼确认。

审计发现旧 golden 实际来自 GPU：六个 case 与旧二进制 GPU 输出全部逐像素相同，
而显式 CPU 路径与它们相差 18.73–31.90 dB。校正前已逐一确认旧二进制禁用 Vulkan
所得 CPU 输出与当前 `--cpu` 输出全部逐像素相同，再重建 CPU golden；没有放宽阈值。
后续必须显式使用 `--cpu --no-daemon`，不能把默认 GPU 优先路径当作 CPU 参考。

## 升级 ncnn 的流程

1. 改 `xmake.lua` 里的 ncnn 版本（`add_requires("ncnn~runtime <ver>")`），重建
   （Android 侧同时 `scripts/build_native.sh`）。
2. 跑 `scripts/run_model_oracle.sh --require-models`。
3. 全部 ≥ 45 dB → 亚 LSB 漂移，直接过；这正是本门想确认的事情。
4. 明显漂移 → 看 `*_diff.png` 与 PSNR：判断是 ncnn 的数值改动（可接受，需要人工确认
   画质）还是图/层语义被改坏（回归，回退版本）。确认可接受后
   `--update-golden`，把 `golden/*.png` + `meta.json` 与版本号一起提交（`--update-golden`
   会打印与旧 golden 的 PSNR/maxdiff，作为该提交的评审依据）。
5. 同时跑 `UPIX_KERNEL_SELFTEST=1` 冒烟（`DEVELOPMENT.md` 坑 4）与 serve E2E。

## 已知边界

- 只有 CPU。GPU golden 会绑死显卡/驱动，跨机器不可移植；GPU 侧靠
  `UPIX_KERNEL_SELFTEST=1`、serve 套件与双端人工比对。
- 6/44 个模型：覆盖 6 个 arch 分支，不覆盖全部权重。
- 不做像素级"画质好坏"判断（没有参考实现/无参考指标），只做漂移与崩坏检测；
  画质下限由 `models/CONVERTING.md` 的转换期验证负责。
