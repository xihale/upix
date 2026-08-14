# CrossScale 架构

> 跨平台分布式图像超分辨率系统。核心推理与分布式协议用 C++ 共享于 `share/`，
> 对外暴露两条入口——PC 端 CLI（`cli/`）与 Android 应用（`apps/`），通过局域网
> TCP + 自定义二进制协议把多设备组成超分集群，按各端实测吞吐权重分配 tile 任务。
>
> 本文档是架构的**单一权威来源**。代码规模/结构以 HEAD `7baf52c` 为准。

---

## 0. 顶层布局

```
                  ┌──────────── PC CLI (cli/, C++23) ────────────┐
                  │  process / server / worker / serve /          │
                  │  model / benchmark / perf / completion        │
                  └───────────────┬──────────────────────────────┘
                                  │ 链接静态库
                  ┌───────────────▼──────────────────────────────┐
                  │          share/ (static lib, C++)            │
                  │  core/     推理引擎 + tile + 分布式调度 + serve│
                  │  utils/    网络 / 图像 / 日志 / 加密           │
                  │  perf/     模块化性能测试框架                  │
                  └───────────────┬──────────────────────────────┘
                                  │ 同一份源码直接编进 .so
                  ┌───────────────▼──────────────────────────────┐
                  │      Android App (apps/, Kotlin + JNI)        │
                  │  Compose UI (MVVM) ← JNI 桥 → share/ 代码     │
                  └──────────────────────────────────────────────┘
```

| 目录 | 角色 | 构建 | 标准 |
|------|------|------|------|
| `share/` | 共享 C++ 核心：推理引擎、tile 处理、分布式协议、serve 守护进程、网络/工具、perf 框架 | xmake 静态库（PC）/ CMake 直接编译（Android） | C++23 (PC), C++17 (JNI) |
| `cli/` | PC 命令行入口，已模块化（`main.cpp` 仅 39 行薄入口 + `cli_app.cpp` 命令树 + 9 个 `cmd_*.cpp`） | xmake → `crossscale-cli` | C++23 |
| `apps/` | Android 应用（Jetpack Compose + MVVM） | Gradle + CMake/NDK | Kotlin + C++17 JNI |
| `models/` | ncnn 模型仓库（`.param` + `.bin`） | 运行时扫描 | — |
| `docs/`、`scripts/` | 文档与运维脚本 | — | — |

**核心设计原则：源码级共享。** `share/` 不是预编译库给 Android 用，而是其 `.cpp` 文件被 Android 的 CMake **glob 自动收集**编译进 `libcrossscale-native.so`，与 PC 端代码完全一致，保证两端协议、调度、引擎逻辑强一致。唯一平台差异是 ncnn 引擎可插拔（见 §2.7）。

**构建已统一**：`share/xmake.lua`（PC）与 `apps/app/src/main/cpp/CMakeLists.txt`（Android）均用 glob 自动收集 share 源文件，新增 `.cpp` 无需手动同步两处列表。

---

## 1. 三大子系统

| 子系统 | 位置 | 职责 |
|--------|------|------|
| **推理引擎** | `share/core/`（非 distributed 部分） | 引擎抽象、ncnn 后端（CPU/Vulkan）、tile 切分与融合、tile 大小自动优化、模型管理、引擎池 |
| **分布式计算** | `share/core/distributed/` + `share/utils/{network,mdns,crypto}` | 二进制协议、coordinator/worker 角色、权重调度、TCP 传输、mDNS 发现 |
| **前端** | `cli/`（PC）+ `apps/app/`（Android） | 用户交互、命令路由、生命周期、UI |

---

## 2. 推理引擎子系统（`share/core/`）

### 2.1 引擎抽象 —— `engine.hpp`

`Engine`（`share/core/engine.hpp`）是纯虚接口，定义推理后端契约：`initialize` / `list_devices` / `load_model` / `process`（单 tile float）/ `process_u8` / `process_batch`（GPU 强制串行，CPU 用 `std::async` 并发）/ GPU tile 流水线三档 / `benchmark` / `get_recommended_tile_size` / `get_memory_info`。

