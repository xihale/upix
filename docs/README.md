# CrossScale 文档导航

> 本目录是 CrossScale 所有文档的入口。按用途分组，每份文档标注定位。

---

## 架构与设计（必读）

| 文档 | 定位 | 何时看 |
|------|------|--------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | **架构单一权威来源**：顶层布局、模块职责、代码规模、构建系统、数据流总览 | 想了解系统怎么组织的，先看这个 |
| [DATA_FLOW.md](DATA_FLOW.md) | 关键操作的数据流路径、组件调用链、线程模型 | 想理解单机/分布式/serve 的执行路径与并发模型 |
| [SERVE_DAEMON_DESIGN.md](SERVE_DAEMON_DESIGN.md) | `crossscale serve` 热引擎守护进程的设计（Unix socket 复用引擎，单图 2-15× 加速） | 改 serve 子命令或理解其简化决策 |

---

## 技术债

| 文档 | 定位 | 何时看 |
|------|------|--------|
| [TECH_DEBT.md](TECH_DEBT.md) | 经核实的简化待办（4 项，按收益排序） | 想找点低风险清理活干的时候 |

## 性能报告（历史数据快照）

[perf/](perf/) 下归档了 6 份性能测试报告，按测试日期与场景索引。**入口**：[perf/README.md](perf/README.md)。

核心结论：单图 GPU≈CPU（固定开销占 60%）；批量复用引擎时 GPU 快 CPU 4×；fp16 是核心速度来源；tile_size=256 是多数图的甜点。

---

## 架构决策记录（历史）

[decisions/](decisions/) 下归档了 8 份设计文档（specs/）与实施计划（plans/），记录已执行/已评估的架构变更。**入口**：[decisions/README.md](decisions/README.md)。

最重要的一份是 [四层架构简化总纲](decisions/specs/2026-07-28-architecture-simplification-design.md)：Layer 1-3 已执行，Layer 4 部分待办（已迁入 TECH_DEBT.md）。
