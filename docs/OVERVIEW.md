# CrossScale 项目概览

> 分布式图像超分辨率系统 — 跨平台、多设备协同推理

---

## 1. 项目定位

CrossScale 是一个**跨平台分布式图像超分辨率（Super-Resolution）系统**，具备以下核心能力：

- **单机超分**：使用 ncnn 推理引擎（CPU / Vulkan GPU）对图像进行基于神经网络的放大
- **分布式集群超分**：多台设备通过局域网组成计算集群，按各自算力权重分配 tile（瓦片）任务
- **跨平台**：PC（Linux/macOS/Windows 命令行） + Android 应用（Jetpack Compose UI）

---

## 2. 系统架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                        PC CLI (cli/)                            │
│  process | server | worker | client | model | benchmark | serve │
│  C++23, xmake build                                              │
└───────────────────────────┬─────────────────────────────────────┘
                            │ 链接静态库
┌───────────────────────────▼─────────────────────────────────────┐
│               share/  —  共享 C++ 核心层                         │
│  ┌─────────────────┐  ┌────────────────┐  ┌──────────────────┐  │
│  │  推理引擎子系统    │  │  分布式计算子系统 │  │  工具层           │  │
│  │  core/           │  │  core/distributed│  │  utils/          │  │
│  │                  │  │                  │  │                  │  │
│  │  Engine 抽象     │  │  Coordinator     │  │  图像 I/O         │  │
│  │  NCNNEngine 实现 │  │  Worker          │  │  网络 TCP         │  │
│  │  TileProcessor   │  │  Scheduler       │  │  日志 Logger      │  │
│  │  TileOptimizer   │  │  Protocol        │  │  密码/哈希        │  │
│  │  ModelManager    │  │  mDNS 发现       │  │  基准测试工具      │  │
│  │  EnginePool      │  │                  │  │  UnixSocket      │  │
│  └─────────────────┘  └────────────────┘  └──────────────────┘  │
└───────────────────────────┬─────────────────────────────────────┘
                            │ JNI 桥 / 直接编译进 .so
┌───────────────────────────▼─────────────────────────────────────┐
│                  Android App (apps/app/)                         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────────────┐  │
│  │ UI 层    │ │ ViewModel│ │ Data 层  │ │ 网络 / JNI 层      │  │
│  │ Compose  │ │ MVVM     │ │ 11 Repos │ │ CoordinatorClient  │  │
│  │ 6 屏幕   │ │ 3 VM     │ │ 持久化   │ │ NativeWorker (JNI) │  │
│  │ Material3│ │ StateFlow│ │ Checkpoint│ │ NCNNEngine (JNI)   │  │
│  └──────────┘ └──────────┘ └──────────┘ └────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. 模块结构速查

```
crossscale/
├── share/                     # 共享 C++ 核心（最重要的目录）
│   ├── core/                  # 推理引擎子系统
│   │   ├── engine.hpp         #   引擎虚基类接口
│   │   ├── ncnn_engine.cpp    #   ncnn 后端实现（3800+ 行）
│   │   ├── tile_processor.*   #   图像切块/推理/融合流水线
│   │   ├── tile_types.hpp     #   瓦片数据结构
│   │   ├── tile_optimizer.hpp #   header-only 瓦片大小优化器
│   │   ├── config.*           #   配置加载/校验/JSON持久化
│   │   ├── model_manager.*    #   模型扫描/解析/管理
│   │   ├── engine_pool.*      #   多引擎池（多线程推理）
│   │   ├── distributed/       #   分布式计算子系统
│   │   │   ├── protocol.*     #      自定义二进制协议
│   │   │   ├── coordinator.*  #      Coordinator（服务端）
│   │   │   ├── worker.*       #      Worker（工作端）
│   │   │   └── scheduler.*    #      权重调度器
│   │   └── serve/             #   本地常驻守护进程
│   │       └── daemon.*       #      预热引擎 UnixSocket 服务
│   ├── utils/                 # 工具层
│   │   ├── image.*            #   图像加载/保存/转换
│   │   ├── network.*          #   TCP Socket/Server 封装
│   │   ├── logger.hpp         #   日志系统（含 Android LogCat）
│   │   ├── crypto.*           #   哈希/UUID/加密
│   │   ├── benchmark.*        #   基准测试计时器
│   │   ├── mdns.*             #   mDNS 局域网广播
│   │   ├── unix_socket.*      #   Unix Domain Socket
│   │   └── expected.hpp       #   Result 类型（类似 Rust）
│   └── perf/                  # 性能测试框架
│       ├── test.hpp           #   测试场景基类
│       ├── runner.*           #   测试运行器
│       ├── probes.*           #   系统探针（GPU/util/温度）
│       ├── workload.*         #   合成负载生成
│       ├── reporter.*         #   指标收集+报告
│       └── tests/             #   具体测试场景
│
├── cli/                       # PC 命令行入口
│   └── src/main.cpp           #   所有子命令处理（~2400 行）
│
├── apps/                      # 应用工作区
│   ├── app/                   # Android 应用模块
│   │   ├── src/main/java/     #   Kotlin 源码
│   │   │   └── com/crossscale/
│   │   │       ├── ui/        #      Compose UI（6 屏）
│   │   │       ├── viewmodel/ #      MVVM ViewModel（3 个）
│   │   │       ├── data/      #      数据层（model + repository）
│   │   │       ├── network/   #      网络发现与连接管理
│   │   │       └── native/    #      JNI 接口声明
│   │   └── src/main/cpp/      #   JNI C++ 桥接层
│   │       └── jni_bridge.cpp #      将 share/ 暴露给 Kotlin
│   └── desktopApp/            #   （已移除/保留骨架）
│
├── models/                    # ncnn 模型文件 (.param + .bin)
├── scripts/                   # 运维脚本
└── docs/                      # 文档
```

