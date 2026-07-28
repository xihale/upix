# CrossScale 架构说明与技术报告

> **用途**：本报告供人工审阅，是当前代码库（截至 2026-07-23，HEAD=`d1d8a68`）的全局架构与技术状态综述。
> 它整合并更新了既有 `docs/ARCHITECTURE.md`（2026-07-04）与 `docs/SERVE_DAEMON_DESIGN.md`（2026-07-23），
> 并纳入最新性能数据。审阅者可直接据此评估架构合理性、风险与技术债。
>
> **代码规模**：`share/` 共享 C++ 约 12.8K 行（30 cpp + 47 hpp/h），`cli/` 单文件 `main.cpp` 约 2.4K 行，
> `apps/` Android 应用 55 个 Kotlin 文件（含第三方头不计）。
>
> **报告结构**：§0 定位 → §1 顶层布局 → §2–§5 四大子系统 → §6 技术栈 → §7 最新性能结论 → §8 技术债与风险 → §9 关键调用链速查。

---

## 0. 一句话定位

CrossScale 是一个**跨平台分布式图像超分辨率系统**：核心推理引擎与分布式协议用 C++（C++23 / C++17 JNI）实现并共享于 `share/`，对外暴露两条入口——PC 端 CLI（`cli/`）和 Android 应用（`apps/`），通过局域网 TCP + 自定义二进制协议把多设备（含其 CPU/GPU）组成一个超分集群，按各端实测吞吐权重分配 tile 任务；并新增一个本地常驻守护进程（`serve`）让单图命令行调用复用热引擎，规避启动固定开销。

```
                  ┌──────────── PC CLI (cli/, C++23) ────────────┐
                  │  process / server / worker / client / serve   │
                  └───────────────┬──────────────────────────────┘
                                  │ 链接
                  ┌───────────────▼──────────────────────────────┐
                  │          share/ (static lib, C++)            │
                  │  core/     推理引擎 + tile + 分布式调度 + serve│
                  │  utils/    网络(unix/tcp) / 图像 / 日志 / 加密 │
                  │  perf/     模块化性能测试框架                  │
                  └───────────────┬──────────────────────────────┘
                                  │ 同一份源码直接编进 .so
                  ┌───────────────▼──────────────────────────────┐
                  │      Android App (apps/, Kotlin + JNI)        │
                  │  Compose UI (MVVM) ← JNI 桥 → share/ 代码     │
                  └──────────────────────────────────────────────┘
```

---

## 1. 顶层布局

| 目录 | 角色 | 构建 | 标准 |
|------|------|------|------|
| `share/` | 共享 C++ 核心：推理引擎、tile 处理、分布式协议、serve 守护进程、网络/工具、perf 框架 | xmake 静态库（PC）/ CMake 直接编译（Android） | C++23 (PC), C++17 (JNI) |
| `cli/` | PC 命令行入口，单文件 `main.cpp`（2449 行） | xmake → `crossscale-cli` 二进制 | C++23 |
| `apps/` | Android 应用（原多平台工作区，桌面端 UI 已移除） | Gradle + CMake/NDK | Kotlin + C++17 JNI |
| `models/` | ncnn 模型仓库（`.param` + `.bin`） | 运行时扫描 | — |
| `docs/`、`scripts/` | 文档与运维/基准脚本 | — | — |

**核心设计原则：源码级共享。** `share/` 不是预编译库给 Android 用，而是其 `.cpp` 文件被 Android 的 CMake 直接列编（`apps/app/src/main/cpp/CMakeLists.txt:114-127`），保证两端协议、调度、引擎逻辑完全一致。唯一的平台差异是 ncnn 引擎实现可插拔（见 §3.7）。

> ⚠️ **审阅要点**：`share/` 的源文件在两处手写枚举——`share/xmake.lua:7-24`（PC）和 `apps/app/src/main/cpp/CMakeLists.txt`（Android，11 个文件）。新增 `.cpp` 需两处同步，易漂移。这是已知维护点（见 §8）。

---

## 2. 子系统一：推理引擎（`share/core/`，非 distributed 部分）

### 2.1 引擎抽象 —— `engine.hpp`

`Engine`（`share/core/engine.hpp:44`）是纯虚接口，定义推理后端契约：

- `initialize(DeviceType)` / `list_devices()` / `load_model(param, bin)`
- `process(...)` —— 单 tile float 推理（纯虚）
- `process_u8(...)` —— u8 路径，默认返回"不支持"
- `process_batch(...)` —— **有默认实现**：GPU 强制串行（Vulkan 不支持并发提交，`engine.hpp:96`），CPU 用 `std::async` 并发
- GPU tile 流水线三档：`supports_gpu_tile_pipeline[_u8][_u8_queue]` + 对应 `process_tiles_gpu_*`
- `benchmark(tile_size)`、`get_recommended_tile_size()`、`get_memory_info()`