工厂 `create_ncnn_engine()` 经间接层转发到 `create_ncnn_engine_impl()`——这个间接层是为 Android 端可替换 stub（§2.7）。

### 2.2 ncnn 后端（已按职责拆分）

原 3892 行的 `ncnn_engine.cpp` 已按职责拆为 5 个文件：

| 文件 | 行数 | 职责 |
|------|------|------|
| `ncnn_engine_impl.hpp` | 128 | `NCNNEngineImpl` 类声明（PIMPL 外壳 + 工厂） |
| `ncnn_engine.cpp` | 1000 | 类骨架：initialize/load_model/process/process_u8/process_batch/benchmark + CPU 路径 |
| `vulkan_shaders.hpp` | 529 | 11 个 GLSL compute shader 常量（header-only） |
| `ncnn_gpu_init.cpp` | 197 | Vulkan 初始化 + GPU 枚举打分（discrete/integrated/fp16/queue）+ 重试 |
| `ncnn_gpu_pipeline.cpp` | 2239 | 三档 GPU 流水线：process_tiles_gpu / _u8 / _u8_queue + SPIRV init |

**关键实现细节**：
- GPU 优先初始化，Vulkan 失败回退 CPU；GPU 实例创建时枚举所有 GPU 打分选最优
- HWC↔CHW 转换全部在 GPU 上用自写 Vulkan compute shader 完成，避免 CPU↔GPU 往返
- fp16 默认开启；驱动 bug（Adreno/Mali NaN、全零输出）时用 `CROSSSCALE_GPU_DISABLE_FP16=1` 关闭保精度
- 全局互斥锁串行化 Vulkan 实例/模型加载（ncnn 的 Vulkan 状态是进程级单例）
- GPU tile 流水线最高档 `process_tiles_gpu_u8_queue`：整图 u8 上传一次，在 GPU 上 crop + 推理 + 量化 + 拼回

### 2.3 Tile 处理 —— `tile_processor.{hpp,cpp}`

`TileProcessor`：**大图切块 → 逐块推理 → 加权融合拼回**。
- `compute_tile_layout`：按 `step = tile_size - overlap` 网格化，边界自动 clamp
- **三段流水线**（Load 线程切块提取 → 主线程 Proc 调 `engine.process_batch` → Save 线程融合），用两个有界队列 + `condition_variable` 同步，让 CPU 提取/融合与 GPU 推理重叠
- GPU 快速路径：若引擎支持，整图直接交给引擎的 GPU 流水线
- `blend_tile`：重叠区二次衰减权重（`w*w`），最后归一化，消除接缝
- `extract_tile_u8`：逐 tile 转 float，避免整图 float 拷贝，省 O(W·H·C) 内存
- `bicubic_scale_u8`：Mitchell-Netravali 双三次插值，RGBA 的 alpha 通道走旁路不过神经网络，省 25% GPU 算力

### 2.4 Tile 大小优化器 —— `tile_optimizer.hpp`（header-only）

`TileSizeOptimizer::find_optimal_tile_size` 自动选最优 tile：候选 {64,128,192,256,320,384,448,512} → 对每个尺寸实测 tiles/sec + 估算内存 → 淘汰超 80% 可用内存的 → 在"最小耗时 ×1.05 容忍阈值"内挑最大 tile（提高 GPU 利用率）。

### 2.5 引擎池 —— `engine_pool.{hpp,cpp}`

`EnginePool`：预创建 N 个独立 Engine 实例 + 队列复用，让多线程并发推理而不串扰 Vulkan 上下文（每线程需独占一个 Engine + 自己的 VkCompute）。

### 2.6 模型管理 —— `model_manager.{hpp,cpp}`

