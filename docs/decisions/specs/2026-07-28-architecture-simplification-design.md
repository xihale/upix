# CrossScale 架构简化设计

> **范围**：全平台激进重设（PC CLI + share 共享核心 + Android Kotlin）。
> 允许破坏向后兼容；保留核心能力（单机超分、分布式集群、serve 守护进程）。
>
> **策略**：分层降复杂度（"剥洋葱"），从外向内、风险递增。每层独立可交付、
> 可验证、可回滚。复用已验证的 serve daemon 简化模式（spec → A/B md5 对比 → 落地）。
>
> **产出形态**：本文件是总纲设计。落地时按层拆成独立的实施 plan，逐个执行。
>
> **基线**：HEAD `d5c8605`，含未提交的 CLI 重构（main.cpp → CLI11 + 9 个 cmd_*.cpp）。

---

## 0. 动机与目标

### 0.1 问题陈述

CrossScale 经过多轮功能迭代（Android app、分布式、GPU 优化、serve daemon、perf 框架），
累积了可观的架构复杂度：

- **巨型文件**：`ncnn_engine.cpp` 3892 行（shader/CPU/GPU/batch/三档流水线全堆一起）、
  `coordinator.cpp` 1131 行、`ProcessingViewModel.kt` 2141 行、`cmd_process.cpp` 1211 行。
- **死代码与投机抽象**：`PONYTAIL_AUDIT.md` 列出 26 项 ~1300 行零调用代码，
  至今未执行。
- **构建漂移**：`share/` 源文件在 `share/xmake.lua` 和 `apps/.../CMakeLists.txt`
  两处手写枚举，新增 `.cpp` 需手动同步。
- **伪实现**（✅ 已在 `8c11549` 解决）：~~`MDNSDiscovery` C++ stub（只打 warning）、`crypto::xor_encrypt`（形同虚设）~~。当前 C++ 仅保留 `MDNSBroadcaster`，crypto 仅保留 `sha256_hex`/`generate_uuid`。
- **自造轮子**：`expected.hpp` 手写 Result（C++23 有 `std::expected`）、
  `config.cpp` 手写 JSON 扫描（已依赖 glaze）。
- **死依赖**：cxxopts（CLI 已改用 CLI11）、Ktor×3（零 import）。

### 0.2 目标

1. **降低代码量**：净减 ~1980 行 + 删 4 个外部依赖。
2. **拆解巨型文件**：4 个大文件 → ~15 个中等文件，每个单一职责、可独立理解。
3. **收敛抽象**：自造轮子换标准库，删投机性调参旋钮。
4. **消除伪实现**：删 stub、删形同虚设的"安全"代码。
5. **统一构建**：源文件自动收集，消除两处手写枚举漂移。

### 0.3 非目标

- **不改核心算法行为**：超分输出必须 bit-identical（A/B md5 验证）。
  tile 流水线、alpha 旁路、GPU 三档能力等经 audit 确认的真优化，保留。
- **不换协议格式**：15B 帧头 + CRC32 轻量无依赖、传输无关（TCP/UnixSocket 复用）、
  跨平台一致。换 HTTP/gRPC 引入的依赖与 Android 兼容问题得不偿失。
- **不合并 serve daemon 与 coordinator**：两者职责正交（见 §3.5 评估）。

---

## 1. 总体策略：分层降复杂度

四层，风险递增。每层独立价值，可随时停在任意层。

```
§1 外壳清理    │ 删 26 项死代码/死依赖/死枚举（~1420行+4依赖）
(零风险)        │ 统一两处源文件枚举 → glob 自动收集
                │ CLI 重构收尾提交
─────────────────────────────────────────────────────────
§2 大文件拆分  │ ncnn_engine.cpp 3892行 → 5 文件
(中风险,A/B验证)│ coordinator.cpp 1131行 → 3 文件
                │ ProcessingViewModel.kt 2141行 → 5 文件
                │ cmd_process.cpp 1211行 → 3 文件
                │ 纯结构重组，不改逻辑
─────────────────────────────────────────────────────────
§3 协议/角色精简│ 删 CLI client 子命令(185行) + mDNS C++ stub(80行) + xor_encrypt  ✅ 全部已执行
(中风险,产品决策)│ serve/coordinator 保持独立（评估结论，见 §4.5）
                │ 协议格式不动
─────────────────────────────────────────────────────────
§4 抽象收敛    │ expected.hpp→std::expected, config手写JSON→glaze
(低风险)        │ 死配置字段清理、引擎初始化序列去重
                │ 砍 GPU 调参 env（5个 CROSSSCALE_GPU_SUBMIT_QUEUE*）
```

