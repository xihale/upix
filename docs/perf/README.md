# 性能测试报告索引

> 历史性能数据快照，按时间/主题归档。每份报告记录特定测试日的硬件、模型、
> 方法与结论。结论已沉淀进架构（如 tile_size 默认值、serve daemon、并发引擎），
> 这些报告保留作为决策依据与回归对照基线。

测试机基准：NVIDIA GeForce RTX 4060 Laptop GPU (8GB) + Intel UHD，模型 Real-ESRGANv3-anime ×2 (fp16)。

| 报告 | 日期 | 主题 | 核心结论 |
|------|------|------|----------|
| [`PERFORMANCE_RESULTS.md`](PERFORMANCE_RESULTS.md) | 2026-02-16 | tile 尺寸 vs 性能基线 | 256 是该图尺寸的最优 tile；auto-tile 选 128 偏小 |
| [`GPU_EFFICIENCY_REPORT.md`](GPU_EFFICIENCY_REPORT.md) | 早期 | GPU 效率初测 | 小图 SM util 仅 6-13%，GPU 欠载 |
| [`GPU_OPTIMIZATION_FINAL.md`](GPU_OPTIMIZATION_FINAL.md) | 早期 | GPU 优化小结 | 优化目标 100% util（阶段性） |
| [`PERF_REPORT_GPU_EFFICIENCY.md`](PERF_REPORT_GPU_EFFICIENCY.md) | 2026-07-23 | 合成 tensor 隔离推理 | submit 序列化瓶颈；并发多引擎 util 59%→96% |
| [`PERF_REPORT_GPU_EXHAUSTIVE.md`](PERF_REPORT_GPU_EXHAUSTIVE.md) | 2026-07-23 | 穷尽探索（2D 网格/精度/残差） | fp16 是核心速度来源，配置已最优 |
| [`PERF_REPORT_REAL_IMAGES.md`](PERF_REPORT_REAL_IMAGES.md) | 2026-07-23 | 真实图像端到端 | 单图 GPU≈CPU（固定开销占 60%）；批量复用引擎 GPU 快 CPU 4× |
| [`PERF_REPORT_LARGE_IMAGES.md`](PERF_REPORT_LARGE_IMAGES.md) | 2026-08-15 | 真实大图（10–50 MP）+ OOM 治理 | 推理 66% 已饱和 + PNG 编码 17%；LPT 并发批量 1.72×；OOM 预检/缩小重试/CPU 混合回退阶梯 + jobs 按 VRAM 准入控制 |

## 关键结论演进

1. **单图瓶颈是固定开销，不是推理**：单图 `crossscale process` 中 Vulkan init (39%) + model load (19%) 占 60%，GPU 优势被淹没。→ 直接催生 **serve daemon**（复用热引擎，单图 2-15× 加速）。
2. **批量场景 GPU 优势爆发**：7 图批量 GPU 1.22s vs CPU 4.91s（4.1×），因为引擎只初始化一次。→ 目录模式 + EnginePool 多引擎并发。
3. **tile_size 默认 256** 是实测最优（256 vs 128/384/512 对比）。大图（≥10 MP）上 auto-tile=200 更优，tile 越大越慢。
4. **fp16 是 GPU 核心加速来源**，默认开启；驱动 bug 时用 `CROSSSCALE_GPU_DISABLE_FP16=1` 关闭。
5. **大图瓶颈是推理本身 + PNG 编码**（各 66%/17%，2026-08）：GPU 管线已饱和（submit 96%），批量场景靠 LPT 大图优先调度 + `--jobs 4` 多引擎再拿 1.72×。
6. **OOM 必须预估，不能等它发生**（2026-08）：GPU 管线的全图输出缓冲与 tile 无关（50 MP×2 ≈ 3.8 GB），`VkMat::create` 失败静默返回空 mat → 段错误。解法：分配前按 heap budget 预检 + 空 mat 检查 + "tile 减半重试 → CPU 混合"回退阶梯；目录并发数按 LPT 前 W 大图显存需求准入控制（默认与显式 `-j` 都生效）。

> 注：这些报告引用的是当时的代码状态（如 `process` 单图、submit 序列化）。
> 部分瓶颈（如 submit 序列化）已通过并发多引擎优化解决，阅读时注意时间线。