`ModelManager`：扫描 `models/` 目录，解析 `.param` 文件推断元数据。放大倍数从文件名推断（`x2`→2、`x3`→3）；输入通道数从首个卷积层反推。

### 2.7 Android 可插拔引擎 —— `ncnn_engine_stub_android.cpp`

Android 端通过 CMake 开关 `CROSSSCALE_USE_NCNN` 决定：ON 编译真实 `share/core/ncnn_engine.cpp`（需 jniLibs 有 `libncnn.a`）；OFF 编译 stub（只做最近邻上采样，让代码能编通走链路）。

### 2.8 配置 —— `config.{hpp,cpp}`

`ProcessingConfig` 聚合 tile/batch/内存/线程/GPU/模型/输出等参数，用 glaze 做 JSON 持久化（`glz::read`/`glz::write_json`）和校验。

### 2.9 引擎子系统调用关系

```
上层调用方
   ├─ ModelManager.scan_models() → ModelInfo
   └─ TileProcessor.process_image[_u8](engine, config)
          ├─ [GPU 快速路径] engine.process_tiles_gpu_u8_queue()
          └─ [通用路径] 三段流水线 → engine.process_batch() → blend_tile
                          └─ 多线程时 EnginePool.acquire/release

auto_tile_size=true 时：
   上层 → TileSizeOptimizer.find_optimal_tile_size(engine, model, config)
            └─ engine.process() 多次 benchmark
```

---

## 3. 分布式计算子系统（`share/core/distributed/`）

### 3.1 二进制协议 —— `protocol.{hpp,cpp}`

**定长 15 字节小端帧头**：

| 字段 | 字节 | 说明 |
|------|------|------|
| magic | 4 | `0x53534358` ("XCSS")，校验用 |
| version | 1 | |
| type | 2 | `MessageType` |
| payload_length | 4 | |
| checksum | 4 | CRC32（`network_utils::crc32`） |

**消息类型**（`MessageType`）按功能分组：握手认证（0x00）、设备管理（0x01）、模型同步（0x02）、任务分发（0x03）、图像分块（0x04）、客户端卸载（0x05）、本地守护进程（0x06，Unix Socket）。

**认证**：明文 key 比对（`AuthRequest.auth_key[64]`），无链路加密。key 默认随机生成或用户 `-k` 指定。配对短串 `ip:port#key` 即 QR 码内容。

### 3.2 三种角色

**Coordinator**（`coordinator.{hpp,cpp}`，服务端，已拆 3 文件）：监听 TCP，认证 worker，把客户端提交的图切块，按调度器指派分发，收集结果拼图。`coordinator.cpp`（骨架+路由）/ `coordinator_task.cpp`（distribute_task 多波循环）/ `coordinator_admin.cpp`（认证+模型同步）。

**Worker**（`worker.{hpp,cpp}`，工作端）：连到 coordinator，认证，**握手时本地 benchmark 测速并上报**，接收 tile 任务、用 Engine 推理、回传结果，周期心跳（5s）。

**Client**：无独立 CLI 子命令。分布式提交由 Android app 通过 JNI 发起（`nativeProcessImageDistributed` 用 `crossscale::TCPSocket` + `Protocol`）。原 CLI `client` 子命令（一次性 raw TCP 提交）已删除。

### 3.3 权重调度 —— `scheduler.{hpp,cpp}`

`Scheduler` 维护在线设备表，按各设备 `tiles_per_second` 归一化为权重：`weight[i] = tps[i] / Σ tps[i]`（否则 1/N 均分）。

**权重数据来源**：worker 在 `handshake` 时调 `engine.benchmark(256)`——对 256×256 哨兵 tile 跑 5 轮取均时，返回 `tiles_per_second = 1000/avg_ms`——经 `DeviceProfileMessage` 上报给 coordinator，coordinator `register_device` 时归一化。