工厂 `create_ncnn_engine()`（`engine.hpp:223`）经 `engine.cpp:5` 转发到 `create_ncnn_engine_impl()`——这个间接层是为 Android 端可替换 stub（§3.7）。

### 2.2 ncnn 后端 —— `ncnn_engine.cpp`

真正的实现是 PIMPL 风格的 `NCNNEngineImpl`（`ncnn_engine.cpp:535`），**3800+ 行，是整个项目最重的文件**。

**CPU 推理**（`process`，`ncnn_engine.cpp:719`）：interleaved HWC float → planar CHW `ncnn::Mat` → extractor → planar → interleaved。

**Vulkan GPU（默认首选）**：
- `initialize` 先 `try_init_vulkan()`，失败回退 CPU（`ncnn_engine.cpp:540`）
- GPU 实例创建时**枚举所有 GPU 并打分选最优**：discrete +10000、integrated +1000、fp16 支持 +50/+100、compute_queue ×10（`ncnn_engine.cpp:3686-3706`）
- 带重试机制（默认 6 次指数退避，env `CROSSSCALE_GPU_INIT_RETRIES` 可调）
- HWC↔CHW 转换全部在 GPU 上用**自写的 Vulkan compute shader** 完成（`ncnn_engine.cpp:43-171` 等 GLSL 段），避免 CPU↔GPU 往返
- fp16 受 `CROSSSCALE_GPU_FAST_MATH=1` 控制，默认关闭保精度
- 全局互斥锁串行化 Vulkan 实例/模型加载（ncnn 的 Vulkan 状态是进程级单例）

**Batch 推理**（`ncnn_engine.cpp:1093`）：多 tile 复用同一 blob/staging allocator，逐个 clone→extract，最后一次性提交 command buffer。

**GPU tile 流水线最高档** `process_tiles_gpu_u8_queue`（`ncnn_engine.cpp:1288`）：整图 u8 上传一次，在 GPU 上 crop + 推理 + 量化 + 拼回，最小化 CPU 参与。

> ⚠️ **审阅要点**：`ncnn_engine.cpp` 3800 行单文件，承担了设备枚举/打分/Vulkan 初始化/shader/CPU 路径/batch/三档 GPU 流水线，是后续可维护性的最大风险点。未见拆分计划。

### 2.3 Tile 处理 —— `tile_processor.{hpp,cpp}` / `tile_types.hpp`

`TileProcessor`（`tile_processor.hpp:21`）：**大图切块 → 逐块推理 → 加权融合拼回**。

- `compute_tile_layout(w, h, config)`：按 `step = tile_size - overlap` 网格化，边界自动 clamp
- **三段流水线**（`tile_processor.cpp:113-285`）：Load 线程（切块提取）→ 主线程（Proc，调 `engine.process_batch`）→ Save 线程（融合），用两个有界队列（容量 2）+ `condition_variable` 同步，让 CPU 提取/融合与 GPU 推理重叠
- GPU 快速路径：若引擎支持，整图直接交给引擎的 GPU 流水线，TileProcessor 不再切（`tile_processor.cpp:76-99`）
- `blend_tile`：重叠区二次衰减权重（`w*w`），最后归一化，消除接缝
- `extract_tile_u8`：逐 tile 转 float，**避免整图 float 拷贝**，省 O(W·H·C) 内存
- `bicubic_scale_u8`：Mitchell-Netravali 双三次插值，**RGBA 的 alpha 通道走旁路**不过神经网络，省 25% GPU 算力

`tile_types.hpp` 定义 `TileRegion` / `TileConfig`(tile_size=256, overlap=16, batch_size=1) / `ProcessingProgress`。

### 2.4 Tile 大小优化器 —— `tile_optimizer.hpp`（header-only）

`TileSizeOptimizer::find_optimal_tile_size`（`tile_optimizer.hpp:147`）自动选最优 tile：

1. 候选 {64,128,192,256,320,384,448,512}，按 min/max 过滤
2. 对每个尺寸 `benchmark_tile_size` 实测 tiles/sec + 估算内存
3. 内存超可用 80% 淘汰
4. 在"最小耗时 ×1.05 容忍阈值"内**挑最大的 tile**（提高 GPU 利用率）

### 2.5 引擎池 —— `engine_pool.{hpp,cpp}`

`EnginePool`（`engine_pool.hpp:13`）：预创建 N 个独立 Engine 实例 + 队列复用，让多线程并发推理而不串扰 Vulkan 上下文（每线程需独占一个 Engine + 自己的 VkCompute）。`acquire()`/`release()` 用 mutex + queue 实现，空池抛异常（上层自限流）。

### 2.6 模型管理 —— `model_manager.{hpp,cpp}`

`ModelManager`（`model_manager.hpp:27`）：扫描 `models/` 目录，解析 `.param` 文件推断元数据：

- **放大倍数从文件名推断**：`x2`→2、`x3`→3
- **输入通道数推断**：先看 `Input` 层 channels，没有则从首个卷积层的 `weight_data_size / (num_output × kernel_w × kernel_h) × groups` 反推
- 模型名 = `父目录名-param文件名`，如 `Real-ESRGANv3-anime-x2`
- 支持导入/校验/删除，`compute_hash` 做简单非加密完整性校验

