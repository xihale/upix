# CrossScale Ponytail 审计报告

> 范围：过度工程与复杂性。按"省得最多"排序。所有发现均已交叉验证调用点
> （grep 全仓排除 build/third_party，并人工核实关键项）。
> 正确性 bug、安全漏洞、性能问题不在本审计范围。

---

## 汇总

| # | 目标 | 标签 | 估计省行 |
|---|------|------|---------|
| 1 | `crypto.cpp` 死函数（md5/xor/hash64/base64/random/file_hash 等） | delete | ~150 |
| 2 | CLI `client` 子命令死分支 | delete | ~175 ✅ 已在 `7853738`/`daa6d29` 删除 |
| 3 | `logger` 未用 setter/级别/文件分支 | delete | ~120 |
| 4 | `TileSizeOptimizer` 投机部分（`estimate_tile_size` + `DevicePerformanceProfile` 大半） | yagni | ~100 |
| 5 | `MDNSDiscovery` C++ stub 类 | delete | ~80 ✅ 已在 `8c11549` 删除 |
| 6 | `ModelManager` 死方法（compute_hash/verify_model/import_model/delete_model/infer_metadata） | delete | ~90 |
| 7 | `ncnn_engine.hpp` 死壳头文件 + `Engine::get_memory_info` | delete | ~65 |
| 8 | `expected.hpp` → C++23 `std::expected` | stdlib | ~60 |
| 9 | `config.cpp` 手写 JSON → nlohmann_json（已依赖） | stdlib | ~55 |
| 10 | `ConnectionInfo` 四个序列化方法 + `generate_connection_info` | delete | ~50 |
| 11 | CLI GPU queue env 调参（5 个 env + 动态批逻辑） | yagni | ~95 |
| 12 | cxxopts 死依赖 + 5 段重复手写参数解析 | delete+stdlib | ~60 |
| 13 | `Scheduler` 死方法/字段（`handle_device_failure` 空实现等） | delete | ~25 |
| 14 | `MessageType` 12 个未用枚举值 + 2 个未用 struct | delete | ~25 |
| 15 | `benchmark.cpp` `PerfStats`/`compute_stats` + `BenchmarkResult` 多余字段 | delete | ~30 |
| 16 | `MessageHandler.kt` 死类 + AppContainer 注册 | delete | ~15 |
| 17 | Ktor 3 个依赖声明（零 import） | delete | 3 行 + 大量 transitive |
| 18 | `safe_parse_int/float` 重造 stdlib | stdlib/shrink | ~20 |
| 19 | Navigation ~40 回调透传（提取 HomeCallbacks） | yagni/shrink | ~50 |
| 20 | `JoinInvite.parseLegacyInvite`（无生产来源） | yagni | ~13 |
| 21 | `ProcessingViewModel` 嵌套死函数 `clearCheckpoint` | delete | ~16 |
| 22 | `failureNames*` 三函数合并 | shrink | ~15 |
| 23 | `EnginePool::cv_` 死成员 | delete | 1 |
| 24 | `network_utils::is_port_available` | delete | ~14 |
| 25 | `crc32(vector<uint8_t>)` 重载 | delete | 3 |
| 26 | `worker.cpp` `connect()` 里构造未发出的 `auth_req` | delete | ~5 |

**总计：~1300+ 行源码 + 删 4 个外部依赖**（cxxopts、ktor ×3）。

---

## 第一梯队：零风险纯删（已验证零调用）

这些是 grep 全仓零外部引用、删除无副作用的死代码。建议立即处理。

### 1. `crypto.cpp` 大半是死代码 [delete] ~省 150 行

`share/utils/crypto.cpp`（243 行）。全仓只有 2 个函数被调用：
- `sha256_hex` — 仅 `mdns.cpp:90` 用 1 次
- `generate_uuid` — `coordinator.cpp:199,460` + `main.cpp:1459`

**零调用函数**（已验证）：
- `md5` / `md5_hex`（`crypto.hpp:18-19`）— **声明了但 cpp 根本没实现**，调用即 link error
- `xor_encrypt` / `xor_decrypt`（`crypto.cpp:186-196`）— 0 调用 ✅ 已在 `8c11549` 删除
- `hash64` / `hash32`（`crypto.cpp:198-210`）— 0 调用
- `random_bytes` / `random_hex`（`crypto.cpp:105-122`）— 0 调用
- `base64_encode` / `base64_decode`（`crypto.cpp:127-184`）— 0 调用
- `file_hash`（`crypto.cpp:226-239`）— 0 调用
- `sha256` 两个 public 重载（`crypto.cpp:21-94`）— 仅被 `sha256_hex` 内部用，可内联

