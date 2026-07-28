# CrossScale 架构分析报告

> 本报告基于对当前代码库的静态分析，梳理系统的整体架构、子系统职责、关键调用链与技术决策。

## 0. 一句话定位

CrossScale 是一个**跨平台分布式图像超分辨率系统**：核心推理与分布式协议用 C++（C++23 / C++17 JNI）实现并共享于 `share/`，对外暴露两条入口——PC 端 CLI（`cli/`）和 Android 应用（`apps/`），通过局域网 TCP + 自定义二进制协议把多设备（含其 CPU/GPU）组成一个超分集群，按各端实测吞吐权重分配 tile 任务。

```
                  ┌──────────── PC CLI (cli/, C++23) ────────────┐
                  │  process / server / worker / client / model  │
                  └───────────────┬──────────────────────────────┘
                                  │ 链接
                  ┌───────────────▼──────────────────────────────┐
                  │          share/ (static lib, C++)            │
                  │  core/     推理引擎 + tile + 分布式调度        │
                  │  utils/    网络 / 图像 / 日志 / 加密           │
                  └───────────────┬──────────────────────────────┘
                                  │ 同一份源码直接编进 .so
                  ┌───────────────▼──────────────────────────────┐
                  │      Android App (apps/, Kotlin + JNI)        │
                  │  Compose UI (MVVM) ← JNI 桥 → share/ 代码     │
                  └──────────────────────────────────────────────┘
```

## 1. 顶层布局

| 目录 | 角色 | 构建 | 标准 |
|------|------|------|------|
| `share/` | 共享 C++ 核心：推理引擎、tile 处理、分布式协议、网络/工具 | xmake 静态库（PC）/ CMake 直接编译（Android） | C++23 (PC), C++17 (JNI) |
| `cli/` | PC 命令行入口，单文件 `main.cpp` | xmake → `crossscale-cli` 二进制 | C++23 |
| `apps/` | Android 应用（原多平台工作区，桌面端 UI 已移除） | Gradle + CMake/NDK | Kotlin + C++17 JNI |
| `models/` | ncnn 模型仓库（`.param` + `.bin`） | 运行时扫描 | — |
| `docs/`、`scripts/` | 文档与运维脚本 | — | — |

**关键设计：源码级共享。** `share/` 不是预编译库给 Android 用，而是其 `.cpp` 文件被 Android 的 CMake 直接列编（`apps/app/src/main/cpp/CMakeLists.txt:114-127`），保证两端协议、调度、引擎逻辑完全一致。唯一的平台差异是 ncnn 引擎实现可插拔（见 §3.7）。

## 2. 三大子系统概览

| 子系统 | 位置 | 职责 |
|--------|------|------|
| **推理引擎** | `share/core/`（非 distributed 部分） | 引擎抽象、ncnn 后端（CPU/Vulkan）、tile 切分与融合、tile 大小自动优化、模型管理、引擎池 |
| **分布式计算** | `share/core/distributed/` + `share/utils/{network,mdns,crypto}` | 二进制协议、coordinator/worker 角色、权重调度、TCP 传输、mDNS 发现 |
| **应用前端** | `cli/src/main.cpp`（PC）+ `apps/app/`（Android） | 用户交互、命令路由、生命周期、UI |

下面依次展开。

---

## 3. 子系统一：推理引擎（`share/core/`）

### 3.1 引擎抽象 —— `engine.hpp`

`Engine`（`share/core/engine.hpp:44`）是纯虚接口，定义推理后端契约：

- `initialize(DeviceType)` / `list_devices()` / `load_model(param, bin)`
- `process(...)` —— 单 tile float 推理（纯虚）
- `process_u8(...)` —— u8 路径，默认返回"不支持"
- `process_batch(...)` —— **有默认实现**：GPU 强制串行（Vulkan 不支持并发提交，`engine.hpp:96`），CPU 用 `std::async` 并发
- GPU tile 流水线三档：`supports_gpu_tile_pipeline[_u8][_u8_queue]` + 对应 `process_tiles_gpu_*`
- `benchmark(tile_size)`、`get_recommended_tile_size()`、`get_memory_info()`

工厂 `create_ncnn_engine()`（`engine.hpp:223`）经 `engine.cpp:5` 转发到 `create_ncnn_engine_impl()`——这个间接层是为 Android 端可替换 stub（§3.7）。

### 3.2 ncnn 后端 —— `ncnn_engine.cpp`

真正的实现是 PIMPL 风格的 `NCNNEngineImpl`（`ncnn_engine.cpp:535`），3800+ 行，是整个项目最重的文件。

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