### 2.7 Android 的可插拔引擎 —— `ncnn_engine_stub_android.cpp`

Android 端通过 CMake 开关 `CROSSSCALE_USE_NCNN` 决定：
- **ON**：编译真实的 `share/core/ncnn_engine.cpp`（需 jniLibs 里有 `libncnn.a`）
- **OFF**：编译 `apps/app/src/main/cpp/ncnn_engine_stub_android.cpp`，返回 `AndroidStubEngine`——`process()` 只做最近邻上采样（无神经网络），`benchmark` 固定 1.0，让代码能编通走链路。**确认是 stub**（`ncnn_engine_stub_android.cpp:12`）。

### 2.8 配置 —— `config.hpp`

`ProcessingConfig`（`config.hpp:13`）聚合 tile/batch/内存/线程/GPU/模型/输出/网络缓冲等参数，自带手写 JSON 持久化（`config.cpp:113`）和校验。同文件还定义 `DevicePerformanceProfile`、`ModelConstraints`，供优化器估算用。

### 2.9 引擎子系统调用关系

```
上层调用方
   │
   ├─ ModelManager.scan_models() → ModelInfo
   │
   └─ TileProcessor.process_image[_u8](engine, config)
          │
          ├─ [GPU 快速路径] engine.process_tiles_gpu_u8_queue()
          │
          └─ [通用路径] 三段流水线 → engine.process_batch() → blend_tile
                          │
                          └─ 多线程时 EnginePool.acquire/release

auto_tile_size=true 时：
   上层 → TileSizeOptimizer.find_optimal_tile_size(engine, model, config)
            └─ engine.process() 多次 benchmark
```

---

## 3. 子系统二：分布式计算（`share/core/distributed/`）

### 3.1 二进制协议 —— `protocol.{hpp,cpp}`

**定长 15 字节小端帧头**（`protocol.hpp:47-53`）：

| 字段 | 字节 | 说明 |
|------|------|------|
| magic | 4 | `0x53534358` ("XCSS")，校验用 |
| version | 1 | |
| type | 2 | `MessageType` |
| payload_length | 4 | |
| checksum | 4 | CRC32（`Protocol::compute_checksum`） |

`Protocol::serialize(header, payload)` 产出 `[15字节头][payload]` 完整帧；`parse_header` 反解。这俩是**传输无关**的，TCP 与 Unix socket 都复用，无需改动。

**消息类型**（`MessageType`，`protocol.hpp:10-37`）按功能分组：
- 握手认证（0x0003~）：`AUTH_REQUEST/RESPONSE`、`DISCONNECT`
- 心跳（0x0006）
- 设备管理（0x0100~）：`DEVICE_PROFILE`
- 模型同步（0x0200~）：`MODEL_LIST/REQUEST/DATA`
- 任务分发（0x0300~）：`TASK_ASSIGNMENT/ACCEPT/PROGRESS/RESULT`
- 图像分块（0x0400~）：`IMAGE_TILE`、`TILES_BATCH`
- 客户端卸载（0x0500~）：`CLIENT_SUBMIT/RESPONSE`
- **本地 serve 守护进程（0x0601~，独立段，见 §5）**：`SERVE_REQUEST/RESPONSE`

**认证**：明文 key 比对（`AuthRequest.auth_key[64]`），无链路加密。`crypto::xor_encrypt` 存在但未用于传输。key 默认随机生成或用户 `-k` 指定。

**配对短串**：`ConnectionInfo` 可序列化为 `ip:port#key`，QR 码即此串。

### 3.2 三种角色

**Coordinator**（`coordinator.{hpp,cpp}`，服务端）：监听 TCP，认证 worker，把客户端提交的图切块，按调度器指派分发，收集结果拼图。核心 `distribute_task()` 是"算 tile 数 → 分波派发 → 收齐 → 拼回"的多波循环（`coordinator.cpp:115-428`）。兼做模型分发服务器。

**Worker**（`worker.{hpp,cpp}`，工作端）：连到 coordinator，认证，**握手时本地 benchmark 测速并上报**，接收 tile 任务、用 Engine 推理、回传结果，周期心跳（5s），按需同步模型（带路径穿越校验）。

**Client**（无独立类，在 `coordinator.cpp` 的 `handle_client` 内联实现，也在 CLI `main.cpp` 的 `client process` 直接走 socket）：用 `MSG_CLIENT_SUBMIT` 把整图发给 coordinator，收 `MSG_CLIENT_RESPONSE` 二进制结果。

### 3.3 权重调度 —— `scheduler.{hpp,cpp}`

`Scheduler` 维护在线设备表，按各设备 `tiles_per_second` 归一化为权重：

```
weight[i] = tps[i] / Σ tps[i]     （否则 1/N 均分）
```