| 层 | 净减行 | 删依赖 | 风险 | 可独立交付 |
|---|---|---|---|---|
| §1 | ~240 | cxxopts + ktor×3 | 零 | ✅ |
| §2 | 0（重组） | 0 | 中 | ✅ |
| §3 | ~280 | 0 | 中 | ✅ |
| §4 | ~150 | 0 | 低 | ✅ |
| **合计** | **~670** | **4** | | |

> 注：§1/§4 原估算（~1700行）基于过时的 `PONYTAIL_AUDIT.md`，经 2026-07-28 实测核查后
> 大幅下调——多数死代码、`expected.hpp`、`config.cpp` 手写 JSON 等早已清理。
> 本设计的**主体价值在第2层（大文件拆分）**，而非行数削减。

---

## 2. 第1层：外壳清理（零风险纯删）

> **⚠️ 重要修正（2026-07-28 核查）**：原设计引用的 `PONYTAIL_AUDIT.md` 基于旧版本代码，
> 经逐项核查当前 HEAD `d5c8605`，audit 列出的多数死代码**早已删除**：
> - `crypto.cpp` 已瘦身至 3 个函数（全在用），无 md5/xor/hash64/base64/random/file_hash。
> - `ModelManager` 接口已无 compute_hash/verify/import/delete 等方法。
> - `ncnn_engine.hpp` 死壳已删。
> - `expected.hpp` 已是 `std::expected` 别名（13 行）。
> - `config.cpp` 已用 glaze（非手写 JSON）。
> - `MessageHandler.kt`、`JoinInvite.parseLegacyInvite` 已删。
> - `MessageType` 死枚举从 12 个降到 1 个（`MSG_IMAGE_TILE`）。
> - audit 标注的 `set_models_directory`、`Scheduler::unregister_device`、
>   `Engine::get_memory_info` **实际都在用，绝不能删**（audit 事实错误）。
>
> 因此第1层范围大幅缩减为下面**已实测确认**的少量残留项。

### 2.1 C++ 残留死代码删除（~90 行）

| 删除目标 | 位置 | 行数 | 验证依据（HEAD `d5c8605` 实测） |
|---|---|---|---|
| `MSG_IMAGE_TILE`(0x0401) 死枚举 | `protocol.hpp:27` | 1 | grep 全仓 0 使用（`MSG_TILES_BATCH` 在用） |
| `ConnectionInfo` 整个 struct + glz::meta + 4 方法 | `coordinator.hpp:29-58` + `protocol.cpp:65-101` | ~50 | 整个 struct 0 外部使用，4 方法仅定义无调用，glz::meta 也无人直接用 |
| `logger.hpp` set_level/set_output_file/set_android_callback + CRITICAL + 文件分支 | `logger.hpp` | ~120 | 0 调用（grep 实测） |
| `PerfStats`/`compute_stats` + BenchmarkResult 多余字段 | `benchmark.{hpp,cpp}` | ~30 | 0 调用（仅 avg_time_ms 在用） |

### 2.2 死依赖删除

| 依赖 | 位置 | 影响 |
|---|---|---|
| `cxxopts` | `xmake.lua` + `cli/xmake.lua` | CLI 已用 CLI11，零引用（待复核 cli/xmake.lua 是否仍声明） |
| Ktor ×3（ktor-client-android/core/cio） | `apps/app/build.gradle.kts` | 零 import（待复核是否仍在依赖列表） |

### 2.3 构建系统统一（消除两处手写枚举漂移）

**问题**：`share/` 的 `.cpp` 在 `share/xmake.lua`（PC）和
`apps/app/src/main/cpp/CMakeLists.txt`（Android）两处手写枚举。

**方案**：自动收集。

- **PC (xmake.lua)**：`os.files("core/**/*.cpp", "utils/**/*.cpp")` 自动 glob，
  配合平台门控（`unix_socket.cpp` 仅 linux/macosx；`android_utils.cpp` 仅 android）。
- **Android (CMakeLists.txt)**：`file(GLOB ...)` 收集 share 源文件。