---

## 4. 三大子系统

### 4.1 推理引擎子系统 (`share/core/`)

| 组件 | 文件 | 职责 |
|------|------|------|
| `Engine` 接口 | `engine.hpp` | 纯虚基类，定义推理后端契约 |
| `NCNNEngineImpl` | `ncnn_engine.cpp` | ncnn CPU/Vulkan GPU 实现，3800+ 行 |
| `TileProcessor` | `tile_processor.*` | 大图切块 → 逐块推理 → 加权融合拼回 |
| `TileSizeOptimizer` | `tile_optimizer.hpp` | header-only，实测候选 tile 大小选最优 |
| `ModelManager` | `model_manager.*` | 扫描 models/ 目录，解析 .param 元数据 |
| `EnginePool` | `engine_pool.*` | 多 Engine 实例池，支持并发推理 |
| `ProcessingConfig` | `config.*` | 配置聚合 + JSON 持久化 + 校验 |

**关键设计**：
- GPU 优先初始化，Vulkan 失败自动回退 CPU
- GPU tile 流水线支持三档能力（基础 batch → u8 流水线 → queue 批量提交）
- Vulkan 全局状态由互斥锁串行化，多线程需独立 Engine 实例
- alpha 通道双三次插值旁路，省 25% GPU 算力

### 4.2 分布式计算子系统 (`share/core/distributed/`)

| 组件 | 文件 | 职责 |
|------|------|------|
| `Protocol` | `protocol.*` | 15 字节定长帧头 + CRC32 二进制协议 |
| `Coordinator` | `coordinator.*` | 服务端：认证/调度/分发/拼图 |
| `Worker` | `worker.*` | 工作端：连 coordinator / 测速 / 推理 / 回传 |
| `Scheduler` | `scheduler.*` | 按 tps 权重分配 tile 到各设备 |

**运行角色**：
```
CLI server start  →  Coordinator（TCP 服务端）
CLI worker connect →  Worker（连接 Coordinator）
CLI client process →  通过 Coordinator 派发给 Workers 执行
Android App        →  既可作为 Worker 参与集群，也可作为 Client 提交任务
```

**工作流程**：
```
1. 认证：Worker → MSG_AUTH_REQUEST → Coordinator 验证 key → 返回 device_id
2. 测速：Worker → engine.benchmark(256) → MSG_DEVICE_PROFILE(tps)
3. 调度：Coordinator → Scheduler 归一化权重 → 按比例切分 tile
4. 执行：循环分波 → 每波派发 → Worker 推理 → 回传结果
5. 拼图：Coordinator 收齐所有 tile → stitch_tiles() → 返回 Client
```

### 4.3 前端子系统

| 前端 | 技术栈 | 职责 |
|------|--------|------|
| PC CLI | C++23, xmake | 所有操作入口（process/server/worker/client/model/serve/benchmark） |
| Android App | Kotlin, Jetpack Compose, Material 3, MVVM | 图形界面，手机端超分与集群参与 |

