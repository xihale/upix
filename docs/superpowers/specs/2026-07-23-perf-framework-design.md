# 性能测试框架(perf)设计文档

- **日期**: 2026-07-23
- **阶段**: Stage 1 — GPU 效率/占用率测试(聚焦 PC 独显)
- **状态**: 设计已确认,直接实现

## 1. 目标与非目标

### 目标
- 在 `share/perf/` 建立模块化的性能测试库,探针(Probe)+ 测试(Test)组合模式
- Stage 1 交付 `GpuEfficiencyTest`:测 GPU 利用率/显存/功耗/时钟,识别"GPU 计算瓶颈 vs CPU 预处理瓶颈"
- 提供 CPU 对照基准(GPU vs CPU 加速比)
- 输出 JSONL 事件流供二次分析

### 非目标(预留扩展,不在本阶段实现)
- Tile 尺寸/重叠网格扫描(`perf tile-sweep`)
- 精度配置开关影响(`perf precision-sweep`)
- 分布式多机效率(`perf distributed`)
- 以上三个的接口契约在本设计中已预留,Stage 1 完成后只需各加一个 Test 类

## 2. 关键决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 形态 | C++ 进程内模块 + CLI `perf` 顶级子命令 | 能量到 ncnn 内部瓶颈 |
| 架构 | Probe + Test 组合 | 新指标只写探针,新场景只写 Test |
| GPU 指标源 | NVML via `dlopen`(运行时加载) | 无官方 nvml.h,零新包依赖,无 NVML 时优雅降级 |
| 负载 | 合成 tensor 喂 `Engine::process` | 绕过 I/O,聚焦推理瓶颈 |
| 输出 | JSONL 事件流 + 控制台摘要 | 可二次聚合,人读友好 |
| 位置 | `share/perf/` 独立静态库 `crossscale-perf` | 不污染 core/utils 生产代码 |

## 3. 环境基线(实测)
- GPU: NVIDIA GeForce RTX 4060 Laptop, 8188 MiB, compute 8.9, driver 595.71.05
- `libnvidia-ml.so` 存在,无官方 `nvml.h` → 采用 `dlopen` 方案
- 模型: `Real-ESRGANv3-anime-x2`(`models/Real-ESRGANv3-anime/x2.{param,bin}`)
- 构建: xmake v3.0.9, C++23, release 开 `-Werror`
- GPU 并发约束: Vulkan 后端强制串行提交(`engine.hpp:96-97`),`--jobs`+`--gpu` 无效

## 4. 架构

### 4.1 目录布局
```
share/perf/
├── xmake.lua              # target("crossscale-perf"), add_deps("crossscale-core")
├── probes.hpp/.cpp        # Probe 基类 + NvmlProbe + TimerProbe
├── reporter.hpp/.cpp      # JSONL 事件流
├── workload.hpp/.cpp      # 合成 tensor 生成
├── runner.hpp/.cpp        # 编排:装载探针→跑 Test→汇总
└── tests/
    └── gpu_efficiency.hpp/.cpp
```

### 4.2 Probe 接口
```cpp
// 探针:在 Test 运行期间后台采样,产出样本
class Probe {
public:
    virtual ~Probe() = default;
    virtual auto name() const -> std::string = 0;
    virtual auto start() -> bool = 0;        // 返回 false 表示不可用(如无 NVML)
    virtual auto sample() -> Sample = 0;     // 同步采样一次
    virtual void stop() = 0;
};
```
- `NvmlProbe`: dlopen NVML,采 gpu_util%、mem_used/total MB、power W、clocks MHz(sm/graphics)
- `TimerProbe`: 高精度计时(per-tile elapsed)

### 4.3 Test 接口
```cpp
class PerfTest {
public:
    virtual ~PerfTest() = default;
    virtual auto name() const -> std::string = 0;
    virtual auto run(const TestConfig& cfg, Reporter& rep) -> int = 0;  // 返回码
};
```
Stage 1 实现 `GpuEfficiencyTest`;`TileSweepTest`/`PrecisionSweepTest`/`DistributedTest` 留同名接口待扩展。

### 4.4 数据流
```
Runner ──装载──> Probe[]
  │                 │ (后台线程定时 sample)
  ├─ start probes ──┤
  ├─ warmup(workload, duration)
  ├─ for each iteration:
  │     TimerProbe.tic
  │     Engine::process(synthetic_tensor)
  │     TimerProbe.toc → emit "tile" event
  ├─ stop probes ──> flush samples → "sample" events
  └─ emit "summary" event (util avg/p50/p99, mem, power, tiles/s, GPU vs CPU)
```

### 4.5 JSONL 事件 schema
每行一个 JSON 对象,`type` 字段区分:
- `{"type":"meta", ...}` 环境信息(GPU名、驱动、compute_cap、模型、tile配置)
- `{"type":"sample", "t_ms":..., "gpu_util":..., "mem_used_mb":..., "power_w":..., "sm_clock":..., "device":"gpu"|"cpu"}`
- `{"type":"tile", "i":N, "elapsed_ms":...}`
- `{"type":"summary", "device":..., "iterations":..., "tiles_per_s":..., "ms_p50":..., "ms_p99":..., "gpu_util_avg":..., "gpu_util_p50":..., "gpu_util_p99":..., "gpu_util_peak":..., "active_ratio":..., "mem_used_avg_mb":..., "power_avg_w":..., "power_peak_w":...}`
- `{"type":"comparison", "gpu_tiles_per_s":..., "cpu_tiles_per_s":..., "speedup":...}`(仅当启用 CPU 基准)

## 5. CLI
```
crossscale-cli perf gpu-efficiency [--model <name>] [--tile 256] [--batch 1]
                                   [--duration 5s] [--warmup 1s]
                                   [--no-cpu-baseline] [--out events.jsonl]
                                   [--sample-ms 100] [--models-dir ./models]
crossscale-cli perf list-probes
```
在 `main.cpp` 已知子命令列表加 `"perf"`,在 benchmark stub 后新增 `if (command == "perf")` 分支。

## 6. 验收标准
1. `xmake` 构建通过(release,`-Werror`)
2. `crossscale-cli perf gpu-efficiency` 产出有效 JSONL(可被 `python -m json.tool` 逐行解析)
3. 控制台打印:GPU util avg/p50/p99/peak、active_ratio、mem、power、tiles/s、GPU vs CPU speedup
4. 报告能区分"GPU 计算受限"vs"CPU 喂数据受限"(通过 active_ratio 与 tile elapsed 对比)

## 7. 风险与缓解
- **NVML dlopen 失败**:Probe.start() 返回 false,Test 在无 GPU 样本时仍跑计时并标注 `gpu_unavailable`
- **fp16 在 Laptop GPU 的 NaN 风险**:本测沿用引擎默认配置,不改 fp16 开关(那是 precision-sweep 的职责)
- **release `-Werror`**:所有未用参数加 `(void)`,无 unused warning