**已知约束**：`file(GLOB)` 新增/删除文件后需重新 configure（可接受）。

### 2.4 第1层验收标准

- `cd cli && xmake` 通过，`crossscale process`/`serve`/`server`/`worker` 行为不变。
- `cd apps && ./gradlew assembleDebug` 通过。
- 对一组测试图，超分输出 md5 与重构前一致。
- cxxopts/ktor 从依赖列表消失；share 源文件用 glob 收集。

---

## 3. 第2层：大文件拆分（纯结构重组）

4 个巨型文件按职责拆成 ~15 个中等文件。**不改逻辑**，每个拆分单元 A/B md5 验证。

### 3.1 `ncnn_engine.cpp`（3892 行）→ 5 文件

按职责边界拆。`NCNNEngineImpl` 仍是 PIMPL，成员不变，方法体分散到不同文件
（C++ 允许成员函数定义跨文件）。

```
share/core/
├─ ncnn_engine.hpp              PIMPL 外壳 + create_ncnn_engine 工厂
│                               （若 §2.1 已删死壳则此为唯一 hpp）
├─ ncnn_engine.cpp              ~600行：NCNNEngineImpl 类骨架
│                               initialize/load_model/benchmark/get_recommended_tile_size
│                               get_memory_info，持有所有子模块，委托调用
├─ vulkan_shaders.hpp           ~450行：所有 GLSL compute shader 常量（header-only）
│                               kHwcToChwComp/kHwcU8ToChwComp/kHwcCropToChwComp
│                               kHwcCropU8ToChwComp/kChwToHwcComp/kChwToHwcU8Comp
│                               kHwcFloatToU8Comp/kClearComp/kBlendTileComp
│                               → 纯数据，零依赖，最易理解
├─ ncnn_gpu_init.cpp            ~350行：Vulkan 初始化 + GPU 枚举打分
│                               try_init_vulkan/create_gpu_instance
│                               score_gpu（discrete/integrated/fp16/queue）
│                               重试逻辑（CROSSSCALE_GPU_INIT_RETRIES）
│                               → "选哪块GPU、怎么初始化" 单一职责
├─ ncnn_cpu_path.cpp            ~550行：process() + process_batch() CPU 分支
│                               HWC→CHW→extractor→CHW→HWC
│                               batch：clone→extract 逐个，std::async 并发
│                               → 不依赖 Vulkan 的纯 CPU 路径
└─ ncnn_gpu_pipeline.cpp        ~1200行：三档 GPU 流水线
                                process_tiles_gpu / _u8 / _u8_queue
                                整图上传→crop→推理→量化→拼回
                                → 最复杂但自成体系
```

**拆分原则**：每个文件单一职责、可独立阅读；shader 常量提到 header-only
被 gpu_pipeline 和 gpu_init 共用；不改任何逻辑，纯 `mv` 代码块 + 加 `#include`。

### 3.2 `coordinator.cpp`（1131 行）→ 3 文件

```
share/core/distributed/
├─ coordinator.hpp              保留：Coordinator 类声明
├─ coordinator.cpp              ~350行：start/accept_loop/handle_client 路由
├─ coordinator_task.cpp         ~500行：distribute_task 多波循环
│                               assign→send→collect→stitch 的核心编排
└─ coordinator_admin.cpp        ~280行：认证 + worker 管理 + 模型同步
                                handle_auth/register_device/sync_models
```

### 3.3 `ProcessingViewModel.kt`（2141 行）→ 5 文件

按关注点拆。共享状态（当前队列、当前任务）提到 `ProcessingState` 数据类，
各 Coordinator 持有引用。

```
viewmodel/
├─ ProcessingViewModel.kt       ~600行：UI 状态暴露 + 委托给下面协调器
├─ BatchQueueCoordinator.kt     ~450行：队列去重 + 顺序执行
├─ CheckpointCoordinator.kt     ~400行：断点续传 + 持久化
├─ RetryCoordinator.kt          ~350行：failed-only 重试逻辑
└─ BackupCoordinator.kt         ~300行：备份/恢复
```

### 3.4 `cmd_process.cpp`（1211 行）→ 3 文件

```
cli/src/
├─ cmd_process.cpp              ~200行：入口 + 单图委派/内联路径
├─ process_batch_cpu.cpp        ~400行：目录模式 CPU EnginePool 多线程
└─ process_batch_gpu.cpp        ~600行：目录模式 GPU 三级流水线
```