**权重数据来源**：worker 在 `handshake` 时调 `engine.benchmark(256)`——对 256×256 哨兵 tile 跑 5 轮取均时（`ncnn_engine.cpp:2887`），返回 `tiles_per_second = 1000/avg_ms`——经 `DeviceProfileMessage` 上报给 coordinator，coordinator `register_device` 时归一化。

**任务分配** `assign_tiles()`（`scheduler.cpp:30-111`）：筛 alive（30s 内有心跳）→ 按权重降序排 → 每设备配额 = `ceil(total × weight)`，受 `max_batch_size` 封顶。失败重调度靠 coordinator 的**多波循环**——失败的 tile 下一波重分（`handle_device_failure` 的孤儿回收目前是空实现）。

### 3.4 网络与发现 —— `share/utils/`

| 模块 | 职责 | 状态 |
|------|------|------|
| `network.{hpp,cpp}` | POSIX socket 薄封装（`TCPSocket`/`TCPServer`）+ `crc32` + 本地 IP/auth-key 生成 | 完整 |
| `unix_socket.{hpp,cpp}` | AF_UNIX 封装，镜像 TCP API，仅 Linux/macOS 编译（见 §5） | 完整（新） |
| `mdns.{hpp,cpp}` | 局域网服务发现。`MDNSBroadcaster` 用 avahi-publish-service 发 `_crossscale._tcp`；`MDNSDiscovery` **未实现**，仅打 warning | **半实现**（广播 OK / 发现 stub） |
| `crypto.{hpp,cpp}` | sha256/md5/base64/uuid/random，用于 task_id/device_id 生成与 mDNS key hash | 完整 |

### 3.5 分布式调用链（典型 worker 模式）

```
启动认证：
  CLI → Coordinator.start() → TCPServer.listen()
  Worker.connect() → send MSG_AUTH_REQUEST → 收 device_id
        → engine.benchmark(256) 测速 → send MSG_DEVICE_PROFILE
        → Coordinator → Scheduler.register_device() → normalize_weights()

任务执行：
  Client → Coordinator.distribute_task(image)
    切 tile 数 → 每波循环:
      Scheduler.assign_tiles(pending) ── 按权重切配额
      → 每个 worker: send TASK_ASSIGNMENT + TILES_BATCH
        ↓ worker 侧
        Worker.execute_task() → engine.process() 逐 tile 推理 → send TASK_RESULT
        ↑ coordinator 收齐
      collect_results() → 刷新心跳
    → stitch_tiles() 拼回大图 → MSG_CLIENT_RESPONSE
```

---

## 4. 子系统三：前端

### 4.1 PC CLI（`cli/src/main.cpp`，2449 行）

**子命令**：`process` / `server [start|info]` / `worker connect` / `client process` / `model list` / `benchmark` / `perf` / `serve [start|stop]`，支持隐式 process（`main.cpp:309-326`）。

**参数解析**：手写 if-else 循环 + `safe_parse_int/float`。

> ⚠️ **审阅要点（技术债）**：`cli/xmake.lua:5` 声明了 `cxxopts` 依赖但 **main.cpp 完全没用**（死依赖，可清理）。

**单机 process 调用链**（单文件模式，`main.cpp:1206-1396`）：

```
main → ModelManager.scan_models() 选模型
     → create_ncnn_engine() → initialize(GPU 优先) → load_model()
     → [auto-tile] TileSizeOptimizer.find_optimal_tile_size()
     → Image::load → normalize_channels_for_model
     → [serve daemon 在则委派, §5] → 否则 TileProcessor.process_image_u8 → Engine 推理
     → Image::save(quality, format)
```

目录模式（`main.cpp:377-1191`）更复杂：CPU `--jobs>1` 用 `EnginePool`；GPU 单图串行但有 prepare/process/save 三级流水线 + 自适应队列深度。

**build**：`cli/xmake.lua` 通过 `add_deps("crossscale-core")` 链接 share 静态库，`share/xmake.lua:5` 的 `add_includedirs(".", {public=true})` 让 `share/` 成为公共 include 根（故 `#include "core/engine.hpp"`）。