**替代：** 无。删完后 crypto.cpp 从 243 行砍到约 90 行。

### 2. `MDNSDiscovery` C++ stub 类 [delete] ~省 80 行 ✅ 已在 `8c11549` 删除

`share/utils/mdns.cpp:205-231` + `mdns.hpp:39-54`。`MDNSDiscovery::start` 只打 `LOG_WARNING("not fully implemented")` 返回成功。**已验证：C++ 侧 0 外部调用**。Android 端用的是 Kotlin `MDNSDiscovery.kt`（基于 NsdManager），不共享此代码。

**替代：** 无。保留 `MDNSBroadcaster`（`main.cpp:1498` 真用）。

> **状态 (2026-07-28)：** 该 C++ stub 已在 commit `8c11549` 删除。当前 `share/utils/mdns.{hpp,cpp}` 仅保留 `MDNSBroadcaster`（PC 服务端广播，`cmd_server.cpp` 真用），Android 侧 `MDNSDiscovery.kt` 独立保留。本项已闭环。

### 3. `ModelManager` 一半方法是死代码 [delete] ~省 90 行

`share/core/model_manager.cpp`（351 行）。已验证零外部调用：

| 方法 | file:line | 状态 |
|------|-----------|------|
| `compute_hash` | `model_manager.cpp:63-91` | 0 调用（`hash*31` 手写 hash） |
| `verify_model` | `model_manager.cpp:93-99` | 0 调用 |
| `infer_metadata` | `model_manager.cpp:58-61` | 0 调用（1 行转发包装） |
| `import_model` | `model_manager.cpp:101-136` | 0 调用 |
| `delete_model` | `model_manager.cpp:138-149` | 0 调用 |
| `set_models_directory` | `model_manager.cpp:13-15` | 0 调用 |

实际在用：`scan_models`、`get_model`、`has_model`、`parse_param_file`。

**替代：** 无。Android 端的 `ModelRepository.kt` 自己实现了导入/删除（含 Zip Slip 防护），不走 C++ 这层。

### 4. `MessageHandler.kt` + AppContainer 注册 [delete] ~省 15 行

`apps/app/src/main/java/com/crossscale/network/MessageHandler.kt`。已验证：除定义（`:5`）和 AppContainer 实例化（`AppContainer.kt:29`）外，`onMessageReceived` 全代码库零调用。

**替代：** 无。删 `MessageHandler.kt` + `AppContainer.kt:15,29`。

### 5. Ktor 3 个依赖声明 [delete] ~省 3 行 + 大量 transitive

`apps/app/build.gradle.kts:150-152`：
```
io.ktor:ktor-client-android:3.4.0
io.ktor:ktor-client-core:3.4.0
io.ktor:ktor-client-cio:3.4.0
```
已验证：`grep "io.ktor\|HttpClient"` 全 `main/` 零命中。网络层是 C++ raw TCP + JNI。

**替代：** 无。APK 体积省最多的一项（ktor 拖一堆传递依赖）。

### 6. `ConnectionInfo` 四个序列化方法 [delete] ~省 50 行

`share/core/distributed/protocol.cpp:68-118` + `coordinator.hpp:35-38`：
- `to_json` / `from_json` / `to_short_string` / `from_short_string` — 全 0 外部调用
- `Coordinator::generate_connection_info`（`coordinator.cpp:95`）— 0 调用

证据：`main.cpp:1540-1549` 自己手拼 JSON 生成 QR，没用这些方法。

**替代：** 无。

### 7. `ncnn_engine.hpp` 死壳头文件 [delete] ~省 65 行

`share/core/ncnn_engine.hpp`（64 行）定义 `class NCNNEngine : public Engine`。已验证：
- 唯一 include 它的是 `ncnn_engine.cpp:1` 自己
- `NCNNEngine` 类名在仓库内零外部引用
- 真正实现是 `ncnn_engine.cpp` 匿名命名空间的 `NCNNEngineImpl`（`:535`），工厂返回 `unique_ptr<Engine>`

**替代：** 无。这个头是重构残留的外壳。

