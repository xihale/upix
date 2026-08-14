# CrossScale Ponytail 审计报告

> 范围：过度工程与复杂性。按"省得最多"排序。
> **状态核对日期**：2026-08-02（已逐项 grep 全仓 + 人工核实当前代码状态）。

本审计最初列出 26 项（均为"可删/可简化"候选）。经架构简化（Layer 1-4）后，**绝大多数已闭环**。本文档已重组为三块：已闭环、待办、明确保留（后者为独立的"经核实保留"清单，不占 26 项名额）。

---

## 汇总（当前状态）

| 状态 | 项数 | 说明 |
|------|------|------|
| ✅ 已闭环 | 22 | 死代码已删 / 已替换为标准库 |
| ⏳ 待办 | 4 | 仍存在于代码，需评估后处理 |

---

## ✅ 已闭环（21 项）

| # | 目标 | 原标签 | 当前状态 |
|---|------|--------|---------|
| 1 | `crypto.cpp` 死函数（md5/xor/hash64/base64/random/file_hash） | delete | ✅ crypto.cpp 243→120 行；crypto.hpp 仅剩 `sha256_hex`+`generate_uuid` 两个真用函数 |
| 2 | CLI `client` 子命令死分支 | delete | ✅ `cmd_client.cpp` 已删除（commit `7853738`/`daa6d29`） |
| 3 | `logger` 未用 setter/级别/文件分支 | delete | ✅ `set_level`/`set_output_file`/`set_android_callback`/`CRITICAL` 全 0 命中 |
| 5 | `MDNSDiscovery` C++ stub 类 | delete | ✅ 已删（commit `8c11549`），仅保留 `MDNSBroadcaster` |
| 6 | `ModelManager` 死方法（compute_hash/verify_model/infer_metadata/import_model/delete_model） | delete | ✅ 全 0 命中；仅保留 `scan_models`/`get_model`/`has_model`/`parse_param_file` |
| 7 | `ncnn_engine.hpp` 死壳头文件 | delete | ✅ 文件已删，0 include |
| 8 | `expected.hpp` → C++23 `std::expected` | stdlib | ✅ 已迁移为 `using expected = std::expected`（13 行 thin alias），全仓 274 处用法 |
| 9 | `config.cpp` 手写 JSON → JSON 库 | stdlib | ✅ 已迁移到 `glaze/glaze.hpp`（手写 `extract_json_*` 全删） |
| 10 | `ConnectionInfo` 四个序列化方法 | delete | ✅ protocol.cpp 中 0 命中 |
| 12 | cxxopts 死依赖 + 重复手写解析 | delete+stdlib | ✅ xmake.lua 中 0 命中，依赖已删 |
| 13 | `Scheduler` 死方法/字段 | delete | ✅ `handle_device_failure`/`set_task_timeout`/`update_device_performance`/`get_device`/`Task.deadline` 全 0 命中；仅保留 `unregister_device` |
| 14 | `MessageType` 未用枚举 + struct | delete | ✅ protocol.hpp 从 26 个枚举精简到 16 个，均带语义注释 |
| 15 | `benchmark.cpp` `PerfStats`/`compute_stats` | delete | ✅ 0 命中；benchmark.hpp 砍到 45 行，仅留 `avg_time_ms` |
| 16 | `MessageHandler.kt` + AppContainer 注册 | delete | ✅ 0 命中 |
| 17 | Ktor 3 个依赖声明 | delete | ✅ build.gradle.kts 0 命中 |
| 18 | `safe_parse_int/float` 重造 stdlib | stdlib/shrink | ✅ 0 命中 |
| 20 | `JoinInvite.parseLegacyInvite` | yagni | ✅ 0 命中 |
| 23 | `EnginePool::cv_` 死成员 | delete | ✅ 0 命中（仅剩 `mutex_`） |
| 24 | `network_utils::is_port_available` | delete | ✅ 0 命中 |
| 25 | `crc32(vector<uint8_t>)` 重载 | delete | ✅ 0 命中 |

---

## ⏳ 待办（3 项）

### A. CLI GPU queue env 调参 [yagni] ~省 95 行

**当前状态**：✅ 已在 Layer 4 §5.2 删除。6 个 `CROSSSCALE_GPU_SUBMIT_QUEUE*` / `_MIN_SUBMIT_BATCH` / `_BATCH_WAIT_MS` env 读取（`getenv→stoul→catch→clamp` 样板）已删（commit `fa95bba`），动态批大小 + batch-fill wait + `last_avg_tiles_per_image`/`batch_tiles_total` 累积器已折叠为固定深度（commit `0cd15d7`）；`ncnn_gpu_pipeline.cpp` 侧 3 个 `CROSSSCALE_GPU_SUBMIT_*` env + ~84 行 budget 算术同批删除，`submit_budget` 恒为 1。

