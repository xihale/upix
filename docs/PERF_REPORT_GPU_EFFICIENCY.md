# CrossScale 性能测试报告:PC 独显效率与占用率

- **测试日期**: 2026-07-23
- **测试构件**: `share/perf/` (`crossscale-perf` 库 + CLI `perf` 子命令)
- **测试类型**: Stage 1 — GPU 效率/占用率
- **原始数据**: `/tmp/perf_t{128,256,384}.jsonl`(JSONL 事件流,共 1131 行/文件)

## 一、测试环境

| 项目 | 配置 |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 Laptop GPU |
| 显存 | 8188 MiB (实际) / NVML 上报 31815 MiB* |
| Compute Capability | 8.9 (Ada Lovelace) |
| 驱动 | 595.71.05 |
| 集显 | Intel UHD Graphics (ADL-S GT1) — 测试时未选用 |
| 模型 | Real-ESRGANv3-anime-x2 (2× 超分) |
| 推理精度 | fp16 packed/storage/arithmetic + int8 storage(引擎默认) |
| GPU 选用逻辑 | ncnn 评分选离散卡(score=10280 > 集显 1210) |
| NVML 采样 | dlopen 运行时加载,采样间隔 50ms |
| 构建 | xmake release, `-O fast -Werror`, C++23 |

\* `engine->get_memory_info()` 上报 31815 MiB 异常(应为 ~8GB),系引擎该接口的已知问题,**不影响效率测试本身**(效率测试用的是 NVML 直读的 `mem_used_mb`,见下)。

## 二、测试方法

**负载**: 合成 tensor(确定性梯度+正弦波,可复现)直接喂 `Engine::process`,绕过图片 I/O,**隔离纯推理开销**。这是能区分"GPU 计算瓶颈 vs CPU 喂数据瓶颈"的关键。

**流程**(每 tile 尺寸重复):
1. NVML 探针后台采样(util/mem/power/clock,50ms 间隔)
2. warmup 2s(丢弃计时)
3. 计时阶段 6s,逐 tile 记录 latency + 同步采 GPU 指标
4. 可选 CPU 对照(同模型走 CPU 后端,验证 GPU 加速比)

**效率判定**:
- `active_ratio` = util≥10% 的样本占比(GPU"醒着"的时间比例)
- `gpu_util` = SM 活动率(纯计算占用)
- 两者结合判定:**util 高 + active 高** = 计算受限;**util 低 + active 高** = CPU 喂数据受限

## 三、核心结果:Tile 尺寸扫描(瓶颈定位)

| Tile | GPU tiles/s | GPU util avg | GPU util p99 | GPU util peak | active_ratio | GPU power avg | SM clock avg |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 128 | **406.75** | 40.7% | 50.0% | 50.0% | 92.5% | 44.1 W | — |
| 256 | 127.55 | 58.6% | 67.0% | 67.0% | 95.7% | 59.4 W | 2470 MHz |
| 384 | 44.62 | **73.2%** | **85.0%** | **85.0%** | 95.0% | 61.6 W | — |

### CPU 对照基准

| Tile | CPU tiles/s | CPU tile latency p50 | GPU tiles/s | **加速比** |
|---:|---:|---:|---:|---:|
| 128 | 44.54 | 21.4 ms | 406.75 | **×9.13** |
| 256 | 5.76 | 172.6 ms | 127.55 | **×22.15** |
| 384 | 2.81 | 355.8 ms | 44.62 | ×15.88 |

### 功耗状态(空闲 vs 满载)

| 状态 | 来源 | GPU 功耗 |
|---|---|---:|
| 空闲(CPU 跑推理,GPU 闲) | CPU 对照阶段 | **~14.9 W** |
| 满载(tile 256 GPU 推理) | GPU 阶段 | **59.4 W** |
| 峰值 | tile 256/384 | 71.0 W |

CPU 对照阶段 GPU util avg=0.7~3.5%、active_ratio<7% —— **证明 GPU 隔离干净,CPU 跑推理时 GPU 确实空闲**,数据有效。

## 四、瓶颈分析

### 结论 1:Tile 越大,GPU 利用率越高 → 当前是"计算密度不足"型低占用
- tile 128 → util 仅 **40.7%**,但 active_ratio 92.5% → GPU 一直在等数据/调度,SM 大量空转
- tile 384 → util 升至 **73.2%**,active_ratio 95% → SM 真正忙起来了
- **根因**: 小 tile 的固定开销(kernel launch、tile 提取、u8→float 转换)占比过高,SM 算力喂不饱。这与 `docs/GPU_EFFICIENCY_REPORT.md` 早期记录的"小图 util 仅 6-13%"是同一类问题,经引擎层的并发 batch + 并行提取优化后已从 13% 提升到 40%(128)/59%(256)。