### 3.3 Tile 处理 —— `tile_processor.{hpp,cpp}` / `tile_types.hpp`

`TileProcessor`（`tile_processor.hpp:21`）：**大图切块 → 逐块推理 → 加权融合拼回**。

- `compute_tile_layout(w, h, config)`：按 `step = tile_size - overlap` 网格化，边界自动 clamp
- **三段流水线**（`tile_processor.cpp:113-285`）：Load 线程（切块提取）→ 主线程（Proc，调 `engine.process_batch`）→ Save 线程（融合），用两个有界队列（容量 2）+ `condition_variable` 同步，让 CPU 提取/融合与 GPU 推理重叠
- GPU 快速路径：若引擎支持，整图直接交给引擎的 GPU 流水线，TileProcessor 不再切（`tile_processor.cpp:76-99`）
- `blend_tile`：重叠区二次衰减权重（`w*w`），最后归一化，消除接缝
- `extract_tile_u8`：逐 tile 转 float，**避免整图 float 拷贝**，省 O(W·H·C) 内存
- `bicubic_scale_u8`：Mitchell-Netravali 双三次插值，**RGBA 的 alpha 通道走旁路**不过神经网络，省 25% GPU 算力

`tile_types.hpp` 定义 `TileRegion` / `TileConfig`(tile_size=256, overlap=16, batch_size=1) / `ProcessingProgress`。

### 3.4 Tile 大小优化器 —— `tile_optimizer.hpp`（header-only）

`TileSizeOptimizer::find_optimal_tile_size`（`tile_optimizer.hpp:147`）自动选最优 tile：

1. 候选 {64,128,192,256,320,384,448,512}，按 min/max 过滤
2. 对每个尺寸 `benchmark_tile_size` 实测 tiles/sec + 估算内存
3. 内存超可用 80% 淘汰
4. 在"最小耗时 ×1.05 容忍阈值"内**挑最大的 tile**（提高 GPU 利用率）

### 3.5 引擎池 —— `engine_pool.{hpp,cpp}`

`EnginePool`（`engine_pool.hpp:13`）：预创建 N 个独立 Engine 实例 + 队列复用，让多线程并发推理而不串扰 Vulkan 上下文（每线程需独占一个 Engine + 自己的 VkCompute）。`acquire()`/`release()` 用 mutex + queue 实现，空池抛异常（上层自限流）。

### 3.6 模型管理 —— `model_manager.{hpp,cpp}`

`ModelManager`（`model_manager.hpp:27`）：扫描 `models/` 目录，解析 `.param` 文件推断元数据：

- **放大倍数从文件名推断**：`x2`→2、`x3`→3
- **输入通道数推断**：先看 `Input` 层 channels，没有则从首个卷积层的 `weight_data_size / (num_output × kernel_w × kernel_h) × groups` 反推
- 模型名 = `父目录名-param文件名`，如 `Real-ESRGANv3-anime-x2`
- 支持导入/校验/删除，`compute_hash` 做简单非加密完整性校验

### 3.7 Android 的可插拔引擎 —— `ncnn_engine_stub_android.cpp`

Android 端通过 CMake 开关 `CROSSSCALE_USE_NCNN` 决定：
- **ON**：编译真实的 `share/core/ncnn_engine.cpp`（需 jniLibs 里有 `libncnn.a`）
- **OFF**：编译 `apps/app/src/main/cpp/ncnn_engine_stub_android.cpp`，返回 `AndroidStubEngine`——`process()` 只做最近邻上采样（无神经网络），`benchmark` 固定 1.0，让代码能编通走链路。**确认是 stub**（`ncnn_engine_stub_android.cpp:12`）。

### 3.8 配置 —— `config.hpp`

`ProcessingConfig`（`config.hpp:13`）聚合 tile/batch/内存/线程/GPU/模型/输出/网络缓冲等参数，自带手写 JSON 持久化（`config.cpp:113`）和校验。同文件还定义 `DevicePerformanceProfile`、`ModelConstraints`，供优化器估算用。

### 3.9 引擎子系统调用关系

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

## 4. 子系统二：分布式计算（`share/core/distributed/`）

### 4.1 二进制协议 —— `protocol.{hpp,cpp}`

**定长 15 字节小端帧头**（`protocol.hpp:47-53`）：

| 字段 | 字节 | 说明 |
|------|------|------|
| magic | 4 | `0x53534358` ("XCSS")，校验用 |
| version | 1 | |
| type | 2 | `MessageType` |
| payload_length | 4 | |
| checksum | 4 | CRC32（`network_utils::crc32`） |