### B. `TileSizeOptimizer` 投机部分 [yagni] ~省 100 行

**当前状态**：`share/core/tile_optimizer.hpp:131` 的 `estimate_tile_size` 仍在（0 调用，被 `find_optimal_tile_size` 完全绕过）。`config.hpp` 的 `DevicePerformanceProfile`/`ModelConstraints` 投机字段仍在。

**建议**：删 `estimate_tile_size`，`DevicePerformanceProfile` 砍到只剩 `available_memory_mb`。保留 `find_optimal_tile_size`（实测选优，合理）。

### C. `worker.cpp` `connect()` 构造未发出的 `auth_req` [delete] ~省 5 行

**当前状态**：仍在 `share/core/distributed/worker.cpp:74-75`。`connect()` 里构造 `AuthRequest auth_req` 但没发出去（真正发送在 `handshake()` `:99-105` 里重新构造一遍）。

**建议**：删 `connect()` 里的死构造。

### D. Navigation ~40 回调透传 [yagni/shrink] ~省 50 行

**当前状态**：仍在 `apps/app/src/main/java/com/crossscale/ui/navigation/Navigation.kt:36-77`。约 42 个 `onXxx` 回调，绝大多数是 `mainViewModel::xxx` 直接透传给 HomeScreen。

**建议**：给 HomeScreen 提取 `HomeCallbacks` 数据类，或让 HomeScreen 内部 `viewModel()` 取 VM 绕过透传。需改 HomeScreen 签名，工作量中等。

### E. `ProcessingViewModel` failureNames 三函数合并 [shrink] ~省 15 行

**当前状态**：`ProcessingViewModel.kt:1750,1758` 的 `orderedFailureDetails` / `failureNamesFromOrdered` 仍在（被多处调用，功能正确，但可合并）。原审计 #21 的嵌套死函数 `clearCheckpoint` 已删（✅），仅剩此项合并优化。

**建议**：三函数合一为 `fun failureNames(m) = m.values.sortedBy{...}.map{it.displayName}`。

---

## ⛔ 明确保留（经核实为真优化 / 真在用）

为避免误删，以下经核实**保留**：

| 项 | 理由 |
|----|------|
| `tile_processor.cpp` 三段流水线（Load/Proc/Save + 双 bounded queue） | 真多线程优化，省显存又压满 GPU |
| `bicubic_scale_u8` + alpha 旁路 | RGBA alpha 走旁路不过神经网络，省 25% GPU 算力，默认 true 真在跑 |
| `extract_tile_u8` | 逐 tile u8→float，省整图 float 拷贝 |
| `Engine::process_tiles_gpu_u8_queue` 三档 GPU pipeline | 真在用 |
| `Engine::process_batch` 默认实现 | `tile_processor.cpp` 真在调 |
| `Engine` 抽象基类本身 | Android stub 是第二个实现（ncnn 缺失时） |
| `ProcessingRepositoryContracts` 7 个接口 | **每接口都有测试 fake 实现**，真用于测试解耦，非 yagni |
| `crc32(ptr,len)` | protocol checksum 真用 |
| `TCPSocket`/`TCPServer` | coordinator/worker/jni 都在用 |
| `Worker::send_profile`/`receive_task`/`execute_task` | worker 循环真用 |
| `Coordinator::distribute_task`/`sync_models_with_worker` | 真用 |
| `generate_qr_ascii` | 单调用方但功能独立、23 行、无抽象层 |
| Android `MDNSDiscovery.kt` | 用平台原生 NsdManager，`DeviceViewModel` 真调 |
| `NativeWorker`/`NCNNEngine.kt` JNI 桥 | thin wrapper，无过度封装 |
| 测试代码 | 真实断言，无占位 |

---

## 修正说明（原始审计的人工核实修正）

1. **`ProcessingRepositoryContracts` 接口层** —— 初判为 yagni（单实现），实际每个接口在 `ProcessingViewModelTest.kt` 都有 fake 实现，**是真在用于测试解耦的合理抽象，已从删除清单移除**。
2. **`tile_processor.cpp` 的 `log::info`** —— 初判为编译 bug，实际代码是 `LOG_INFO` 宏（正确），**误判已撤销**。
3. **`config.cpp` JSON 库** —— 原审计建议 nlohmann_json，实际最终选用 `glaze`（同样达成"用库替换手写"目标，且更轻量、编译期反射）。