### 3.5 第2层验收标准

- 每个拆分完成后，对同一组测试图跑 `process`，输出 md5 与拆分前一致。
- `cd cli && xmake` 与 `cd apps && ./gradlew assembleDebug` 均通过。
- 拆出的每个文件可独立阅读理解（同行评审：能否不看其他文件就理解这个文件的职责）。

---

## 4. 第3层：协议/角色精简（产品决策）

消除分布式子系统的设计冗余。允许破坏兼容。

### 4.1 CLI `client` 子命令删除（185 行）

`cmd_client.cpp` 是"PC 端 raw TCP 客户端直接提交单图"路径。产品形态是 Android app
通过 JNI 发起（`nativeProcessImageDistributed`），CLI client 是重复实现且无人用。

**删除范围**：
- `cli/src/cmd_client.cpp` 整文件
- `cli/src/cli_app.cpp` 中 client 子命令注册
- `cli/src/args.hpp` 中 `Client` struct
- `cli/src/cli_app.hpp` 中 `cmd_client` 声明
- `share/core/distributed/coordinator.cpp` 中 `MSG_CLIENT_SUBMIT` 的 `handle_client`
  接收处理（Android 的分布式提交是独立 JNI 实现，不走 coordinator 的 handle_client；
  执行时复核此假设）

### 4.2 mDNS 统一：删 C++ stub

- **删** `share/utils/mdns.cpp:205-231` + `mdns.hpp:39-54` 的 `MDNSDiscovery` C++ stub
  （~80 行，只打 warning）。
- **保留** `MDNSBroadcaster`（PC 服务端广播，`main.cpp:1498` 真用）。
- **保留** Android `MDNSDiscovery.kt`（平台原生 NsdManager，`DeviceViewModel` 真调）。
- **结果**：PC 只广播，Android 只发现，职责清晰，删掉自欺欺人的 C++ stub。

> **Status (2026-07-28):** the C++ `MDNSDiscovery` stub was already removed in commit `8c11549`. Only `MDNSBroadcaster` (kept, as intended) and the Android `MDNSDiscovery.kt` remain. This section is now doc-only; no further code action.

### 4.3 `crypto::xor_encrypt` 删除

明文 key 认证已是现状，`xor_encrypt`/`xor_decrypt`（`crypto.cpp:186-196`）是伪安全，
0 调用。删除。（注：§2.1 的 crypto 死函数清理已涵盖此项，此处单独登记强调。）

> **Status (2026-07-28):** already executed in commit `8c11549`. `crypto.{hpp,cpp}` now expose only `sha256_hex` and `generate_uuid`. Doc-only reconciliation in this layer.

### 4.4 协议格式不动

15B 帧头 + CRC32 保持。理由见 §0.3 非目标。仅做：
- 复核 `MSG_IMAGE_TILE`(0x0401) 是否死枚举（audit 标注可能遗留），是则删。
- 在 `protocol.hpp` 补一份线格式集中说明注释。

### 4.5 serve daemon ↔ coordinator 合并评估：不合并

**评估结论**：两者职责正交，合并会耦合不相关关注点。

| 维度 | serve daemon | coordinator |
|---|---|---|
| 传输 | UnixSocket（本地） | TCP（局域网） |
| 状态 | 无状态，1连接=1请求=1响应 | 有状态，worker 注册/心跳/任务循环 |
| 角色 | 单机加速（复用热引擎） | 分布式调度（多设备协同） |
| 是否持引擎 | ✅ 是（核心目的） | ❌ 否 |
| 用户 | `crossscale process` 本地调用 | Android app / worker 连入 |

**不合并的三条理由**：
1. 职责正交——serve 解决"单图固定开销"，coordinator 解决"多设备任务分配"。
   合并会让一个类同时管本地无状态 RPC + 分布式有状态调度，职责膨胀。
2. 合并的诱惑点是"都该持引擎"，但 coordinator 持引擎会引入 engine_mutex 串行
   （serve 已有的复杂度），且本地请求与分布式任务会争抢同一引擎——更复杂。
3. 两者真正共享的是协议帧格式（已复用 `Protocol::serialize`）和 `TileProcessor`
   （都用 `process_image_u8`），已共享。合并实体无增量收益。