**消息类型**（`MessageType`，`protocol.hpp:9-45`）按功能分组：
- 握手认证（0x0001~）：`HANDSHAKE`、`AUTH_REQUEST/RESPONSE`、`DISCONNECT`
- 心跳（0x0006~）
- 设备管理（0x0100~）：`DEVICE_PROFILE`、`DEVICE_JOIN/LEAVE`
- 模型同步（0x0200~）：`MODEL_LIST/REQUEST/DATA/SYNC`
- 任务分发（0x0300~）：`TASK_ASSIGNMENT/ACCEPT/REJECT/PROGRESS/RESULT/CANCEL`
- 图像分块（0x0400~）：`IMAGE_INFO`、`TILES_BATCH`
- 客户端卸载（0x0500~）：`CLIENT_SUBMIT/RESPONSE`

**认证**：明文 key 比对（`AuthRequest.auth_key[64]`），无链路加密。`crypto::xor_encrypt` 曾存在但未用于传输（已在 `8c11549` 删除）。key 默认随机生成或用户 `-k` 指定。

**配对短串**：`ConnectionInfo` 可序列化为 `ip:port#key`，QR 码即此串。

### 4.2 三种角色

**Coordinator**（`coordinator.{hpp,cpp}`，服务端）：监听 TCP，认证 worker，把客户端提交的图切块，按调度器指派分发，收集结果拼图。核心 `distribute_task()` 是"算 tile 数 → 分波派发 → 收齐 → 拼回"的多波循环（`coordinator.cpp:115-428`）。兼做模型分发服务器。

**Worker**（`worker.{hpp,cpp}`，工作端）：连到 coordinator，认证，**握手时本地 benchmark 测速并上报**，接收 tile 任务、用 Engine 推理、回传结果，周期心跳（5s），按需同步模型（带路径穿越校验）。

**Client**（无独立类，在 `coordinator.cpp` 的 `handle_client` 内联实现，也在 CLI `main.cpp:1733` 直接走 socket）：用 `MSG_CLIENT_SUBMIT` 把整图发给 coordinator，收 `MSG_CLIENT_RESPONSE` 二进制结果。

### 4.3 权重调度 —— `scheduler.{hpp,cpp}`

`Scheduler` 维护在线设备表，按各设备 `tiles_per_second` 归一化为权重：

```
weight[i] = tps[i] / Σ tps[i]     （否则 1/N 均分）
```

**权重数据来源（即 README 说的"首次小任务测速"）**：worker 在 `handshake` 时调 `engine.benchmark(256)`——对 256×256 哨兵 tile 跑 5 轮取均时（`ncnn_engine.cpp:2887`），返回 `tiles_per_second = 1000/avg_ms`——经 `DeviceProfileMessage` 上报给 coordinator，coordinator `register_device` 时归一化。

**任务分配** `assign_tiles()`（`scheduler.cpp:30-111`）：筛 alive（30s 内有心跳）→ 按权重降序排 → 每设备配额 = `ceil(total × weight)`，受 `max_batch_size` 封顶。失败重调度靠 coordinator 的**多波循环**——失败的 tile 下一波重分（`handle_device_failure` 的孤儿回收目前是空实现）。

### 4.4 网络与发现 —— `share/utils/`

| 模块 | 职责 | 状态 |
|------|------|------|
| `network.{hpp,cpp}` | POSIX socket 薄封装（`TCPSocket`/`TCPServer`）+ `crc32` + 本地 IP/auth-key 生成 | 完整 |
| `mdns.{hpp,cpp}` | 局域网服务发现。`MDNSBroadcaster` 用 avahi-publish-service 发 `_crossscale._tcp`（带 key 的 sha256 前 16 位），PC 服务端在用。原 C++ `MDNSDiscovery` stub 已在 `8c11549` 删除（PC 端不发现，只广播）；发现侧由 Android `MDNSDiscovery.kt`（原生 NsdManager）承担 | 完整（PC 广播 / Android 发现，职责分离） |
| `crypto.{hpp,cpp}` | sha256/md5/base64/uuid/random，用于 task_id/device_id 生成与 mDNS key hash | 完整 |

### 4.5 分布式调用链（典型 worker 模式）

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

## 5. 子系统三：前端

### 5.1 PC CLI（`cli/src/main.cpp`）

**子命令**：`process` / `server [start|info]` / `worker connect` / `client process` / `model list` / `benchmark`，支持隐式 process（`main.cpp:309-326`）。

**参数解析**：手写 if-else 循环 + `safe_parse_int/float`。注意 `cli/xmake.lua:5` 声明了 `cxxopts` 依赖但 **main.cpp 完全没用**（死依赖，可清理）。

**单机 process 调用链**（单文件模式，`main.cpp:1206-1396`）：