**任务分配** `assign_tiles()`：筛 alive（30s 内有心跳）→ 按权重降序排 → 每设备配额 = `ceil(total × weight)`，受 `max_batch_size` 封顶。失败重调度靠 coordinator 的多波循环——失败的 tile 下一波重分。

### 3.4 网络与发现 —— `share/utils/`

| 模块 | 职责 |
|------|------|
| `network.{hpp,cpp}` | POSIX socket 薄封装（`TCPSocket`/`TCPServer`）+ `crc32` + 本地 IP/auth-key 生成 |
| `mdns.{hpp,cpp}` | 局域网服务发现。`MDNSBroadcaster` 用 avahi-publish-service 发 `_crossscale._tcp`（PC 服务端广播） |
| `crypto.{hpp,cpp}` | `sha256_hex` / `generate_uuid`，用于 task_id/device_id 生成与 mDNS key hash |
| `unix_socket.{hpp,cpp}` | Unix Domain Socket（serve daemon 用，Linux/macOS） |

> **mDNS 职责分离**：PC 端只广播（`MDNSBroadcaster`），发现侧由 Android `MDNSDiscovery.kt`（原生 NsdManager）承担。原 C++ `MDNSDiscovery` stub 已删除。

### 3.5 分布式调用链

```
启动认证：
  CLI server start → Coordinator.start() → TCPServer.listen()
  Worker.connect() → send MSG_AUTH_REQUEST → 收 device_id
        → engine.benchmark(256) 测速 → send MSG_DEVICE_PROFILE
        → Coordinator → Scheduler.register_device() → normalize_weights()

任务执行（Android app 发起）：
  Android → nativeProcessImageDistributed (JNI) → MSG_AUTH_REQUEST → MSG_CLIENT_SUBMIT(整图)
  → Coordinator.distribute_task
    → 切 tile 数 → 每波循环:
      Scheduler.assign_tiles(pending) ── 按权重切配额
      → 每个 worker: send TASK_ASSIGNMENT + TILES_BATCH
        ↓ worker 侧
        Worker.execute_task() → engine.process() 逐 tile 推理 → send TASK_RESULT
        ↑ coordinator 收齐
      collect_results() → 刷新心跳
    → stitch_tiles() 拼回大图 → MSG_CLIENT_RESPONSE → 写文件
```

---

## 4. serve 守护进程（`share/core/serve/`）

`crossscale serve` 是**本地长驻守护进程**，持有一份已初始化+已加载模型的 ncnn 引擎（"热引擎"），通过 Unix domain socket 接受 `crossscale process` 单图请求并回传超分结果。目的是**把单图调用的 ~490ms 固定开销（Vulkan init + model load）摊薄到零**，单图从 ~0.85s 降到 ~0.12s（~4×，小图最高 ~15×）。无守护进程时 CLI 行为零变化（静默回退内联路径）。

**为什么与 coordinator 独立**：两者职责正交。serve 解决"单图固定开销"（无状态，1连接=1请求，持引擎，UnixSocket 本地）；coordinator 解决"多设备任务分配"（有状态，worker 注册/心跳/任务循环，不持引擎，TCP 局域网）。合并会让一个类同时管本地无状态 RPC + 分布式有状态调度，职责膨胀。两者真正共享的是协议帧格式（已复用 `Protocol::serialize`）和 `TileProcessor`（都用 `process_image_u8`）。

完整设计见 `docs/decisions/serve-daemon-design.md`。

---

## 5. 前端子系统

### 5.1 PC CLI（`cli/`，已模块化）

**子命令**：`process` / `model list` / `benchmark` / `perf {list-probes|gpu-efficiency|gpu-concurrency|gpu-reuse}` / `server {start|info}` / `worker connect` / `serve {start|stop}` / `completion`。

**结构**：
- `main.cpp`（39 行）：薄入口，parse argv 并 dispatch
- `cli_app.cpp`（210 行）：CLI11 命令树（所有子命令的选项/校验/回调）
- `args.hpp`：参数 struct
- `cmd_*.cpp`（9 个）：每个子命令的业务逻辑
- `process_batch.cpp`：目录模式批处理（CPU EnginePool / GPU 流水线）