### 4.2 Android 应用（`apps/app/`）

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
├── network/   (Coordinator / MDNS / Message / JoinInvite) ← 发现+封装
└── native/    (NCNNEngine.kt, NativeWorker.kt)         ← JNI 桥
```

**UI 层**：6 屏（Home/Devices/Models/Settings/Logs/Help）+ QR 扫描屏，7 个组件，底部导航 3 项。Material 3 + 动态取色（Android 12+）。导航用 navigation-compose 单 NavHost，**状态上提 + 回调下传**模式（所有 ViewModel 在顶层创建一次）。

**ViewModel 层**（3 个，全部 `StateFlow` + `WhileSubscribed`）：
- `MainViewModel`：模型列表/选中、输入源、输出、开关、连接设备
- `DeviceViewModel`：mDNS 发现（Android `NsdManager`）、手动/QR 连接、自动重连
- `ProcessingViewModel`（最重，2141 行）：批处理引擎，含队列去重、checkpoint 断点续传、failed-only 重试、备份恢复

**Data 层**：11 个 repository，全部持久化本地（EncryptedSharedPreferences 存 auth key、JSON 落 filesDir 存 checkpoint/queue）。7 个接口契约（`ProcessingRepositoryContracts.kt`）便于测试。

**网络层关键事实**：**Android 端的真正网络通信在 C++ JNI 层完成**（`jni_bridge.cpp` 的 `nativeProcessImageDistributed` 用 `crossscale::TCPSocket` + `Protocol`）。Kotlin `network/` 包只做**发现**（`MDNSDiscovery` 用 Android NsdManager）和**封装**（`CoordinatorClient` 调 JNI）。`MessageHandler` 目前是占位。`JoinInvite` 支持 4 种 payload 解析（JSON / `crossscale://` / `host:port#key` / 遗留格式）。

**JNI 桥**（`apps/app/src/main/cpp/jni_bridge.cpp`）暴露两类 native 方法：
- `NCNNEngine`（单例 object）：本地推理 `nativeProcessImage` + 分布式 `nativeProcessImageDistributed`（完整 client 协议在 C++ 内）
- `NativeWorker`（实例类，`AutoCloseable`）：长驻 worker 会话 `AndroidWorkerSession`，含后台任务循环线程

**Android 与 share 的关系**：`CMakeLists.txt:106-127` 把 share 的 11 个 `.cpp` **直接编译进 `libcrossscale-native`**，协议/worker/scheduler/网络全部与 PC 共用。唯一差异是 ncnn 引擎可插拔（§2.7）。

**build 关键项**：`compileSdk/targetSdk=36`，`minSdk=26`，JVM 21，NDK 29，CMake 4.1.2，C++17 -O3，ABI 三档（arm64-v8a/armeabi-v7a/x86_64），STL `c++_shared`。Release `isMinifyEnabled=true`，签名用环境变量无则回退 debug。

---

## 5. 子系统四：本地 serve 守护进程（`share/core/serve/`，2026-07-23 新增）

> **动机**：性能测量发现，单图命令行调用的 ~60% 时间是固定启动开销（Vulkan init ~332ms + model load ~159ms ≈ 490ms），真正推理只占 40%（见 §7）。守护进程常驻持有热引擎，把固定开销摊薄到零，单图加速 **2.2×–14.9×**。详细设计见 `docs/SERVE_DAEMON_DESIGN.md`，本节是综述。

### 5.1 架构

```
┌─────────────────────────────┐         ┌──────────────────────────────────────┐
│ client: crossscale process  │         │ daemon: crossscale serve (后台)       │
│  -i in.png -o out.png       │         │  持有热 Engine(init+load 一次)       │
│                             │         │  UnixServer 监听 serve.sock           │
│  ① 探测 socket 是否存在/可连 │──不存在──▶ 静默回退内联路径(行为零变化)        │
│  ② 连上 → 读图 → 拼 request │         │                                      │
│       = [ServeRequest][图]  │─Unix──▶ │  ③ accept → handle_client(每连接一线程)│
│  ⑥ 收 [ServeResponse][结果] │◀─sock── │  ④ engine_mutex_ 下:懒换模型→推理      │
│     → Image::save           │         │  ⑤ 拼 [ServeResponse][结果] 一次性回发 │
└─────────────────────────────┘         └──────────────────────────────────────┘
```

**数据流是单向无状态的**：一个连接 = 一次请求 = 一次响应。无握手、无会话、无心跳。

### 5.2 传输 —— Unix domain socket（`share/utils/unix_socket.{hpp,cpp}`）

镜像 `TCPSocket`/`TCPServer` 的 API 表面，但用 `AF_UNIX`。**仅 Linux/macOS 编译**（`share/xmake.lua:31` 的 `is_plat("linux","macosx")` 门控）。

- `UnixServer::listen`：`unlink` 旧 socket 文件（清理崩溃残留）→ `bind` → **`chmod 0600`**（仅属主可连，本地鉴权）→ `listen(backlog=16)`
- `UnixServer::accept`：**poll 500ms 超时循环**，让 accept 环能周期性醒来观察关闭标志
- `UnixSocket::send_all`/`receive_all`：循环直到发/收满，保证帧完整性；`io_mutex_`（`std::recursive_mutex`）保护并发 I/O
- 边界：残留 socket 文件 → `listen` 先 `unlink`；path 过长 → 提前报 `ENAMETOOLONG` 而非截断

### 5.3 协议（复用分布式帧头 + 2 个新消息）

复用 15 字节帧头（§3.1），新增 2 个消息类型（独立 `0x06xx` 段，与分布式 TCP 协议解耦，可独立演进）：

```cpp
MSG_SERVE_REQUEST  = 0x0601,  // client→daemon
MSG_SERVE_RESPONSE = 0x0602,  // daemon→client
```