```
main → ModelManager.scan_models() 选模型
     → create_ncnn_engine() → initialize(GPU 优先) → load_model()
     → [auto-tile] TileSizeOptimizer.find_optimal_tile_size()
     → Image::load → normalize_channels_for_model
     → TileProcessor.process_image_u8(→ Engine 推理)
     → Image::save(quality, format)
```

目录模式（`main.cpp:377-1191`）更复杂：CPU `--jobs>1` 用 `EnginePool`；GPU 单图串行但有 prepare/process/save 三级流水线 + 自适应队列深度。

**build**：`cli/xmake.lua` 通过 `add_deps("crossscale-core")` 链接 share 静态库，`share/xmake.lua:5` 的 `add_includedirs(".", {public=true})` 让 `share/` 成为公共 include 根（故 `#include "core/engine.hpp"`）。

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

**Android 与 share 的关系**：`CMakeLists.txt:106-127` 把 share 的 11 个 `.cpp` **直接编译进 `libcrossscale-native`**，协议/worker/scheduler/网络全部与 PC 共用。唯一差异是 ncnn 引擎可插拔（§3.7）。

**build 关键项**：`compileSdk/targetSdk=36`，`minSdk=26`，JVM 21，NDK 29，CMake 4.1.2，C++17 -O3，ABI 三档（arm64-v8a/armeabi-v7a/x86_64），STL `c++_shared`。Release `isMinifyEnabled=true`，签名用环境变量无则回退 debug。

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
| 服务发现 | mDNS（PC 端 avahi-publish；Android NsdManager） |
| 安全 | EncryptedSharedPreferences（Android auth key）；明文 key 认证（协议层） |

### 依赖来源

- PC 端：`xmake require` 拉取 ncnn/libwebp/libpng/libjpeg/nlohmann_json/cxxopts/qr-code-generator-cpp
- Android 端：`apps/scripts/sync_native_deps.sh` 用 `xrepo install` 把 ncnn/glslang/libomp/zlib/libpng/libjpeg/libwebp 编进 `jniLibs/` + 头文件进 `cpp/third_party/`（这些是构建产物，不入 git）

---

## 7. 架构特点与值得注意之处

### 优点
1. **源码级跨平台复用**：share/ 一份代码，PC 和 Android 都直接编译，协议/调度/引擎逻辑强一致。
2. **引擎抽象 + 可插拔后端**：ncnn 缺失时 Android 仍能编译运行（stub），降低构建门槛。
3. **GPU 优化深入**：自写 Vulkan shader 做 HWC↔CHW、整图 u8 上传一次的 queue 流水线、alpha 旁路、batch 合并、互斥锁串行化单例状态——这些都是工程实战细节。
4. **分布式权重设计合理**：握手 benchmark 得 tps → 归一化 → 按比例切配额 → 多波重调度，简单有效。
5. **Android 健壮性投入大**：checkpoint 断点续传、队列持久化、原子写、Zip Slip 防护、payload 上限、双探针输出校验。

### 技术债 / 可改进
1. **`mdns` 发现侧职责分离**：PC 端只广播（`MDNSBroadcaster`，服务端在用），发现侧由 Android `MDNSDiscovery.kt`（原生 NsdManager）承担，PC 与 Android 不共享发现代码。原 C++ `MDNSDiscovery` stub 已在 `8c11549` 删除。
2. **CLI `cxxopts` 是死依赖**：声明了却完全没用，手写解析。要么用起来要么删掉。
3. **`Scheduler::handle_device_failure` 孤儿回收是空实现**，靠 coordinator 多波循环兜底，但单波内的部分失败可能延迟到下一波才重试。
4. **协议层无链路加密**，明文 key 认证，仅适合可信局域网。`crypto::xor_encrypt` 曾形同虚设（已在 `8c11549` 删除）。
5. **`MessageHandler.kt` 是占位**，未充分使用。
6. **`ProcessingViewModel` 2141 行偏重**，可考虑拆分。
7. **`share/xmake.lua` 的源文件列表是手写枚举**（`share/xmake.lua:7-24`），新增 `.cpp` 需手动同步；Android 的 CMake 也是手写枚举 11 个文件——两处列表需保持一致，易漂移。

---

## 8. 关键调用链速查

**单机超分（PC CLI 单图）**：
```
main.cpp → ModelManager.scan_models
        → create_ncnn_engine → Engine.initialize → Engine.load_model
        → [auto] TileSizeOptimizer.find_optimal_tile_size
        → Image::load → normalize_channels_for_model
        → TileProcessor.process_image_u8 → Engine.process[_batch] → blend_tile
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
