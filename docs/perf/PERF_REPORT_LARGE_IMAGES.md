# 真实大图性能报告(10–50 MP)

> 测试日期: 2026-08-15 ｜ 机器: NVIDIA GeForce RTX 4060 Laptop GPU (8 GB) + Intel UHD
> 模型: Real-ESRGANv3-anime ×2 (fp16) ｜ CLI: `crossscale-cli process --no-daemon`
> 测试图: 本机真实照片/截图(`~/Pictures`,4.6–50.4 MP RGB),非合成图
> 对照此前报告: 固定开销研究见 `PERF_REPORT_REAL_IMAGES.md`(≤1 MP 合成图),本报告补齐大图视角

---

## 一、TL;DR

| # | 发现 | 数据 |
|---|---|---|
| **1** | **大图场景瓶颈彻底转移:GPU 推理占 66%,且已算满** | 50 MP 图 15.3s 中推理 10.3s(submit 占用 96%),固定开销(init+load)仅 ~10% |
| **2** | **PNG 压缩成为第二瓶颈(17%),单线程不可再分** | 201 MP 输出:压缩 2.45s + filter 0.11s;zlib 单流限制无法并行 |
| **3** | **tile 越大越慢(大图上),auto-tile=200 恰好最优** | 50 MP:tile 200→10.3s,256→11.3s,384→16.2s,512→17.6s(纯 GPU 时间) |
| **4** | **目录模式多引擎并发 + LPT 调度 = 1.72×** | 5 图(含一张 50 MP):22.0s → 12.8s(`--gpu --jobs 4`) |
| **5** | **daemon 遇大图直接失败(已修)** | 输出 >100 MB 时曾报错退出且无输出;现回落 inline,结果 md5 一致 |

**结论**:小图时代的两大瓶颈(启动开销、submit 串行)在当前代码里都已解决——单引擎管线已把 GPU 喂到 96%,多引擎目录并发可再提聚合吞吐。大图的剩余成本是**有效计算本身 + PNG 编码**,前者已近最优,后者是格式固有成本(改 `--output-format jpg` 立省 ~2.7s)。

---

## 二、测试集与阶段分解

### 2.1 真实图集

| 图像 | 尺寸 | 输入 MP | ×2 输出 | 文件 |
|---|---|---|---|---|
| big | 7000×7200 | 50.4 | 201.6 MP | 61.6 MB PNG |
| mid | 3442×3840 | 13.2 | 52.9 MP | 12.8 MB PNG |
| tall | 2376×4632 | 11.0 | 44.0 MP | 7.6 MB PNG |
| wide | 3840×1600 | 6.1 | 24.6 MP | 5.6 MB PNG |
| small | 2250×2250 | 5.1 | 20.3 MP | 5.9 MB PNG |

### 2.2 阶段分解(50 MP → 201 MP PNG,优化后 15.25s)

| 阶段 | 耗时 | 占比 | 说明 |
|---|---|---|---|
| PNG 解码 | 0.27 s | 2% | libpng,61.6 MB 文件 |
| Vulkan init | 1.29 s | 8% | 小图场景的 39% 占比在大图被摊薄 |
| 模型加载 | 0.21 s | 1% | |
| **GPU 推理管线** | **10.28 s** | **67%** | 1520 tiles / 24 submits,submit 等待占 96% → GPU 真实计算饱和 |
| 收尾(download+量化) | ~0.6 s | 4% | |
| **PNG 编码** | **~2.6 s** | **17%** | filter 0.11s + libdeflate level1 压缩 2.45s + 写盘 |

各图管线计时均为 **submit 96% / record 3%** —— 单引擎已无 GPU 饥饿,与旧报告"util 15%"的代码状态不同。

---

## 三、判别实验:GPU 时间花在哪

| 实验 | 结果 | 推论 |
|---|---|---|
| tile 扫描 200/256/384/512 | GPU 时间 10.3/11.3/16.2/17.6s | 大 tile 反而慢:200² 恰是本 GPU+模型的效率甜点,auto-tile 选择正确 |
| overlap 0 vs 16 | tiles −17%,GPU 时间 −21% | GPU 时间 ∝ tile 数 → 纯推理主导,blend/融合不是热点 |
| batch 2 vs 4 vs 8 | 10.3 / 10.7 / 9.7s | ±5%,噪声级,不动 |
| 目录 jobs 1 vs 2(优化前) | 22.36 vs 22.42s | 无收益:最大图 13.4s 排在队尾构成关键路径(调度问题,非并发问题) |

---

## 四、已实施的优化(3 项,均已验证)

### 4.1 目录模式 LPT 调度(最大图优先) `cli/src/process_batch.cpp`

原顺序来自 `recursive_directory_iterator`(文件系统哈希序,实测近似小图优先),最大图永远垫底,并发 worker 闲置等待。改为按文件大小降序:

| 配置(5 图,含 50 MP) | 优化前 | 优化后 |
|---|---|---|
| `--gpu --jobs 1` | 22.36 s | 22.00 s(单 worker 次序无关) |
| `--gpu --jobs 2` | 22.42 s | 20.67 / 21.00 s |
| `--gpu --jobs 3` | — | 21.73 s |
| **`--gpu --jobs 4`** | — | **12.80 / 12.38 s(1.72×)** |

jobs=4 显著快于 2/3:小图群先被 4 引擎并吞掉,50 MP 大图随后近似独占 GPU。并发多引擎的聚合吞吐**超过**单引擎(引擎间互相填补 record/下载间隙),与 perf 框架当年"并发 util 96%"的结论一致。jobs=3 慢于 jobs=2 属争用模式差异,如实记录。
✅ 输出 md5 与单进程逐字节一致。

### 4.2 daemon 大响应回落 inline `cli/src/cmd_process.cpp`

原:输出超过 `MAX_NETWORK_PAYLOAD`(100 MB)→ `ERROR daemon response too large`,**退出且不产出文件**(13 MP ×2 ≈ 158 MB 即触发)。
现:告警并回落 inline 路径,结果 md5 一致。
✅ 13 MP 图经 daemon 现在成功;5 MP 图(<100 MB)仍走 daemon 快路径 1.80s(inline 2.27s)。

### 4.3 PNG 保存微优化 `share/utils/image.cpp`

- AVG filter 按行并行(行间只读原始行,天然独立;>16 MB 才开线程)
- `filtered`(~600 MB)与 `compressed bound`(~600 MB)两个缓冲改 `make_unique_for_overwrite`,消除两遍整页零初始化

50 MP 图:filter 274→106 ms,memset 省 ~0.2s,单图 wall 15.79→15.25s(−3.4%);输出逐字节一致(md5)。

同时新增永久计时埋点:`CROSSSCALE_PIPELINE_TIMER=1` 时打印 `png save WxH: filter=Xms compress=Yms`。

---

## 五、遗留瓶颈与建议(按 ROI)

| # | 项 | 现状 | 建议 |
|---|---|---|---|
| 1 | libdeflate 单流压缩 2.45s(50 MP 图 16%) | zlib 单流无法多线程;level 已是 1 | 用户侧:`--output-format jpg` 立省 2.7s(12.8 vs 15.5s);格式不可换则接受 |
| 2 | GPU 推理 10.3s | 已饱和(fp16+tile200+chunk64) | 仅剩换模型/换硬件空间;`out_gpu` float32 累加缓冲(201 MP×4B=0.8GB)改 fp16 理论省一半 blend 带宽,但 overlap 实验表明 blend 占比小,预期收益有限 |
| 3 | 输出 Image 构造零初始化 ~600 MB | `vector::resize` 先 memset 再被 GPU 下载覆盖 | ~0.1s,不值得改 Image 语义,记录在案 |
| 4 | ×4 模型 + 大图的 VRAM | 全图 float32 累加缓冲:50 MP×4 输出需 ~4.8+1.6 GB | **已解决(八节)**:预检拒绝 + tile 缩小重试 + CPU 混合回退阶梯 |
| 5 | 目录模式默认 jobs=1 | 多引擎收益需用户显式 `--jobs 4` | **已解决(八节)**:默认按 VRAM 准入控制自动选 jobs |

---

## 六、复现

```bash
# 阶段分解 + 管线计时 + PNG 计时
CROSSSCALE_PIPELINE_TIMER=1 \
  ./build/linux/x86_64/release/crossscale-cli process -i big.png -o out.png --no-daemon

# tile / overlap / batch / 格式单变量 A/B
... --tile-size {200,256,384,512}   # 大图上 200 最优
... --tile-overlap {0,16}           # 判别推理 vs blend 占比
... --output-format jpg             # 绕过 PNG 压缩尾部
... --batch-size {2,4,8}            # 无显著差异

# 目录并发(LPT 已内置,大图自动先跑)
$CLI process -i <dir> -o <out> --gpu --jobs 4

# daemon 行为(小图快路径 / 大图自动回落)
$CLI serve start
$CLI process -i small.png -o o.png   # 走 daemon
$CLI process -i big.png -o o.png     # 回落 inline,结果一致
```

---

## 七、与既有报告的关系

| 报告 | 场景 | 瓶颈结论 |
|---|---|---|
| `PERF_REPORT_GPU_EFFICIENCY.md` | 合成 tensor | submit 序列化(旧代码) |
| `PERF_REPORT_GPU_EXHAUSTIVE.md` | 合成 tensor | fp16 是核心速度来源 |
| `PERF_REPORT_REAL_IMAGES.md` | ≤1 MP 合成图端到端 | 固定开销 60% 主导;daemon 破局 |
| **本报告** | **4.6–50 MP 真实图** | **推理 66% 已饱和 + PNG 编码 17%;并发调度是批量场景的最后一块肉(已吃掉)** |

---

## 八、第二轮:OOM 预估与回退(2026-08-15)