### 8. `MessageType` 12 个未用枚举 + 2 个未用 struct [delete] ~省 25 行

`share/core/distributed/protocol.hpp:9-45` 的 26 个枚举值中，实际被 `.cpp` 引用的只有 14 个。从未引用：
```
MSG_HANDSHAKE_REQUEST/RESPONSE, MSG_HEARTBEAT_ACK,
MSG_DEVICE_LIST/JOIN/LEAVE, MSG_MODEL_SYNC_START/COMPLETE,
MSG_TASK_REJECT, MSG_IMAGE_INFO/COMPLETE, MSG_ERROR
```
`HandshakeRequest` / `HandshakeResponse`（`protocol.hpp:55-66`）两个 struct 也零引用（auth 直接用 `AuthRequest`，handshake 被绕过）。

**替代：** 无。

### 9. `Scheduler` 死方法/字段 [delete] ~省 25 行

`share/core/distributed/scheduler.cpp` + `scheduler.hpp`。已验证零外部调用：

| 成员 | file:line | 状态 |
|------|-----------|------|
| `handle_device_failure` | `scheduler.cpp:113-124` | 0 调用，返回永远空的 vector（孤儿回收是空实现） |
| `set_task_timeout` / `task_timeout_` | `scheduler.hpp:75,82` | 0 调用，默认 60s 从不被读 |
| `unregister_device` | `scheduler.cpp:13-17` | 0 调用 |
| `update_device_performance` | `scheduler.cpp:19-28` | 0 调用 |
| `get_device` | `scheduler.cpp:126-136` | 0 调用 |
| `Task.deadline` 字段 | `scheduler.hpp:39` | 从不读取 |

**替代：** 无。`assign_tiles` 本身合理（加权分发 + max_batch_size 封顶），保留。

### 10. `benchmark.cpp` `PerfStats` / `compute_stats` [delete] ~省 30 行

`share/utils/benchmark.cpp:41-69` + `benchmark.hpp:50-60`。已验证 0 调用。`benchmark()` 函数在用（`tile_optimizer.hpp:88`、`ncnn_engine.cpp:2893`、`main.cpp:1433`），但只读 `result.avg_time_ms` 一个字段。

**替代：** 删 `PerfStats`/`compute_stats`，`BenchmarkResult` 砍到只剩 `avg_time_ms`。

### 11. `network_utils::is_port_available` + `crc32(vector)` 重载 [delete] ~省 17 行

- `network.cpp:287-300`（`is_port_available`，14 行）— 0 调用
- `network.cpp:345`（`crc32(const vector<uint8_t>&)` 重载，3 行）— 0 调用（`crc32(ptr,len)` 在用）

**替代：** 无。

### 12. `logger` 未用配置 [delete/shrink] ~省 120 行

`share/utils/logger.hpp`（209 行）+ `logger.cpp`（空文件）。已验证零调用：
- `set_level`（`:44`）— min_level 永远默认 INFO
- `set_output_file`（`:49`）— `file_` 永远不开
- `set_android_callback`（`:158`）— JNI 侧无调用（Android 用 logcat）
- `LOG_CRITICAL` / `LogLevel::CRITICAL`（`:22,207`）— 0 调用
- 因 `set_output_file` 没人调，`source_location` 文件分支（`:57-155`）永远不走

**替代：** 删 CRITICAL 级别 + 三个 setter + 文件/android 分支，logger.hpp 砍到约 90 行。

### 13. `EnginePool::cv_` 死成员 [delete]

`share/core/engine_pool.hpp:29`：`std::condition_variable cv_` 声明了但从不 `wait`（池空直接 throw）。死成员，误导。

### 14. `worker.cpp` `connect()` 构造未发出的 `auth_req` [delete] ~省 5 行

`worker.cpp:74-78`：在 `connect()` 里构造了 `AuthRequest auth_req` 但没发出去（真正发送在 `handshake()` 里重新构造一遍）。

---

## 第二梯队：需要小重构（替换为标准库/简化）

### 15. `config.cpp` 手写 JSON 解析器 [stdlib] ~省 55 行

`share/core/config.cpp:63-140`。项目**已依赖 nlohmann_json**（`xmake.lua:37`、`protocol.cpp:5,69,83` 在用），但 config.cpp 偏偏手写了 `extract_json_string/int/bool`（48 行 `string::find` + `substr` 扫描）+ 手拼 `<<` 输出。