2 个线协议结构体：
```cpp
struct ServeRequest  { char model_name[64]; int32_t tile_size, tile_overlap,
                       width, height, channels; uint32_t img_length; };
struct ServeResponse { int32_t ok; int32_t width, height, channels;
                       uint32_t img_length; char error[128]; };
```

线格式（payload 部分，接在 15 字节头后）：结构体 + 原始图像字节**连续拼接成单个 payload**，由 `Protocol::serialize` 包成**一个完整帧**一次性发送（这是简化的关键之一）。

### 5.4 守护进程核心 —— `serve/daemon.{hpp,cpp}`

`ServeDaemon` 持有：一个 `Engine`（已 init+load）、一个 `ModelManager`、一个 `UnixServer`、一个 `TileProcessor`，加 `engine_mutex_` 串行化引擎访问。

**`start()`**：引擎初始化（摊薄 ~490ms 固定开销的关键）+ 绑定 socket。错误码语义明确（2=init 失败, 3=扫描失败, 4=无模型, 5=模型名不存在, 6=加载失败, 7=socket 失败）。

**`run()`** accept 主循环：每连接 `std::thread(detach)` → `handle_client`，让慢的 GPU 推理不阻塞 accept。**引擎访问仍由 `engine_mutex_` 串行**（Vulkan 不支持并发 submit 的硬约束，`engine.hpp:96`）。

**`handle_client`**：读头 → 校验 → 读 payload → `memcpy` 出 `ServeRequest` → 校验 `img_length <= 256MB`（`MAX_SERVE_IMAGE_BYTES`）→ `lock_guard(engine_mutex_)` 下懒换模型 + `process_image_u8` 推理 → 拼响应一次性发。

**懒换模型 `ensure_model_loaded`**：`model_name` 空/相同 → 直接返回（常见路径）；否则 `get_model` + `load_model`（参照 `worker.cpp:305` 模式）。

**正确性保证**：守护进程调的是**与单图 CLI 内联路径完全相同的 `TileProcessor::process_image_u8`**（同参数：tile_size、overlap、batch=1、enable_alpha_bicubic=true）。A/B 实测 md5 逐字节一致（§7）。

### 5.5 CLI 集成 —— 委派探测 + serve 子命令

**委派探测**（`main.cpp:426-539`）：在 `process` 单图分支开头插入 lambda `delegate()`，返回 `std::optional<int>`：
- 任何"无守护进程"迹象（socket 不存在、connect 拒绝、读图失败）→ 返回 `nullopt`，**静默**落回内联路径（零回归）
- 真正的 daemon 侧错误 → 返回 1 并打印
- `--no-daemon` flag 强制跳过探测（调试用）

**`serve start`**（默认后台）：
1. 防重复启动守卫：读 pid 文件，`kill(pid, 0)==0`（进程活着）则拒绝；pid 文件在但进程死 → 视为残留覆盖
2. 守护化（非 `--fg`）：单 `fork` → 父进程 `return 0` → 子进程 `setsid` → 关闭 stdio
3. `daemon.start()` → 失败删 pid 文件退出
4. **成功后**写 pid 文件（避免写了 pid 又启动失败留误导性文件）
5. 接线信号处理（见下）→ `daemon.run()` → 退出清理

**`serve stop`**：读 pid → `kill(pid, SIGTERM)` → daemon 的信号处理器触发 `stop()`。异步：不等真正退出即返回。

> ⚠️ **审阅重点（验证期发现并修复的真实 bug，commit `d1d8a68`）**：初版简化砍掉协议内的 `MSG_SERVE_STOP` 改用 SIGTERM 后，出现 daemon 收到 SIGTERM 不退出的回归——因为通用 `signal_handler` 只设全局标志 `g_running`，而 `ServeDaemon::run()` **不观察** `g_running`，只看自己的成员。修复：新增专用 `serve_signal_handler`，通过文件级静态指针 `g_serve_daemon`（仅 serve start 路径赋值）"伸手"调 `daemon.stop()`。这是"简化引入回归"的典型案例，审阅时应确认该指针的生命周期与线程安全是否健壮。

---

## 6. 跨切面：技术栈与依赖

| 层 | 技术 |
|----|------|
| 推理 | ncnn（Vulkan GPU 加速），自写 GLSL compute shader |
| 图像 | libpng / libjpeg / libwebp / stb_image |
| 构建（PC） | xmake，C++23 |
| 构建（Android） | Gradle + CMake/NDK，C++17 JNI |
| Android UI | Jetpack Compose + Material 3 + navigation-compose |
| 二维码 | qr-code-generator-cpp（PC）/ CameraX + ML Kit（Android 扫码） |
| 协议 | 自定义二进制（15B 帧头 + CRC32） |
| 本地 IPC | Unix domain socket（Linux/macOS） |
| 服务发现 | mDNS（PC 端 avahi-publish；Android NsdManager） |
| 安全 | EncryptedSharedPreferences（Android auth key）；明文 key 认证（协议层）；chmod 0600（本地 IPC） |