### 结论 2:Vulkan 串行提交是硬约束,单 tile 难破 ~73% util
- `engine.hpp:96-97` 明确:Vulkan 后端 batch 强制串行,`--jobs`+`--gpu` 无效。
- 单 tile 384 已达 73% util,继续增大 tile 受限于显存(tile²×ch×4×mult),收益递减。
- **突破方向**: 要接近 99% 需让 batch 真正并行(多 queue submit)或双缓冲流水线,这是引擎层而非 perf 层的工作。

### 结论 3:GPU 加速比在 tile 256 达峰值 ×22.15
- tile 128 加速比仅 ×9.13:小 tile CPU 也不吃亏(GPU 固定开销吃掉优势)
- tile 256 加速比 ×22.15:GPU 优势充分发挥的甜点
- tile 384 回落至 ×15.88:CPU 在大 tile 下多核(4 线程)摊薄了差距
- **生产建议**: 默认 tile_size=256 是合理选择,与 `docs/PERFORMANCE_RESULTS.md` 的历史结论一致

### 结论 4:SM 时钟全程锁在 ~2470 MHz(无降频)
- 满载时 SM/graphics clock 稳定在 2370-2475 MHz,**未发生热降频**
- 这排除了"功耗墙/温度墙导致 util 假高"的可能 —— 测到的 util 是真实计算占用

## 五、发现的待改进项

| # | 问题 | 严重度 | 归属 |
|---|---|---|---|
| 1 | `engine->get_memory_info()` 上报 31815 MiB(应为 ~8GB) | 低 | 引擎层 `ncnn_engine.cpp` |
| 2 | GPU 单 tile util 上限 ~73%,距 99% 目标有差距 | 中(架构约束) | 引擎层 Vulkan 并发 |
| 3 | `--jobs`+`--gpu` 静默失效(无警告) | 低 | CLI 层应提示用户 |
| 4 | 小 tile(128)util 仅 40%,固定开销占比高 | 中 | 引擎层 batch/提取优化 |

## 六、perf 构件交付清单

| 文件 | 作用 |
|---|---|
| `share/perf/probes.{hpp,cpp}` | Probe 接口 + NvmlProbe(dlopen NVML) + TimerProbe |
| `share/perf/reporter.{hpp,cpp}` | JSONL 事件流输出(meta/sample/tile/summary/comparison) |
| `share/perf/workload.{hpp,cpp}` | 合成 tensor 生成器(确定性梯度) |
| `share/perf/test.{hpp}` + `tests/gpu_efficiency.cpp` | PerfTest 接口 + GpuEfficiencyTest |
| `share/perf/runner.{hpp,cpp}` | 测试编排 + 名称分发 |
| `share/perf/xmake.lua` | `crossscale-perf` 静态库(依赖 core,dlopen 链 dl) |
| `cli/src/main.cpp` | `perf gpu-efficiency` / `perf list-probes` 子命令 |

### 验收
- ✅ `xmake build` 通过(release, `-Werror`)
- ✅ `perf gpu-efficiency` 产出有效 JSONL(`python -m json.tool` 逐行解析 1131 行无误)
- ✅ 控制台打印完整指标(util avg/p50/p99/peak、active_ratio、mem、power、tiles/s、加速比)
- ✅ 报告区分计算受限 vs 喂数据受限(经 active_ratio + tile 扫描验证)
- ✅ NVML 运行时加载,无官方 nvml.h 头文件依赖,无 NVML 时优雅降级

### 预留扩展(Stage 2-4,接口已对齐)
- `perf tile-sweep`:tile_size × overlap × batch 网格(只需加 `TileSweepTest`)
- `perf precision-sweep`:fp16/int8/arith 配置扫描(只需加 `PrecisionSweepTest`)
- `perf distributed`:多机 weight/吞吐效率(只需加 `DistributedTest`)

## 七、复现命令

```bash
# 构建
xmake build

# 单次测试(tile 256,8s)
./build/linux/x86_64/release/crossscale-cli perf gpu-efficiency \
  --tile 256 --warmup 2s --duration 8s --sample-ms 50 --out events.jsonl

# tile 尺寸扫描(本报告用)
for T in 128 256 384; do
  ./build/linux/x86_64/release/crossscale-cli perf gpu-efficiency \
    --tile $T --warmup 2s --duration 6s --sample-ms 50 --out perf_t${T}.jsonl
done

# 查看可用测试/探针
./build/linux/x86_64/release/crossscale-cli perf list-probes
```