**真问题另解**：两者各有"引擎初始化 + 模型加载"样板代码，抽 `EngineSetup` 自由函数
复用（见 §5.3）。

### 4.6 第3层验收标准

- CLI 不再有 `client` 子命令（`crossscale --help` 不显示）。
- `crossscale server start` + Android worker 连入 + 分布式超分仍正常工作。
- `crossscale serve start/stop` + `crossscale process` 仍正常工作。
- PC 端不再编译 `MDNSDiscovery`（grep 确认无引用）。

---

## 5. 第4层：抽象收敛（去重 + 砍投机旋钮）

> **⚠️ 重要修正（2026-07-28 核查）**：原设计的两项主要工作已完成：
> - `expected.hpp` 已是 `std::expected` 别名（13 行），无需替换。
> - `config.cpp` 已用 glaze（`glz::read`/`glz::write_json`），无需替换。
>
> 第4层范围缩减为下面已实测确认的残留项。

### 5.1 引擎初始化序列去重

serve daemon `start()` 和 worker `connect()` 各有一套"create_ncnn_engine → initialize →
scan_models → load_model"序列。抽自由函数复用（具体省行数待实现时实测）。

> **✅ 已执行（Layer 4 §5.1，commit `19327f8`）**：抽出 `setup_engine()` 自由函数
> （`share/core/engine_setup.{hpp,cpp}`）。
> **范围修正**：原 spec 只列 serve + worker 两站，实测核查后发现：
> - **采用 helper 的站点（2）**：`serve/daemon.cpp` start()、`cli/cmd_process.cpp` 单文件
>   路径——这两站执行完整的 create→init→scan→select→load 五步序列。
> - **未采用 helper 的站点（7）**：
>   - `cmd_worker.cpp`：多 worker 分布式角色已删（commit `e026fcb`），文件不再存在。
>   - `coordinator.cpp ensure_engine_ready()`：create+init（GPU→CPU 回退）+ 模型经
>     `get_model` 懒加载，不经 scan，模式不同，helper 不适用。
>   - `process_batch.cpp` 两处（jobs≤1 + async）：caller 已预选模型，helper 会重复 scan。
>   - `engine_pool.cpp`：构造器直接收 `param_path`/`bin_path`，不经 scan。
>   - `jni_bridge.cpp` 三处：NDK 构建未在本层 A/B 验证范围内，留待 Android 侧后续采用。
>   - perf 测试 4 处：测试脚手架，保持现状。

### 5.2 GPU 调参环境变量收敛

CLI 有 5 个 `CROSSSCALE_GPU_SUBMIT_QUEUE*` env（每个 getenv→stoul→catch→clamp 样板，
在 `cmd_process.cpp` 内），无文档、用户无从得知。

**决策**：删所有 env 读取，硬编码合理默认（queue depth = `clamp(hw_concurrency, 2, 4)`），
动态批逻辑退化为固定。省 ~95 行（待实测确认）。

> **✅ 已执行（Layer 4 §5.2，commit `fa95bba` + `0cd15d7`）**：实测删除 9 个 env vars
> （6 个 queue env + 3 个 pipeline env `_SUBMIT_TILES_PER_TASK`/`_SUBMIT_MB`/`_SUBMIT_BUDGET`）
> + ~65 行动态批逻辑（wait window / prep_fill 预估 / tiles-target cap / 两个累积器）
> + ~84 行 budget 算术，共 ~180 行；`submit_budget` 恒为 1（blend RAW/WAR hazard）。
> 保留下来的 escape hatches（`CROSSSCALE_GPU_DISABLE_FP16` 等 5 个驱动 bug 变量、
> 4 个 tile-override 变量、`CROSSSCALE_PROGRESS`）不受影响。

### 5.3 第4层验收标准

- `EngineSetup` 抽出后，serve 与 worker 的初始化路径行为不变。
  ✅ serve `start()` 已采用 helper（错误码 2/3/4/5/6 折叠为 2，唯一调用方只判 `!= 0`）；
  worker 路径已随多 worker 角色删除，无对应初始化。
- 删 GPU env 后，目录模式批量处理性能不退化（perf 框架 benchmark 对比）。
  ✅ 实测 `process --gpu` 单图 1024x512→2048x1024 成功；2 图目录批量 2/2 成功，
  queue 日志 `GPU submit queue enabled, depth=3`，无回退告警。

---

## 6. 执行顺序与依赖