**替代：** `nlohmann::json::parse(path)` + `j.value("tile_size", 256)`。`load` 从 28 行→8 行，`save` 从 26 行→10 行。

### 16. `expected.hpp` → C++23 `std::expected` [stdlib] ~省 60 行

`share/utils/expected.hpp`（60 行）手写 `expected<T,E>`。CLI 标准是 C++23（`xmake.lua`），有 `std::expected`。

**注意：** JNI 侧是 C++17，不能直接换。建议用 `#if __has_include(<expected>)` 区分，或保留 polyfill 但在 C++23 路径 `using expected = std::expected`。

**附加问题：** 构造函数有歧义风险——`expected(T)` 和 `expected(E)` 在 `T==E` 或都可从字面量构造时重载决议歧义（`expected.hpp:12-13`）。`std::expected` 用 `std::unexpect_t` tag 解决了。

### 17. `safe_parse_int/float` 重造 stdlib [stdlib/shrink] ~省 20 行

`cli/src/main.cpp:50-71`。本质是 `std::stoll/stof` 包一层 catch-LOG-throw，外加 `min_val/max_val` 范围检查。

**替代：** `std::clamp(std::stoll(s), min, max)`；要日志 catch 一次。模板可压成 5 行。

### 18. cxxopts 死依赖 + 5 段重复手写解析 [delete+stdlib] ~省 60 行

- `cxxopts` 在 `xmake.lua:38` + `cli/xmake.lua:5` 声明，但 main.cpp 零 `#include <cxxopts.hpp>`（已验证）
- main.cpp 手写了 5 段几乎一样的 `for(i..argc){ if(arg=="-x") x=argv[++i]; }`（`:349-367,1452-1456,1526-1533,1579-1591,1737-1745`）

**替代：** 二选一——要么删 cxxopts 依赖保持手写，要么真用 cxxopts 把 5 段合成 1 处定义。当前"依赖在、却手写、还重复 5 遍"是最差态。

### 19. `TileSizeOptimizer` 投机部分 [yagni] ~省 100 行

`share/core/tile_optimizer.hpp`（276 行）。`find_optimal_tile_size`（实测 8 尺寸 + 5% 容忍带选最大）是合理优化，**保留**。投机的是：
- `estimate_tile_size`（`:122-145`）— 0 调用，被 `find_optimal_tile_size` 完全绕过
- `DevicePerformanceProfile`（`config.hpp:63-81`）— 除 `available_memory_mb` 外全死：`tile_benchmarks` 永远空、`performance_score` 永远 0、`optimal_tile_size` 永远 256
- `ModelConstraints`（`config.hpp:84-92`）— `memory_multiplier`、`min/max_tile_size` 字段没人填

**替代：** 删 `estimate_tile_size`，`DevicePerformanceProfile` 砍到只剩 `available_memory_mb`。

---

## 第三梯队：投机性配置 / 冗余抽象

### 20. CLI `client` 子命令死分支 [delete] ~省 175 行

`cli/src/main.cpp:1730-1904`。这是"raw TCP 客户端直接提交单图"路径，但产品形态是 Android app 通过 mDNS 发现 PC 后走 JNI（`nativeProcessImageDistributed`）。CLI 侧 `client process` 入口无人用。`MSG_CLIENT_SUBMIT/RESPONSE` 协议字段在 coordinator.cpp 有处理（向后兼容）。

**替代：** 删 CLI 的 `client` 分支。若担心协议兼容，保留 coordinator.cpp 的接收处理。

### 21. CLI GPU queue env 调参 [yagni] ~省 95 行

`cli/src/main.cpp:536-593`（5 个 `CROSSSCALE_GPU_SUBMIT_QUEUE*` env，每个都是 `getenv→stoul→catch→clamp` 样板）+ `:877-900,940-1017` 的动态批深度自适应逻辑。无文档、无默认值以外的预设、用户无从得知这些 env 存在。

**替代：** 删所有 env 读取，硬编码合理默认（queue depth = clamp(hw,2,4)），动态批逻辑退化为固定。

### 22. Navigation ~40 回调透传 [yagni/shrink] ~省 50 行

`apps/app/src/main/java/com/crossscale/ui/navigation/Navigation.kt:36-78` + `CrossScaleApp.kt:236-374`。约 40 个 `onXxx` 回调，绝大多数是 `mainViewModel::xxx` 直接透传给 HomeScreen。