---

## 5. 构建系统

### PC CLI (xmake)

```
xmake require      # 安装依赖（ncnn/libpng/libjpeg/libwebp/glaze/qr-code-generator-cpp）
xmake              # 编译
xmake run          # 运行
```

依赖关系链：
```
xmake.lua (根)
├── includes share/    →  target crossscale-core (静态库)
│                        ├── core/*.cpp (引擎/tile/分布式/daemon)
│                        ├── utils/*.cpp (网络/图像/日志/加密)
│                        └── perf/*.cpp (性能测试框架)
├── includes share/perf → target crossscale-perf
└── includes cli/      →  target crossscale-cli (二进制)
                           └── deps: crossscale-core + crossscale-perf
```

### Android App (Gradle + CMake/NDK)

```
./gradlew assembleDebug     # 编译 Android APK
./gradlew :desktopApp:run   # 运行桌面版本
```

Android NDK 将 `share/core/` 的 11 个 `.cpp` **直接编译进 `libcrossscale-native.so`**，与 PC 端代码完全一致。

---

## 6. 关键技术决策

| 决策 | 选择 | 原因 |
|------|------|------|
| 跨平台共享 | 源码级（非库级） | PC 和 Android 用同一份 `.cpp` 编译，保证行为一致 |
| 推理后端 | ncnn (CPU + Vulkan) | 移动端友好，Vulkan 跨平台 GPU 加速 |
| 分布式协议 | 自定义二进制 (15B 帧头 + CRC32) | 轻量、低开销、无额外依赖 |
| 设备发现 | mDNS (avahi / NsdManager) | 局域网零配置，Android 原生支持 |
| 图像格式 | libpng / libjpeg / libwebp | 覆盖主流网络图片格式 |
| 权重分配 | handshake benchmark 实测 tps | 真实反映设备算力 |
| Android UI | Jetpack Compose + Material 3 | 现代 Android UI 框架 |
| 状态管理 | MVVM + StateFlow | 响应式，生命周期安全 |

---

## 7. 分布式协议帧格式

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 ├─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┤
 │        Magic (0x53534358)         │ V │   MessageType   │
 ├─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┤
 │        Payload Length             │        Checksum (CRC32)         │
 ├─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┤
 │                              Payload ...                            │
 └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘
```

消息类型分组：
- **0x00**: 握手/认证/心跳
- **0x01**: 设备管理/Profile
- **0x02**: 模型同步
- **0x03**: 任务分发/进度/结果
- **0x04**: 图像数据块
- **0x05**: Client 提交/响应
- **0x06**: 本地守护进程（Unix Socket）

---

## 8. 关键性能设计

- **Tile 流水线**：三段式（Load → Process → Save）用有界队列解耦，CPU 预处理与 GPU 推理重叠
- **GPU Submit Queue**：多图场景下批量提交 GPU 任务，自适应队列深度
- **Alpha 旁路**：RGBA 图像的 alpha 通道走双三次插值，不经过神经网络
- **GPU Shader**：自写 Vulkan Compute Shader 做 HWC↔CHW 转换，避免 CPU↔GPU 往返拷贝
- **Auto-tile**：对候选 tile 大小实测 + 内存约束 + 最优选择（5% 容忍窗口内取最大 tile）

---

## 9. 项目规模

| 维度 | 数据 |
|------|------|
| 源文件总数 | ~166 文件 |
| Kotlin | 55 文件（33%） |
| C++ (hpp/cpp) | 52 文件（31%） |
| 最大源文件 | `ncnn_engine.cpp` (~3867 行) |
| 最大 Kotlin 文件 | `ProcessingViewModel.kt` (~2141 行) |
| 共享核心代码 | `share/` ~40 文件 |

---

## 10. 开发指南关键点

1. **共享代码优先**：算法/协议变更必须在 `share/`，同时影响 PC 和 Android
2. **ncnn 引擎可插拔**：Android 提供 stub 实现，无 ncnn 也能编译
3. **分布式协议兼容**：修改 `protocol.hpp` 消息类型时需同步两端
4. **Android 模型**：模型从 assets 拷贝到内部存储，新增模型需更新拷贝逻辑
5. **两处文件列表需同步**：`share/xmake.lua` 和 `apps/CMakeLists.txt` 手写枚举共用的 `.cpp` 文件