四层有弱依赖，但每层可独立交付。推荐顺序：

```
§1 外壳清理 ──┐
              ├─→ §4 抽象收敛（expected/config/EngineSetup 依赖 §1 的死代码清理）
§2 大文件拆分 ┘
              ↓
        §3 协议/角色精简（独立，可任意时机做）
```

- **§1 必须先做**：死代码清理是后续所有层的认知基础（拆分时不该拆到死代码）。
- **§2 紧跟 §1**：拆分时顺便确认每块都是活代码。
- **§3 独立**：产品决策项，可任意时机。
- **§4 在 §1/§2 后**：expected/config 替换需先确认死代码已清。

每层完成后单独提交、单独验证，不混合。

---

## 7. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 拆分引入回归（行为变更） | 每个拆分单元 A/B md5 对比（复用 serve daemon 验证模式） |
| 删 client 子命令破坏 Android 分布式 | 执行前复核"Android 不走 coordinator handle_client"假设；保留 coordinator 的 `MSG_CLIENT_SUBMIT` 接收直到确认 |
| `file(GLOB)` 漏文件 | 构建后用 `xmake -v` / CMake 构建日志复核编译单元清单 |
| `std::expected` 与 JNI C++17 冲突 | `#if __has_include(<expected>)` 区分，或 polyfill 仅 Android |
| 删 GPU env 影响性能 | 删除前后用 perf 框架 benchmark 对比，确认无退化 |
| Kotlin 拆分破坏 ViewModel 状态机 | 拆分后跑 `./gradlew testDebugUnitTest`，含 ProcessingViewModelTest |

---

## 8. 不动清单（经 audit 确认的真优化/真在用）

以下明确保留，避免误删：

| 项 | 理由 |
|---|---|
| `tile_processor.cpp` 三段流水线（Load/Proc/Save + 双 bounded queue） | 真多线程优化，省显存又压满 GPU |
| `bicubic_scale_u8` + alpha 旁路 | RGBA alpha 走旁路不过神经网络，省 25% GPU 算力 |
| `extract_tile_u8` | 逐 tile u8→float，省整图 float 拷贝 |
| `Engine::process_tiles_gpu_u8_queue` 三档 GPU pipeline | 真在用 |
| `Engine::process_batch` 默认实现 | 真在调 |
| `Engine` 抽象基类本身 | Android stub 是第二个实现 |
| `ProcessingRepositoryContracts` 7 接口 | 每接口都有测试 fake 实现，真解耦 |
| 协议帧格式（15B 头 + CRC32） | 轻量无依赖、传输无关 |
| `crc32(ptr,len)` / `TCPSocket`/`TCPServer` | 真在用 |
| `Worker::send_profile`/`receive_task`/`execute_task` | 真在用 |
| `Coordinator::distribute_task`/`sync_models_with_worker` | 真在用 |
| serve daemon 整体（热引擎加速、懒换模型、静默回退） | 单图 2-15× 加速，已验证 |
| Android `MDNSDiscovery.kt`（NsdManager）/ `NativeWorker`/`NCNNEngine.kt` JNI 桥 | 真在用 |

---

## 9. 相关文档索引

| 文档 | 内容 |
|---|---|
| `docs/PONYTAIL_AUDIT.md` | 26 项过度工程审计（§1 的执行清单来源） |
| `docs/ARCHITECTURE.md` | 当前架构全貌（已合并原 OVERVIEW/TECHNICAL_REPORT） |
| `docs/SERVE_DAEMON_DESIGN.md` | serve daemon 设计（§3.5 评估依据） |
| `docs/DATA_FLOW.md` | 关键调用链（拆分验证参考） |
| `docs/decisions/specs/2026-07-23-serve-daemon-simplification-design.md` | serve daemon 简化的成功先例（本设计的验证模式来源） |

---

## 附：落地形态

本文件是总纲。下一步按层拆成独立的实施 plan（经 writing-plans 技能）：

- `plans/2026-07-28-layer1-cleanup.md`（外壳清理，零风险，先做）
- `plans/2026-07-28-layer2-file-splitting.md`（大文件拆分，A/B 验证）
- `plans/2026-07-28-layer3-protocol-role.md`（协议/角色精简，产品决策）
- `plans/2026-07-28-layer4-abstraction.md`（抽象收敛）

每个 plan 独立执行、独立验证、独立合并。