### 8.1 问题:OOM 不是报错,是段错误或黑图

三条 GPU tile 管线(u8 单图 / u8 queue / float)都在显存里分配**整张输出图**的缓冲:

```
float 累加器 (出图像素×C×4B) + 权重图 (×4B) + u8 下载暂存 (×C×1B)
50 MP ×2 ≈ 3.8 GB;×4 ≈ 15 GB —— 与 tile 大小无关
```

而 `VkMat::create` 分配失败时**静默返回空 mat**,ncnn 不报错;后续 `record_clone`/dispatch 解引用空 buffer → SEGV(实测:`-j 4` 并发 5 图时在 96% 进度处 `vkAllocateMemory -2` → 空暂存 → 段错误,rc=139)。

### 8.2 已实施(全部验证)

**① 分配前预估(preflight)** `share/core/ncnn_gpu_pipeline.cpp`
三条管线在分配任何大缓冲之前,用公式(全图输出 + 输入上传 + tile 工作集 `(t+2×16)²×64ch×2B×16×batch`)对比 `vkdev_->get_heap_budget()`,放不下直接返回 `kErrOutOfMemory`(230),零成本拒绝。预算查询收口到 `Engine::get_device_memory_budget_mb()`,支持 `CROSSSCALE_GPU_FORCE_BUDGET_MB`(测试钩子,模拟小显存设备)。

**② 分配后空 mat 检查** 所有全图级 `VkMat::create`(含循环深处最后一批的下载暂存)后检查 `data == nullptr`(`buffer()` 会解引用 null,不能用)→ 同样转成 OOM 错误。这把"96% 进度段错误"变成干净错误。

**③ 回退阶梯** `share/core/tile_processor.cpp` `process_image_u8`/`process_image`:

```
GPU(tile) → OOM → tile 减半(≥64)重试 → 仍 OOM → CPU 混合管线
(全图累加缓冲移到主机内存,GPU 只做 tile 级推理)
```

`process_u8` 内部同样:u8 queue → OOM → float 管线 → OOM → CPU 管线。CPU 路径分配全图 float 缓冲前用 `/proc/meminfo` 的 `MemAvailable` 预检(不用 `sysinfo.freeram`——不含可回收缓存,长期偏低会误拒)。

**④ 目录模式 jobs 准入控制** `cli/src/process_batch.cpp`
PNG/JPEG 头嗅探每张图尺寸 → 估算全图输出显存需求(~20 B/输出像素)→ LPT 序前 W 大图之和 ≤ 90% 预算的最大 W(≤4)即并发数。默认(未给 `-j`)直接用它;显式 `-j` 超出时**封顶并告警**(实测 `-j 4` 在 6 GB 预算下封到 2,5/5 成功;不封顶则 3/5——CPU 混合回退仍需 GPU 推理,显存被并发引擎占满时连 tile 都分配不了)。引擎 init 失败也从整批失败改为降级到已就绪的 worker。

### 8.3 验证矩阵(RTX 4060 Laptop 8 GB,预算 ~6.1 GB)

| 场景 | 结果 |
|---|---|
| 单图 5 MP / 50 MP(正常路径) | rc=0,md5 与第一轮基线逐字节一致 |
| 目录默认(5 图,自动 jobs=2) | 16.7–20.7s,5/5,md5 全对(旧:jobs=1 静默 22s) |
| 目录显式 `-j 4` | 封顶告警→jobs=2,5/5,md5 全对(旧:**段错误**;仅加空检查后:3/5) |
| `FORCE_BUDGET_MB=600` + 13 MP | 默认 tile 被压到 64 → 预检拒 → CPU 混合,rc=0,PSNR vs 基线 51.1 dB |
| `FORCE_BUDGET_MB=900` + 13 MP | 默认 128 → 缩小重试 64 → CPU 混合,rc=0,PSNR 51 dB |
| daemon 周期 | 输出与旧 daemon 基线 md5 一致 |
| ASAN `-j 4` 全批 | 无 SEGV/越界(此前两个 UB:`std::async` 按引用捕获循环变量 `i` 悬空;`Image` 无 padding 被 `record_clone` 16B 对齐读越界 4B——已修,`Image::size()` 改为返回 w×h×c) |

CPU 混合回退的输出与 GPU 管线有 tile 边界舍入差异(PSNR 51 dB),非 bit 一致但内容等价。

### 8.4 Android / remote 模式与 LPT

- **LPT(最大图优先)只在 CLI 目录并行模式有意义**。Android 端 `ProcessingRunEngine.kt` 的 `for (item in runImages)` 是串行逐张(native 单 session / remote 单 coordinator),串行总时长与顺序无关,无需也不适用 LPT。
- Android native 与 remote(daemon 走 `process_u8`)都经过本轮的预检/回退:引擎层(`share/core`)是共享的,Android 直接继承 ①②③;remote 的 serve daemon 同样。
- 唯一 CLI 专属的是 ④(准入控制),Android/remote 串行不涉及。