**构建安全加固**（`xmake.lua`）：debug 开 ASan；linux/macosx 加 `-fstack-protector-strong`、`-D_FORTIFY_SOURCE=2`、`-fPIE`、`-pie -Wl,-z,relro,-z,now`；release 开 `-Werror`、`-O3`、strip。

**依赖版本锁定**（`xmake.lua`）：ncnn 锁 `20260113`——因为 ncnn `20260526` 的 Vulkan 依赖 `glslang-nihui` 无匹配 release，`20260113` 是最新能解析其 glslang 依赖的版本。

### 依赖来源

- PC 端：`xmake require` 拉取 ncnn/libwebp/libpng/libjpeg/glaze/qr-code-generator-cpp
- Android 端：`apps/scripts/sync_native_deps.sh` 用 `xrepo install` 把 ncnn/glslang/libomp/zlib/libpng/libjpeg/libwebp 编进 `jniLibs/` + 头文件进 `cpp/third_party/`（这些是构建产物，不入 git）

---

## 7. 最新性能结论（2026-07-23，详见 `docs/PERF_REPORT_REAL_IMAGES.md`）

测试机：RTX 4060 Laptop GPU (8GB) + Intel UHD。模型：Real-ESRGANv3-anime ×2 (fp16)。

### 7.1 三个核心发现

| # | 发现 | 数据 |
|---|------|------|
| **1** | **单图端到端 GPU≈CPU** | 256² GPU 0.53s vs CPU 0.52s；1024² GPU 0.88s vs CPU 0.84s |
| **2** | **根因：固定开销占 60%** | 1024² 分解：Vulkan init 332ms(39%) + model load 159ms(19%) + 推理仅 40% |
| **3** | **批量复用引擎时 GPU 优势爆发：快 CPU 4 倍** | 7 图批量：GPU 1.22s vs CPU 4.91s；比逐图处理快 4.1× |

### 7.2 serve 守护进程实测加速（单图，daemon 在 vs 不在）

| 图像 | 内联（无 daemon） | 经 daemon | 加速 |
|---|---|---|---|
| photo_256 | 0.521s | **0.035s** | **14.9×** |
| text_400 | 0.532s | 0.053s | 10.0× |
| anime_512 | 0.558s | 0.074s | 7.5× |
| portrait_512 | 0.580s | 0.119s | 4.9× |
| photo_1024 | 0.849s | 0.380s | 2.2× |

小图收益最大（固定开销占比更高）。

### 7.3 serve 守护进程验证矩阵

| 维度 | 结果 |
|---|---|
| **正确性（A/B）** | 5 图 daemon 输出 vs 内联输出，**md5 逐字节相同**（同一 `process_image_u8` 路径） |
| **回退** | daemon 不在 → `exists(socket)` 快速失败 → 静默走内联路径，行为零影响 |
| **并发** | 3 客户端同时请求，`engine_mutex_` 串行处理，输出全部正确，daemon 存活 |
| **生命周期** | 残留 socket / 残留 pid（进程死）/ already-running 三种边界均正确处理 |

### 7.4 perf 框架（`share/perf/`）

模块化性能测试框架：`runner` + `workload` + `probes`(GPU SM util/mem/power 采样) + `reporter`。三个内置测试：
- `gpu_efficiency`（基础效率）
- `gpu_concurrency`（并发多引擎 submit，util 59%→96%）
- `gpu_reuse`（VkCompute 复用，bit-identical，单引擎 +11%）

经 CLI `crossscale perf <test>` 暴露。

### 7.5 性能优化历史脉络

| commit | 内容 | 收益 |
|---|---|---|
| `ebdc465` | 模块化 perf 框架 + GPU 效率测试 | 基线 |
| `f102624` | 并发多引擎 GPU submit | util 59%→96% |
| `c1bae0d` | 穷尽 GPU 探索报告 | 物理极限 |
| `63ef8aa` | VkCompute 复用 | 单引擎 +11%（bit-identical） |
| `f48a98f` | 真实图像端到端报告 | 揭示固定开销瓶颈 |
| `4545191` | serve 守护进程 | 单图 2-15× |

---

## 8. 技术债与风险（供审阅优先评估）

### 已确认的问题

1. **`mdns` 发现侧未实现**（`MDNSDiscovery::start` 仅打 warning）。PC 服务端广播 OK，但 PC 端发现其他 coordinator 是 stub；Android 端用原生 NsdManager，与 PC 侧不共享代码。

2. **CLI `cxxopts` 是死依赖**（`cli/xmake.lua:5`）：声明了却完全没用，手写解析。应删或用起来。

3. **`Scheduler::handle_device_failure` 孤儿回收是空实现**，靠 coordinator 多波循环兜底，单波内部分失败可能延迟到下一波才重试。

4. **协议层无链路加密**，明文 key 认证，仅适合可信局域网。`crypto::xor_encrypt` 形同虚设。

5. **`MessageHandler.kt` 是占位**，未充分使用。