**单机 process 调用链**：
```
main → cmd_process → ModelManager.scan_models() 选模型
     → create_ncnn_engine() → initialize(GPU 优先) → load_model()
     → [auto-tile] TileSizeOptimizer.find_optimal_tile_size()
     → Image::load → normalize_channels_for_model
     → TileProcessor.process_image_u8(→ Engine 推理)
     → Image::save(quality, format)
```

### 5.2 Android 应用（`apps/app/`）

**架构：Jetpack Compose + MVVM + 单 Activity + 手写 DI**。

```
com/crossscale/
├── CrossScaleApplication.kt   持有 AppContainer
├── MainActivity.kt            唯一 Activity
├── CrossScaleApp.kt           顶层 Composable + NavHost 宿主
├── AppContainer.kt            手工 DI（实例化所有 repository）
├── ui/        (screens, components, navigation, theme) ← Compose + Material 3
├── viewmodel/ (Main / Device / Processing)             ← StateFlow + combine
├── data/      (model, repository)                      ← 持久化
├── network/   (Coordinator / MDNS / JoinInvite)        ← 发现+封装
└── native/    (NCNNEngine.kt, NativeWorker.kt)         ← JNI 桥
```

**ViewModel 层**（3 个，全部 `StateFlow` + `WhileSubscribed`）：`MainViewModel`（模型/输入/输出/开关）、`DeviceViewModel`（mDNS 发现/QR 连接/自动重连）、`ProcessingViewModel`（批处理引擎，含队列去重、checkpoint 断点续传、failed-only 重试、备份恢复）。已从 ProcessingViewModel 抽出 `ProcessingMappers` / `BackupRestoreManager` / `InputCountEstimator` / `ProcessingUiModels`。

**关键事实**：**Android 端的真正网络通信在 C++ JNI 层完成**（`jni_bridge.cpp` 的 `nativeProcessImageDistributed` 用 `crossscale::TCPSocket` + `Protocol`）。Kotlin `network/` 包只做发现（`MDNSDiscovery` 用 Android NsdManager）和封装（`CoordinatorClient` 调 JNI）。

**JNI 桥**（`apps/app/src/main/cpp/jni_bridge.cpp`）暴露两类 native 方法：`NCNNEngine`（单例 object，本地推理 + 分布式）、`NativeWorker`（实例类，长驻 worker 会话）。

**Android 本地超分调用链**：
```
UI → ProcessingViewModel → NativeImageProcessingRepository
   → NCNNEngine.nativeProcessImage (JNI)
   → jni_bridge.cpp → share/core 的 Engine + TileProcessor
```

---

## 6. 技术栈与依赖

| 层 | 技术 |
|----|------|
| 推理 | ncnn（Vulkan GPU 加速），自写 GLSL compute shader |
| 图像 | libpng / libjpeg / libwebp / stb_image |
| 构建（PC） | xmake，C++23 |
| 构建（Android） | Gradle + CMake/NDK，C++17 JNI |
| Android UI | Jetpack Compose + Material 3 + navigation-compose |
| CLI 解析 | CLI11 |
| 序列化 | glaze（config JSON） |
| 二维码 | qr-code-generator-cpp（PC）/ CameraX + ML Kit（Android 扫码） |
| 协议 | 自定义二进制（15B 帧头 + CRC32） |
| 服务发现 | mDNS（PC 端 avahi-publish；Android NsdManager） |
| 安全 | EncryptedSharedPreferences（Android auth key）；明文 key 认证（协议层） |

**依赖来源**：PC 端 `xmake require` 拉取 ncnn/libwebp/libpng/libjpeg/glaze/qr-code-generator-cpp；Android 端 `apps/scripts/sync_native_deps.sh` 用 `xrepo install` 把 ncnn/glslang/libomp/zlib/libpng/libjpeg/libwebp 编进 `jniLibs/`（构建产物，不入 git）。

