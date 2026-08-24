# 架构决策记录（ADR）索引

本目录归档 CrossScale 的历史架构决策——设计文档（specs/）与实施计划（plans/）。
这些是**已执行或已评估的决策记录**，非当前任务。当前架构请看 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。

---

## 架构简化总纲（2026-07-28）

| 文档 | 内容 |
|------|------|
| [specs/2026-07-28-architecture-simplification-design.md](specs/2026-07-28-architecture-simplification-design.md) | 四层简化总纲（"剥洋葱"）：Layer 1 外壳清理 → Layer 2 文件拆分 → Layer 3 协议/角色裁剪 → Layer 4 抽象合并 |

### 四层执行状态

| 层 | Plan | 状态 | 关键产出 |
|----|------|------|---------|
| Layer 1 | [plans/2026-07-28-layer1-cleanup.md](plans/2026-07-28-layer1-cleanup.md) | ✅ 已执行 | 死代码删除（`ConnectionInfo`/`MSG_IMAGE_TILE`）；`share/xmake.lua` 与 Android `CMakeLists.txt` 改为 glob 自动收集（消除双向漂移） |
| Layer 2 | [plans/2026-07-28-layer2-file-splitting.md](plans/2026-07-28-layer2-file-splitting.md) | ✅ 已执行 | 4 个巨型文件拆分：`ncnn_engine.cpp` 3892→3 文件、`coordinator.cpp`→3 文件、`ProcessingViewModel.kt` 拆分、`cmd_process.cpp`→8 个 `cmd_*.cpp`。A/B md5 验证零逻辑变更 |
| Layer 3 | [plans/2026-07-28-layer3-protocol-role-pruning.md](plans/2026-07-28-layer3-protocol-role-pruning.md) | ✅ 已执行 | 删除 CLI `client` 子命令（`cmd_client.cpp`）；订正仍描述已删 stub（`xor_encrypt`/C++ `MDNSDiscovery`）的过时文档 |
| Layer 4 | [plans/2026-07-28-layer4-abstraction-consolidation.md](plans/2026-07-28-layer4-abstraction-consolidation.md) | ⏳ 部分执行 | §5.1（`expected.hpp`→`std::expected`）与 §5.2（`config.cpp`→glaze）在 plan 编写时已发现是 no-op（早已完成）。**两个真实 Task 未执行**：① `engine_setup` 去重（9 处 create→init→scan→load 序列未合并）；② 9 个死 GPU env 未删（待办 A，已在 Layer 4 §5.2 闭环） |

---

## 其他决策

| 文档 | 日期 | 内容 | 状态 |
|------|------|------|------|
| [specs/2026-07-23-perf-framework-design.md](specs/2026-07-23-perf-framework-design.md) | 2026-07-23 | `share/perf/` 性能测试框架设计（Probe+Test 组合模式，GPU 效率探针） | ✅ 已实现，产出见 [`../perf/`](../perf/) 报告 |
| [specs/2026-07-23-serve-daemon-simplification-design.md](specs/2026-07-23-serve-daemon-simplification-design.md) | 2026-07-23 | `crossscale serve` 守护进程简化设计（4→2 MessageType，手工帧→`Protocol::serialize`） | ✅ 已实现，设计说明见 [`../SERVE_DAEMON_DESIGN.md`](../SERVE_DAEMON_DESIGN.md) |
| [plans/2026-07-23-serve-daemon-simplification.md](plans/2026-07-23-serve-daemon-simplification.md) | 2026-07-23 | serve daemon 简化的实施计划 | ✅ 已执行 |

---

## 说明

- **specs/** 是设计文档（"做什么、为什么"），**plans/** 是实施计划（"怎么做、分几步"）。一对 spec+plan 对应一次架构变更。
- 这些文档记录的是**决策当时的代码状态**，部分行号/文件名已随重构失效。当前代码以 [`../ARCHITECTURE.md`](../ARCHITECTURE.md) 为准。
- 未执行的待办项（Layer 4 的两个 Task）已迁移到 [TECH_DEBT.md](../TECH_DEBT.md)，避免散落。