6. **`ProcessingViewModel` 2141 行偏重**，可考虑拆分。

7. **`ncnn_engine.cpp` 3800+ 行单文件**，是可维护性最大风险点，无拆分计划。

8. **两处手写源文件枚举易漂移**：`share/xmake.lua:7-24`（PC）和 `apps/app/src/main/cpp/CMakeLists.txt`（Android，11 个文件）需保持同步。

### serve 守护进程特有风险

9. **平台限制**：AF_UNIX 仅 Linux/macOS。Android/Windows 的 `serve` 子命令需在命令层显式 gate（当前 CLI 本就面向 Linux/macOS，可接受，但应在 help 里说明）。

10. **推理强制串行**：无论多少并发连接，GPU 推理都排队（Vulkan 单例硬约束）。高并发批量场景应用目录模式 `--jobs`（多引擎实例），而非 daemon。

11. **256MB 图像上限**（`MAX_SERVE_IMAGE_BYTES`）：超大单图可能超限，返回 "image too large"；客户端可加 `--no-daemon` 回退。

12. **stop 是异步的**：`serve stop` 发 SIGTERM 后立即返回，daemon 实际 ≤500ms 后退出。连续快速 start/stop 可能遇到窗口（有 already-running 守卫兜底）。

13. **pid 文件残留**：进程被 `kill -9` 强杀会残留 pid 文件（已有"pid 在但进程死"的守卫处理）。

### 安全方面

14. **明文 key 认证 + 无链路加密**（§3.1）。若部署在不可信网络需重新设计认证/传输层。
15. **本地 IPC 未加密 payload**，仅靠 `chmod 0600`（多用户环境需注意）。

---

## 9. 关键调用链速查

**单机超分（PC CLI 单图，daemon 不在）**：
```
main.cpp → ModelManager.scan_models
        → create_ncnn_engine → Engine.initialize → Engine.load_model
        → [auto] TileSizeOptimizer.find_optimal_tile_size
        → Image::load → normalize_channels_for_model
        → TileProcessor.process_image_u8 → Engine.process[_batch] → blend_tile
        → Image::save
```

**单机超分（PC CLI 单图，daemon 在）**：
```
main.cpp → [serve daemon 探测] → 连 Unix socket
        → send [ServeRequest][图] → daemon: process_image_u8 → recv [ServeResponse][结果]
        → Image::save
```

**分布式超分（Client → Coordinator → Workers）**：
```
client process → TCPSocket → MSG_AUTH_REQUEST → MSG_CLIENT_SUBMIT(整图)
  → Coordinator.distribute_task
    → Scheduler.assign_tiles (按 tps 权重)
    → 各 Worker.execute_task → Engine.process(逐 tile) → MSG_TASK_RESULT
    → collect_results → stitch_tiles
  → MSG_CLIENT_RESPONSE → 写文件
```

**Android 本地超分**：
```
UI → ProcessingViewModel → NativeImageProcessingRepository
   → NCNNEngine.nativeProcessImage (JNI)
   → jni_bridge.cpp → share/core 的 Engine + TileProcessor
```

**Android 加入集群**：
```
DeviceViewModel → MDNSDiscovery (NsdManager 发现) 或 QR 扫描 → JoinInvite 解析
  → CoordinatorClient.connect → NativeWorker.nativeConnect (JNI)
  → jni_bridge.cpp AndroidWorkerSession → share/core Worker (handshake+benchmark+心跳+任务循环)
```

**serve 守护进程生命周期**：
```
crossscale serve start → fork+setsid 守护化 → ServeDaemon.start(init+load+listen)
                       → 写 pid 文件 → 接线 serve_signal_handler → run() accept 循环
crossscale process -i in -o out → 探测 socket → 委派 → 收结果
crossscale serve stop → 读 pid → kill(SIGTERM) → serve_signal_handler → daemon.stop()
                      → run() 在 ≤500ms 内退出 → 析构 unlink socket → 删 pid
```

---

## 附：相关文档索引

| 文档 | 内容 |
|------|------|
| `docs/ARCHITECTURE.md` | 2026-07-04 的旧架构报告（serve daemon 之前的版本，本报告已整合其内容） |
| `docs/SERVE_DAEMON_DESIGN.md` | serve 守护进程完整设计（§5 的展开，含协议简化前后对比） |
| `docs/PERF_REPORT_REAL_IMAGES.md` | 真实图像端到端性能报告（§7 的数据来源） |
| `docs/PERF_REPORT_GPU_EFFICIENCY.md` | GPU 效率报告 |
| `docs/PERF_REPORT_GPU_EXHAUSTIVE.md` | GPU 穷尽探索报告 |
| `docs/PONYTAIL_AUDIT.md` | 全仓库过度工程审计 |
| `docs/superpowers/` | serve daemon 简化的实现计划与设计记录 |
| `AGENTS.md` | 构建命令、代码风格、仓库结构（开发指南） |
| `README.md` | 用户向使用说明 |