---

## 7. 架构特点

### 优点
1. **源码级跨平台复用**：share/ 一份代码，PC 和 Android 都直接编译，协议/调度/引擎逻辑强一致。
2. **引擎抽象 + 可插拔后端**：ncnn 缺失时 Android 仍能编译运行（stub），降低构建门槛。
3. **GPU 优化深入**：自写 Vulkan shader 做 HWC↔CHW、整图 u8 上传一次的 queue 流水线、alpha 旁路、batch 合并、互斥锁串行化单例状态。
4. **分布式权重设计合理**：握手 benchmark 得 tps → 归一化 → 按比例切配额 → 多波重调度，简单有效。
5. **Android 健壮性投入大**：checkpoint 断点续传、队列持久化、原子写、Zip Slip 防护、payload 上限、双探针输出校验。
6. **serve daemon 复用热引擎**：单图 2-15× 加速，已验证。

### 已知限制 / 技术债
1. **`Scheduler::handle_device_failure` 孤儿回收是空实现**，靠 coordinator 多波循环兜底，单波内部分失败可能延迟到下一波才重试。
2. **协议层无链路加密**，明文 key 认证，仅适合可信局域网。
3. **serve daemon 平台限制**：AF_UNIX 仅 Linux/macOS；推理强制串行（Vulkan 单例硬约束），高并发批量场景应用目录模式 `--jobs`；256MB 图像上限。
4. **mDNS 发现侧职责分离**：PC 只广播，Android 只发现，不共享发现代码（设计选择，非缺陷）。
5. **`ProcessingViewModel` 仍 ~1865 行**：已抽出 4 个协作类，队列/checkpoint/retry 三者通过共享状态强耦合，进一步拆分收益有限。

---

## 8. 关键调用链速查

**单机超分（PC CLI 单图，daemon 不在）**：
```
main → cmd_process → ModelManager.scan_models
     → create_ncnn_engine → Engine.initialize → Engine.load_model
     → [auto] TileSizeOptimizer.find_optimal_tile_size
     → Image::load → normalize_channels_for_model
     → TileProcessor.process_image_u8 → Engine.process[_batch] → blend_tile
     → Image::save
```

**单机超分（PC CLI 单图，daemon 在）**：
```
main → cmd_process → [serve daemon 探测] → 连 Unix socket
     → send [ServeRequest][图] → daemon: process_image_u8 → recv [ServeResponse][结果]
     → Image::save
```

**分布式超分（Android → Coordinator → Workers）**：
```
Android → nativeProcessImageDistributed (JNI) → MSG_AUTH_REQUEST → MSG_CLIENT_SUBMIT(整图)
  → Coordinator.distribute_task
    → Scheduler.assign_tiles (按 tps 权重)
    → 各 Worker.execute_task → Engine.process(逐 tile) → MSG_TASK_RESULT
    → collect_results → stitch_tiles
  → MSG_CLIENT_RESPONSE → 写文件
```

**Android 加入集群**：
```
DeviceViewModel → MDNSDiscovery (NsdManager 发现) 或 QR 扫描 → JoinInvite 解析
  → CoordinatorClient.connect → NativeWorker.nativeConnect (JNI)
  → jni_bridge.cpp AndroidWorkerSession → share/core Worker (handshake+benchmark+心跳+任务循环)
```

---

## 9. 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档导航入口 |
| `docs/decisions/serve-daemon-design.md` | serve 守护进程完整设计 |
| `docs/decisions/architecture-simplification.md` | 2026-07 架构简化决策（4 层） |
| `docs/decisions/ponytail-audit.md` | 全仓库过度工程审计（含执行状态） |
| `docs/perf/` | 性能测试报告 |
| `AGENTS.md` | 构建命令、代码风格、仓库结构（开发指南） |
| `README.md` | 用户向使用说明 |