**替代：** 给 HomeScreen 提取 `HomeCallbacks` 数据类，或让 HomeScreen 内部 `viewModel()` 取 VM 绕过透传。需改 HomeScreen 签名，工作量中等。

### 23. `JoinInvite.parseLegacyInvite` [yagni] ~省 13 行

`apps/app/src/main/java/com/crossscale/network/JoinInvite.kt:72-84`。`host:port:key` 三段冒号 legacy 格式无生产来源（CLI `server info` 只产 JSON QR，worker connect 走 `host:port -k`）。

**替代：** 删 `parseLegacyInvite` + `:36` 调用。

### 24. `ProcessingViewModel` 嵌套死函数 + 三函数合并 [delete/shrink] ~省 31 行

- `ProcessingViewModel.kt:799-815`：`discardCheckpoint()` 内嵌套定义 `fun clearCheckpoint(...)` 从未调用（外层直接调 repository 的同名方法）。重构残留，且缩进错位。
- `ProcessingViewModel.kt:1902-1921`：`orderedFailureDetails` / `failureNamesFromOrdered` / `failureNames` 三函数，`failureNames` 内部就是前两者的链式调用。

**替代：** 删嵌套死函数；三函数合一为 `fun failureNames(m) = m.values.sortedBy{...}.map{it.displayName}`。

---

## 明确不动（已核实为真优化 / 真在用）

为避免误删，以下经核实**保留**：

| 项 | 理由 |
|----|------|
| `tile_processor.cpp` 三段流水线（Load/Proc/Save + 双 bounded queue） | 真多线程优化，省显存又压满 GPU |
| `bicubic_scale_u8` + alpha 旁路 | RGBA alpha 走旁路不过神经网络，省 25% GPU 算力，默认 true 真在跑 |
| `extract_tile_u8` | 逐 tile u8→float，省整图 float 拷贝 |
| `Engine::process_tiles_gpu_u8_queue` 三档 GPU pipeline | `main.cpp:988` + `ncnn_engine.cpp:1288` 都在用 |
| `Engine::process_batch` 默认实现 | `tile_processor.cpp:264` 真在调 |
| `Engine` 抽象基类本身 | Android stub 是第二个实现（ncnn 缺失时） |
| `ProcessingRepositoryContracts` 7 个接口 | **每接口都有测试 fake 实现**（`ProcessingViewModelTest.kt:837` 起），真用于测试解耦，非 yagni |
| `crc32(ptr,len)` | protocol checksum 真用 |
| `TCPSocket`/`TCPServer` | coordinator/worker/jni 都在用 |
| `Worker::send_profile`/`receive_task`/`execute_task` | main.cpp worker 循环真用 |
| `Coordinator::distribute_task`/`sync_models_with_worker` | 真用 |
| `generate_qr_ascii` | 单调用方但功能独立、23 行、无抽象层 |
| Android `MDNSDiscovery.kt` | 用平台原生 NsdManager，`DeviceViewModel` 真调 |
| `NativeWorker`/`NCNNEngine.kt` JNI 桥 | thin wrapper，无过度封装 |
| 测试代码 | 真实断言，无占位 |

---

## 建议执行顺序

**第一批（零风险纯删，立即做）**：#1-14 —— 全是 grep 验证过的零调用死代码，删除无副作用。预计省 ~650 行。

**第二批（小重构，标准库替换）**：#15-19 —— config JSON、expected、safe_parse、cxxopts、tile_optimizer 投机部分。预计省 ~295 行。

**第三批（需评估产品意图）**：#20-24 —— client 子命令、GPU env、Navigation 回调、legacy invite、VM 死函数。需确认产品方向后处理。

---

## 修正说明

本报告在 agent 初判基础上做了以下修正（通过 grep + 人工核实）：

1. **`ProcessingRepositoryContracts` 接口层** —— agent 初判为 yagni（单实现），实际每个接口在 `ProcessingViewModelTest.kt` 都有 fake 实现，**是真在用于测试解耦的合理抽象，已从删除清单移除**。
2. **`tile_processor.cpp` 的 `log::info`** —— agent 初判为编译 bug，实际代码是 `LOG_INFO` 宏（正确），**误判已撤销**。
3. **`ncnn_engine.hpp`** —— agent 说零 include，实际被 `ncnn_engine.cpp:1` 自己 include；但 `NCNNEngine` 类名零外部引用，仍是死壳，结论不变。
