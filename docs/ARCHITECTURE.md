# Upix 架构

> 跨平台图像超分辨率系统：PC 端 CLI 与 Android 应用共享同一份 C++ 核心（`share/`），
> 通过局域网 TCP + 加密配对协议组成"手机提交、PC 计算"的卸载模式。
>
> 本文档是架构的**单一权威来源**，随代码滚动更新（最近全面核对：2026-10-13；
> 不再钉具体 commit——曾钉 `9920fc9` 后内容先行演化、戳记失真；同理正文也不钉
> 精确行数）。
> 与代码冲突时以代码为准，并请回来修文档。

**目录**：[0. 顶层布局](#0-顶层布局) · [1. 子系统划分](#1-子系统划分) · [2. 推理引擎子系统](#2-推理引擎子系统sharecore) · [3. 通信与配对子系统](#3-通信与配对子系统sharecoredistributed) · [4. serve daemon](#4-serve-daemonsharecoreservedaemonhppcpp) · [5. 前端子系统](#5-前端子系统) · [6. 构建系统](#6-构建系统) · [7. 技术栈与依赖](#7-技术栈与依赖) · [8. 架构特点与已知限制](#8-架构特点与已知限制) · [9. 关键调用链速查](#9-关键调用链速查) · [10. 相关文档](#10-相关文档)

---

## 0. 顶层布局

```
              ┌──────────── PC CLI (cli/, C++23) ────────────┐
              │  隐式 upscale / model / host / daemon /      │
              │  completions （perf 探针在 tools/upix-perf）  │
              └───────────────┬──────────────────────────────┘
                              │ 链接静态库
              ┌───────────────▼──────────────────────────────┐
              │          share/ (upix-core 静态库)            │
              │  core/     推理引擎 + tile 处理 + serve daemon │
              │  core/distributed/  加密配对协议 + coordinator │
              │  utils/    网络 / 图像 / 加密 / XMP 标记 / 日志 │
              │  perf/     模块化性能测试框架                  │
              └───────────────┬──────────────────────────────┘
                              │ 同一份源码编进 .so（xmake）
              ┌───────────────▼──────────────────────────────┐
              │      Android App (apps/, Kotlin + JNI)        │
              │  Compose UI (MVVM) ← jni_bridge → share/ 代码 │
              └──────────────────────────────────────────────┘
```

桌面端 `desktop/` 不链接 share/。它用 Qt Quick 开窗口，界面调用同一份 `upix-cli`（放大、模型、主机、热引擎）。

| 目录 | 角色 | 构建 | 标准 |
|------|------|------|------|
| `share/` | 共享 C++ 核心：推理引擎、tile 处理、加密配对协议、coordinator、serve daemon、网络/图像/加密工具、perf 框架 | xmake 静态库 `upix-core`（PC 与 Android 共用同一工程） | C++23（JNI 桥 C++17） |
| `cli/` | PC 命令行入口（`main.cpp` 薄入口 + `cli_app.cpp` 命令树 + `cmd_*.cpp` 每子命令一个） | xmake → `upix-cli` + 9 个测试二进制 | C++23 |
| `tools/` | 主机侧工具：`dump-spirv`（SPIR-V pack 生成器）+ `upix-perf`（独立性能探针二进制） | xmake | C++23 |
| `apps/` | Android 应用（Jetpack Compose + MVVM，单 Activity + 手写 DI） | Gradle + xmake（NDK），产物 `libupix-native.so` | Kotlin + C++17 JNI |
| `models/` | ncnn 模型目录（源码仓库只含 README；模型经 GitHub release 分发） | 运行时扫描 | — |
| `docs/`、`scripts/` | 文档与构建/测试/发版脚本 | — | — |

**核心设计原则：源码级共享。** Android 端不是链接预编译库，而是由 `apps/xmake.lua`
的 `upix-native` target 依赖 `upix-core`——与 PC 端完全相同的 share/ 源文件、同一个
xmake 工程、同一套编译选项——编进 `libupix-native.so`，保证两端引擎、协议、图像处理
逻辑强一致。两端构建均已统一到 xmake（Android 侧由 Gradle 任务 `xmakeNative` 驱动，
不存在 CMake）。

**模型不随二进制分发**：CLI 与 App 都从 [models release](https://github.com/xihale/upix/releases/tag/models)
按需下载（`upix model download` / App 模型页），或导入本地 `.param`/`.bin`。

### 0.1 Rust 实现（`rust-rewrite` 分支）

PC 侧正在整体迁到 Rust：`crates/` 是 cargo workspace，**唯一 C++ 岛**是引擎。

| crate | 角色 | 对应旧实现 |
|------|------|-----------|
| `upix-core` | tile 管线/优化器/引擎池/模型管理/配置 | `share/core/`（除引擎本体） |
| `upix-img` | png/jpeg/webp/avif(dlopen) 编解码、EXIF 旋正、SR marker | `share/utils/image*`、`sr_marker*` |
| `upix-net` | 帧协议、X25519/Ed25519/secretbox 会话、身份与信任库、TCP/UDS | `share/core/distributed/`、`share/utils/{network,crypto}*` |
| `upix-host` | coordinator（配对门/取消/分块提交/模型运维）+ 热引擎 daemon | `share/core/distributed/coordinator.cpp`、`share/core/serve/` |
| `upix-engine-ffi` | ncnn/Vulkan 引擎的 C ABI + 安全包装 | `share/core/ncnn_*`、`vulkan_shaders`、`spirv_pack` |
| `upix-cli` | clap 命令树、daemon 委托、completions | `cli/` |
| `upix-gui` | nsh 界面（**暂停**，workspace 排除） | `apps/`、`desktop/` 的一部分 |

**单一引擎源**：`share/core/` 的引擎源码不复制——xmake 与 cargo 各自原样编译
同一批文件。差别只有 SPIR-V pack：xmake 用 `share/core/spirv_pack.S`（`.incbin`），
cargo 构建定义 `UPIX_SPIRV_PACK_EXTERNAL` 由 Rust 侧运行时注入
（`crates/upix-engine-ffi/csrc/spirv_pack.bin`）。

**等价性是证据**：wire 协议/身份/帧布局由 C++ 实现导出的金标向量逐字节断言
（`crates/upix-net/tests/golden/`）；真实放大在 CPU、GPU、daemon 三条路径与 C++
CLI 输出逐像素一致（CPU vs GPU 自身的 ±222 差值来自 ncnn 后端，两侧一致）。

**过渡态**：GUI 暂缓期间，旧 C++ 树（`share/ cli/ apps/ desktop/`）保持可构建，
继续承载 Android 与桌面工作台；`share/` 中被 Rust 取代的子系统在 GUI 迁移完成后才删除。


---

## 1. 子系统划分

| 子系统 | 位置 | 职责 |
|--------|------|------|
| **推理引擎** | `share/core/`（非 distributed 部分） | 引擎抽象、ncnn 后端（CPU/Vulkan）、tile 切分与融合、tile 大小优化、模型管理、SPIR-V pack、GPU 精度 profile |
| **通信与配对** | `share/core/distributed/` + `share/utils/{network,crypto_sodium,stream_socket}` | 二进制帧协议、libsodium 加密握手、身份与信任库、coordinator（PC 服务端）、模型共享 |
| **前端** | `cli/`（PC）+ `apps/app/`（Android） | 用户交互、命令路由、生命周期、UI、JNI 桥 |

> **历史注记**：早期版本是多 worker 权重调度集群（worker 注册/心跳/tps 权重/mDNS 发现），
> 已整体删除简化为"手机↔PC 一对一配对"。协议里相关消息号一并移除，详见 git 历史。

---

## 2. 推理引擎子系统（`share/core/`）

### 2.1 引擎抽象 —— `engine.hpp`

`Engine` 纯虚接口定义推理后端契约：`initialize` / `list_devices` / `load_model` /
`process`（单 tile float）/ `process_u8` / `process_batch`（GPU 强制串行，CPU 用
`std::async` 并发）/ GPU tile 流水线入口 / `benchmark` / `get_recommended_tile_size`
（VRAM 感知推荐）/ `get_memory_info`。工厂 `create_ncnn_engine()` 供两端共用。

### 2.2 ncnn 后端（按职责拆分）

| 文件 | 职责 |
|------|------|
| `ncnn_engine_impl.hpp` | `NCNNEngineImpl` 类声明（PIMPL 外壳 + 工厂） |
| `ncnn_engine.cpp` | 类骨架：initialize/load_model/process/process_u8/process_batch/benchmark + CPU 路径 |
| `ncnn_gpu_init.cpp` | Vulkan 初始化 + GPU 枚举打分（discrete/integrated/fp16/queue）+ 重试 |
| `ncnn_gpu_pipeline.cpp` | GPU tile 流水线（最高档 `process_tiles_gpu_u8_queue`：整图 u8 一次上传，GPU 上 crop + 推理 + 量化 + 拼回），全子系统最大的单文件 |
| `vulkan_shaders.hpp` | 12 个 GLSL compute kernel 源码常量 + RAII guard |
| `groupnorm_layer.hpp` | 自研 GroupNorm 层（Vulkan + CPU 双路径），覆盖 ncnn 无 GPU 实现的缺口 |

**关键实现细节**：

- GPU 优先初始化，Vulkan 失败回退 CPU；GPU 实例创建时枚举所有 GPU 打分选最优。
- HWC↔CHW 转换、量化、tile 融合全部在 GPU 上用自写 Vulkan compute shader 完成，
  避免 CPU↔GPU 往返。
- **SSBO 小类型禁令（Adreno 规避）**：Adreno 驱动会静默杀死含标量 `float16`/`uint8`
  SSBO 成员的 kernel。所有自定义 shader 的 SSBO 一律声明为 `uint` 字，fp16 载荷用
  `packHalf2x16`/`unpackHalf2x16` 显式打包（仅 core GLSL，不用扩展）。
- **自研 GroupNorm 层（`groupnorm_layer.hpp`）**：上游 ncnn 的 GroupNorm 只有 CPU
  实现，GPU 图里每个 GN 强制一次设备往返 + 全队列排空（RealPLKSR 每瓦片 28 次，
  GPU 利用率掉到 ~30%）。引擎在创建 `ncnn::Net` 时用 `register_custom_layer`
  覆盖该类型：GPU 走 `kGroupNormComp`（elempack-1 u32 字格式，reduce+normalize
  两次 dispatch 共享一条 pipeline），CPU 路径逐位复刻内建实现。
- 全局互斥锁串行化 Vulkan 实例/模型加载（ncnn 的 Vulkan 状态是进程级单例）。
- RGBA 输入不进 4 通道网络：模型是 RGB（`out.c == 3`），把 alpha 一起送进流水线会让
  `process()` 的输出形状校验（`out.c == channels`）拒绝每个 tile。故按 stride 逐 tile 解交错，
  只有前 3 个通道进模型，alpha 由 `bicubic_scale_u8`（Mitchell-Netravali）在 host 侧放大后
  合并回输出第 4 通道；u8 / float 两条入口同一套语义。CLI、批处理和远程入口保留
  RGBA；RGB 模型处理 RGBA 时使用 host 侧分块与 alpha 合成，推理仍在所选设备运行，
  RGB 输入继续使用原 GPU 快速管线。

### 2.3 GPU 精度 profile —— `gpu_precision.hpp`

冻结的 Vulkan 编译 profile，两个预设：**Integrated**（手机/核显：fp16 只用于 packed
storage，数学全 fp32——Adreno/Mali 残差块会溢出 fp16 范围）与 **Discrete**（PC 独显：
额外开 fp16 算术）。两者都关闭 shader local memory / coop-matrix / subgroup。按设备名
识别 Adreno/Mali/PowerVR 自动选择；`UPIX_GPU_DISABLE_FP16=1` 可整体关闭 fp16 保精度。

### 2.4 SPIR-V pack —— `spirv_pack.{hpp,cpp}` + `spirv_pack.bin/S`

预编译 SPIR-V 容器（magic `CSpv`/`CSpz`，libdeflate 压缩），按 `(kind, id, 选项位)`
查表取模块，替代运行时 glslang 在线编译（Android 端不带 shader 编译器）。pack 未命中
时回退 `ncnn::compile_spirv_module` 在线编译（仅 host）。`tools/dump_spirv.cpp` 用
冻结 profile 离线生成 pack。`.bin` 经 `spirv_pack.S` 的 `.incbin` 嵌入二进制
（host + Android arm64）；构建钩子保证 pack 更新后自动 touch `.S`，避免陈旧 pack
静默发布（`.incbin` 依赖不被构建系统追踪，见 `share/xmake.lua` 的 `before_build`）。

### 2.5 Tile 处理 —— `tile_processor.{hpp,cpp}`

大图切块 → 逐块推理 → 加权融合拼回：

- `compute_tile_layout`：按 `step = tile_size - overlap` 网格化，边界 clamp，并把
  边界 tile 尺寸向安全类别对齐（规避 ncnn CPU 路径特定宽度下的静默垃圾输出）。
- **三段流水线**（Load 线程切块提取 → 主线程 Proc 调 `engine.process_batch` → Save
  线程融合），两个有界队列 + `condition_variable` 同步，CPU 提取/融合与 GPU 推理重叠。
- GPU 快速路径：引擎支持时整图直接交给 `process_tiles_gpu_u8_queue`。
- GPU chunk 提交/执行重叠（`ncnn_gpu_pipeline.cpp` u8 主路径）：ncnn 20260526 的
  VkCompute 仍只有 `submit_and_wait`（无 fence submit），故用一个专职提交线程承担阻塞
  等待 + 两个交替 VkCompute 双缓冲 chunk——CPU 录制 chunk N+1 时 chunk N 在 GPU 执行；
  chunk 的 tile 输入/输出/下载 staging 一律等 fence 信号后才回收，跨 chunk 的
  WAR/WAW 依赖 ncnn 每 SSBO binding 的 barrier（首个同步域覆盖先前提交）+ 单线程
  FIFO 提交保序。UPIX_PIPELINE_TIMER 显示 p2_submit 等待占比与总耗时双降
  （实测目录模式 SR 段 −11%）。
- `blend_tile`：重叠区二次衰减权重（`w*w`）+ 归一化，消除接缝。
- `extract_tile_u8`：逐 tile 转 float，避免整图 float 拷贝；源/目标通道数分开传，
  RGBA→RGB 时逐 tile 解交错丢掉 alpha（无需整图 RGB 副本）。

### 2.6 其余模块

| 模块 | 职责 |
|------|------|
| `tile_optimizer.hpp` | 自动选最优 tile：候选 {64…512} 实测 tiles/sec + 内存预算淘汰，在最小耗时 ×1.05 容忍带内挑最大 tile |
| `engine_pool.{hpp,cpp}` | N 个独立 Engine 实例 + 队列复用（CPU 目录模式 `--jobs` 并发用） |
| `model_manager.{hpp,cpp}` | 递归扫描模型目录，解析 `.param` 推断元数据（放大倍数从文件名 xN，输入通道数从首个卷积层反推） |
| `model_metadata.{hpp,cpp}` | 目录模型的场景标注表（动漫/照片/综合/修复…+ ★推荐），CLI/GUI 徽标数据源；Android 侧对照表在 `ModelScenes.kt` |
| `model_graph.{hpp,cpp}` | 纯 param 文件图分析：xN 文件名约定单一实现（`scale_from_filename`）+ 无网络加载的放大倍数图检测（Interp 级/反卷积/PixelShuffle 主干 BFS/头部 Reorg 除子）。引擎加载与 ModelManager 扫描共用，`test_model_catalog` 对抗 publish 脚本三方钉一致性 |
| `engine_setup.{hpp,cpp}` | `initialize_engine`（GPU/CPU/GPU_THEN_CPU/默认四策略，含回退）+ `switch_model` 懒加载换模（coordinator 与 daemon 共用） |
| `config.{hpp,cpp}` | `ProcessingConfig` 聚合，glaze JSON 持久化 + 校验 |

### 2.7 调用关系

```
上层调用方
   ├─ ModelManager.scan_models() → ModelInfo
   └─ TileProcessor.process_image[_u8](engine, config)
          ├─ [GPU 快速路径] engine.process_tiles_gpu_u8_queue()   ← spirv_pack 提供 kernel
          └─ [通用路径] 三段流水线 → engine.process_batch() → blend_tile
                          └─ CPU 多线程时 EnginePool.acquire/release

auto_tile_size=true 时：
   上层 → TileSizeOptimizer.find_optimal_tile_size(engine, model, config)
            └─ engine.process() 多次 benchmark
```

GPU 快速路径失败后尝试通用路径；它仍使用当前引擎，GPU 引擎不会因此重新加载为
CPU 引擎。OOM 缩小 tile 后仍可能失败，不能把该分支视为保证成功的 CPU 回退。

---

## 3. 通信与配对子系统（`share/core/distributed/`）

### 3.1 二进制帧协议 —— `protocol.{hpp,cpp}`

**定长 14 字节小端帧头**：`magic[4]`（`0x53534358`，crossscale 时代遗留值）+
`type[2]` + `payload_length[4]` + `checksum[4]`（CRC32）。加密帧外层 CRC 置 0
（明文 CRC 会泄露信息，完整性由 Poly1305 承担）。

**消息类型**（`MessageType`）按功能分组：

| 组 | 消息 | 说明 |
|----|------|------|
| 加密握手 | `KEY_HELLO 0x0007`（明文）/ `AUTH_PROOF 0x0008` / `AUTH_RESPONSE 0x0004` | X25519+Ed25519 握手，之后全帧加密 |
| 任务提交 | `CLIENT_SUBMIT 0x0500` / `CLIENT_RESPONSE 0x0501` / `TASK_PROGRESS 0x0502` / `CLIENT_DATA 0x0503` / `TASK_CANCEL 0x0504` | 手机→PC 提交整图（会话内可多次，双格式：单帧 struct+数据 / struct 帧 + CLIENT_DATA ≤8MB 续帧，后者供 >4MB 大图削峰）；TASK_PROGRESS 为 PC→手机流式进度（兼防 NAT 空闲踢线），手机端透传进 UI；CLIENT_DATA 双向复用——PC→手机为 >100MB 结果续帧，手机→PC 为分块上传续帧；TASK_CANCEL（手机→PC，空载荷）中止当前在途任务——PC 在下一个 tile 边界停算并以 success=0/error="canceled" 应答，会话保持可用（响应已发出后才到达的迟到 CANCEL 被容忍忽略） |
| 配对 | `CLIENT_CONNECT_REQUEST 0x0700` / `CLIENT_CONNECT_RESPONSE 0x0701` | 批准位 + 信任位 + AVIF 能力位 |
| 共享模型库 | `MODEL_LIST/UPLOAD/DOWNLOAD 0x0510-0x0515` | 手机浏览/上传/拉取 PC 上的模型 |
| 本地 daemon | `SERVE_REQUEST/RESPONSE/DATA/PROGRESS 0x0601-0x0604` | Unix domain socket，独立号段 |

单帧 payload 上限 100MB（`MAX_NETWORK_PAYLOAD`）。

### 3.2 加密与身份（libsodium）

| 模块 | 职责 |
|------|------|
| `share/utils/crypto_sodium.{hpp,cpp}` | X25519 ECDH（共享点经 BLAKE2b + domain separator 硬化成 32B 会话密钥）、Ed25519 签名/验签、`crypto_secretbox`（XSalsa20-Poly1305）seal/open |
| `identity.{hpp,cpp}` | 持久 Ed25519 身份（`~/.upix/identity`，0600；HOME 未设时回退 `%USERPROFILE%`，见 `utils/platform.hpp`）；`TrustStore` 白名单（`~/.upix/trusted_clients`，每行一个 base64 公钥）；`fingerprint()` = sha256(pubkey) 前 8 字节大写 hex |
| `crypto_session.{hpp,cpp}` | `EncryptedSession` 封帧/解帧：wire = `[14B 头][nonce 24B][密文+MAC 16B]` |
| `share/utils/network.{hpp,cpp}` | `TCPSocket`/`TCPServer`（基于 `StreamSocket` fd 基类 + I/O 锁防并发半发半收）、crc32（zlib）、本机 IP 枚举 |

**握手流程**（coordinator.cpp `run_handshake`）：

```
双方互发明文 KEY_HELLO（eph_pub32 + id_pub32 + sig64）
  → 组 transcript（双方 eph/id 公钥拼接，128B）
  → 服务端 Ed25519 签 transcript 放回 KEY_HELLO（防中间人替换握手参数）
  → X25519 派生并激活会话密钥
  → 客户端在加密帧内发 AUTH_PROOF（自己的 Ed25519 签名）
  → 服务端验签 → 查 TrustStore（只查询既有批准记录，握手不新增信任）
```

**配对批准**：`CONNECT_REQUEST` 走配对门——可信 peer 自动批准；首次 peer 在 PC 侧
阻塞等待交互批准（stdin `[y/n]`，`--auto-approve` 跳过，供 CI）。手机端首次连接会弹
指纹确认对话框（`FingerprintConfirmDialog`），严格模式下可钉死预期公钥防 MITM。
所有客户端都必须先完成 `CONNECT_REQUEST`，任务和模型消息才能进入业务分发；只有明确批准才写入 TrustStore，
拒绝后的重连仍需审批。`stop()` 先拒绝队列中及已交给 UI 的未决审批，再回收连接线程；迟到的审批回调无效。

**能力协商**：`ClientConnectResponse` 携带 `avif_decode`/`avif_encode` 位（PC 探测
系统 libavif 填充），手机据此门控 AVIF 输入/输出，而不是假设 PC 一定支持。

### 3.3 Coordinator（PC 服务端）

`coordinator.{hpp,cpp}`（骨架+路由）+ `coordinator_admin.cpp`（模型目录设置）。
监听 TCP，完成上述握手与配对，然后：

连接数上限为 8，握手接收超时 10s；连接处理线程捕获协议/密钥派生异常，关闭该
连接而不终止服务。stop() 先 shutdown 活跃 socket，再 join handler，避免静默 peer
使退出永久阻塞。该超时是 socket I/O 超时，不是整个分块上传的总期限。

- 接收 `MSG_CLIENT_SUBMIT`（请求头 + 压缩的图像文件字节，原始像素不上无线电）。
  双格式兼容：submit 帧 payload = sizeof(struct)+数据 即旧式单帧；=
  sizeof(struct) 即新式分块——按 struct 声明的 `input_data_length` **预分配**
  总长缓冲，随后逐个解开 ≤8MB 的 `MSG_CLIENT_DATA` 续帧原位拼接，wire 与
  明文的全量拷贝永不同时存在（峰值 ≈ 总长 + 2×8MB）。分块流被非 DATA 帧
  打断时以 per-task 错误应答（"chunked submit interrupted"），会话保持可用，
  旧手机可无缝续用；
- **EXIF 竖拍自动旋转**：JPEG 解码后按 EXIF orientation（`utils/exif`，1-8）把旋转烘焙进
  像素再送推理（等同 PIL exif_transpose / libvips autorotate），与手机本地路径行为一致；
  无 EXIF / 非 JPEG 输入零行为变化（仅一次头部扫描）；
- **远程 tile 尺寸 0=auto**：手机默认提交 `tile_size=0`，PC 在 `process_locally` 里用
  `TileSizeOptimizer` 实测选优（探针按 model+设备+尺寸桶进程级缓存，每模型只付一次
  ~秒级测量；CLI 的 `--auto-tile` 同一缓存），失败兜底 `get_recommended_tile_size`；
  手机端三处 min 约束（Kotlin/jni/协议）均放行 0 且仅 0 例外；
- 用本地引擎跑完整超分（`engine_setup` 初始化 + `TileProcessor.process_image_u8`）；
- 处理中持续回发 `MSG_TASK_PROGRESS`，完成后回 `MSG_CLIENT_RESPONSE`（+ 必要时
  `MSG_CLIENT_DATA` 续帧）；编解码失败以 per-task 错误回传，不摔连接。
  进度帧不由 worker 直接发送：worker 只把最新 {pct,eta} 写进
  `ClientTaskRun` 原子槽，handler 轮询线程每 tick 至多发一帧——直接发送曾与
  轮询读在共享 io_mutex 上锁车队化，实测每帧 ~10s、推理放大 1900×；
  （io_mutex 现已拆分为收/发两把锁，见 上游"StreamSocket 拆分收发两把锁"提交 StreamSocket）
- **任务级取消**：每个 CLIENT_SUBMIT 的解码/推理/编码/应答跑在该连接自己的工作
  线程上，handler 线程以 ~200ms 短超时轮询继续收帧（`recv_encrypted_poll`：仅对
  首字节限时，帧一旦开始就用整段超时收完，轮询绝不弃半帧；收向超时经
  `TCPSocket::set_recv_timeout` 单独调小，发送超时不受影响）。收到
  `MSG_TASK_CANCEL`、轮询发现连接死亡、进度帧发送失败或 coordinator stop() 时置
  每任务 `std::atomic<bool>` cancel——`process_locally` 在入口/引擎锁后/编码前
  检查，`TileProcessor` 逐 tile 检查，取消代价 ≤ 1 个 tile 的白算，任务以
  success=0/"canceled" 应答。`MSG_TASK_CANCEL` 取消当前会话全部在途任务，与手机的
  批次取消语义一致；其他连接不受影响。工作线程由 handler 在返回前 join，stop() 沿
  client_threads_ → handler → worker 链路确定性回收；
- **会话循环**：响应后连接保持打开，继续收下一张 `CLIENT_SUBMIT`（帧间空闲超时
  60s）。握手/配对每连接只发生一次，整批任务复用同一条 TCP；旧版一次性客户端
  发完即关，循环读到 EOF 自然退出。推理互斥仅覆盖 `process_locally` 的推理段，
  输出编码在锁外进行；
- 顺带响应模型共享请求（列出/接收/下发模型，仍为一次性操作）。

### 3.4 客户端侧（无独立 CLI 子命令）

分布式提交由 Android app 经 JNI 发起（见 §5.2）。**图像批处理走持久会话**：
`nativeOpenDistributedSession`（TCP 连接 + 握手 + 配对，每批一次）→ 每张图
`nativeSendDistributedTask`（上传）+ `nativeReceiveDistributedResult`（收结果，
TASK_PROGRESS 经回调透传进 UI）→ 批末 `nativeCloseDistributedSession`。
批内为 submit-ahead 流水线，深度由 PC 在 CONNECT_RESPONSE 的
`pipeline_depth` 协商（当前固定 2：一张在算 + 一张完整在途在网线上，
见 `coordinator.cpp` 与 `jni_bridge.cpp` 的 `g_peer_pipeline_depth`）：
上传 N+1 与 PC 推理 N 重叠、手机写盘/SAF 提交 N 与 PC 解码/推理 N+1 重叠，
下一张原图字节在等结果时后台预取。旧 PC 不带该字段时深度退化为 0（串行）。模型共享等其他操作仍为
一次性独立 TCP 连接（握手 → 配对 → 单操作帧 → 收结果 → 关连接）。
超时：连接 10s、I/O 60s。传输失败自动重开会话重发一次（容忍 PC 重启）。

**大图分块上传**：`nativeSendDistributedTask` 对 >4MB 输入改发 struct 帧 + N 个
≤8MB `MSG_CLIENT_DATA` 续帧（`allowChunked=false` 强制旧式单帧）。不再构造
整份 plaintext/wire 全量拷贝——提交侧峰值从 ~2×输入 降到 ~8MB。**版本回退**：
旧 PC 不认识分块（摔线或回 "truncated" 类 per-task 错误）时，`awaitResult`
自动降级为单帧重发一次（仅当尺寸 ≤100MB 帧上限）：per-task 错误 → 同会话重发；
摔线 → 重开会话重发（端点保留）；并记住该端点不支持分块，后续大图直发单帧。

**取消**：`ProcessingViewModel.cancelProcessing` 取消协程之外，还会经
`CoordinatorClient.cancelRemoteTask` → `nativeCancelDistributedTask` 置会话级
cancel 原子（线程安全、非阻塞）。阻塞中的 `nativeReceiveDistributedResult` 以
~200ms 轮询接收（首字节限时、整帧收完不弃半帧），发现置位后**由接收线程自己**
发出 `MSG_TASK_CANCEL`——轮询接收在间隙释放 rx 锁，自己发帧无竞争
（`StreamSocket` 已拆分 tx/rx 两把锁；旧的单 io_mutex 会让发送排队等满一个
200ms 轮询窗，曾被测出响应下发 11-23s/546KB）。随后继续收 PC 的
"canceled" 终止应答（上限 90s；旧 PC 不认识 CANCEL 会摔线，报
"Distributed task canceled (connection lost)"），Kotlin 侧将取消映射为
`CancellationException`（不算失败、不清已配对端点），批次 finally 关会话收尾。

---

## 4. serve daemon（`share/core/serve/daemon.{hpp,cpp}`）

`upix daemon` 是**本地常驻守护进程**（仅 Linux/macOS，AF_UNIX）：持有一份已初始化 +
已加载模型的引擎（"热引擎"），把单图调用的 ~490ms 固定开销（Vulkan init + 模型加载）
摊薄到接近零（单图 ~0.85s → ~0.12s，小图最高 ~15×）。

- 生命周期全自动：CLI 单图路径发现无 daemon 时自动后台拉起，空闲超时自动退出
  （CLI 默认 600s，`--idle-timeout` 调整，`0` 常驻）；`--no-daemon` 跳过委托。
  拉起失败静默回退内联路径。
- 复用 `Protocol` 帧格式与 0x0601-0x0604 号段，但**不加密**（本机 Unix socket，
  图像字节不过 socket——只传路径与参数）。明文帧的头部 checksum 双向强制校验
  （`Protocol::verify_frame`：不匹配即拒收应答），加密帧不带 checksum（防明文
  CRC 泄密，完整性由 Poly1305 承担）。socket 文件 chmod 0600，信任边界=同用户。
- 每连接一个 jthread，引擎访问互斥串行（Vulkan 单例硬约束）。

与 coordinator 职责正交：daemon 解决"单图固定开销"（无状态 1 连接 1 请求，持引擎，
本地）；coordinator 解决"跨设备卸载"（有状态，配对/信任/任务循环，不持引擎，局域网
TCP）。二者共享的是协议帧格式与 TileProcessor。

---

## 5. 前端子系统

### 5.1 PC CLI（`cli/`）

**子命令**：隐式 upscale（无动词，`input`/`-i`/`-o`/`-m`/`--gpu`/`--jobs`/`-f` 等
全在根级）/ `model {list|list-remote|download|import}` / `host {start|info}` /
`daemon {start|stop|status}` / `completions <bash|zsh|fish>`（help 中隐藏）/
隐藏动词 `__complete`（动态补全后端）。性能探针在独立二进制 `upix-perf`。

**机器可读输出**：GUI/脚本消费面统一为「每 `'\n'` 行一个紧凑 JSON 对象、人读日志走
stderr」。两档：`--json`（version/model/daemon/host 输出单份文档；host start 输出
`card`/`pairing`/`task` 事件流）与 `UPIX_PROGRESS=json`（单图/目录进度事件
`progress`/`saved`/`current`/`note`）。装配在 `common.{hpp,cpp}`
（`json_escape`/`emit_json_line`/`report_progress`/`build_connection_card`），
不开启时人读输出逐字节不变。桌面端 `desktop/src/machine.cpp` 是对偶解码器。

**结构**：

| 文件 | 职责 |
|------|------|
| `main.cpp` | 薄入口：拦截 `__complete`，其余交 CLI11 树 dispatch |
| `cli_app.cpp` | CLI11 命令树（所有子命令的选项/校验/回调） |
| `args.hpp` / `common.{hpp,cpp}` | 参数 struct；模型目录解析（`$UPIX_MODELS_DIR` > `$XDG_DATA_HOME/upix/models` > `~/.local/share/upix/models`，HOME 未设回退 `%USERPROFILE%`）、输出命名、日志装配等共用逻辑 |
| `cmd_process.cpp` | 单图超分（daemon 委托 + 内联路径） |
| `process_batch.cpp` | 目录模式批处理（CPU EnginePool / GPU 多引擎 prepare→SR→save 流水线） |
| `cmd_model.cpp` + `model_downloader.{hpp,cpp}` | 模型列表/导入 + release 清单解析与下载（libcurl） |
| `cmd_host.cpp` / `cmd_daemon.cpp` | 连接卡（IP/QR/指纹）+ coordinator 生命周期；daemon 控制 |
| `cmd_complete.cpp` / `cmd_completions.cpp` | cobra 风格动态补全引擎；三 shell 薄适配脚本输出/安装 |

**有效处理配置**：默认值 → JSON 配置 → 显式 CLI 选项，由 `resolve_processing_config` 合并一次，
单图和目录的 CPU/GPU 路径消费同一份有效配置。无配置且未指定分块大小时才使用引擎推荐值。
daemon 协议不携带 tile batch size，因此有效 `batch_size != 1` 或开启自动分块时走内联执行。

**CLI 模型安装**：下载、校验及精确 `.param`/`.bin` 配对验证均在独立暂存目录完成，再备份并替换现有文件。
可处理的提交失败会回滚，回滚失败保留并报告恢复目录；平铺两个文件的替换不保证断电原子性。

**输出标记（SR XMP）**：所有输出图嵌入 XMP 标记 `urn:upix:sr:1`（`share/utils/sr_marker`，
PNG iTXt / JPEG APP1 / WebP XMP chunk / AVIF mdat 四格式同一份探测代码）。CLI/Android
回喂带标记的图默认跳过（防止反复放大），`-f`/`--force` 强制重处理。

**PC 端解码 autorotate**：单图/目录/daemon/远程四条 PC 解码路径统一在解码后按 JPEG EXIF
orientation（`share/utils/exif`）烘焙旋转再送推理；输出从像素重编码，天然不带 EXIF（拍摄
日期/GPS 等元数据随输出丢失是既有行为）。

**单机 process 调用链**：
```
main → cmd_process → ModelManager.scan_models() 选模型
     → create_ncnn_engine() → initialize(GPU 优先) → load_model()
     → [auto-tile] TileSizeOptimizer.find_optimal_tile_size()
     → Image::load → EXIF autorotate → normalize_channels_for_model
     → TileProcessor.process_image_u8 → Engine 推理 → blend_tile
     → sr_marker 注入 → Image::save(quality, format, EncoderOptions)
```

### 5.2 Android 应用（`apps/app/`）

**架构：Jetpack Compose（Material 3 Expressive）+ MVVM + 单 Activity + 手写 DI。**

```
io/upix/
├── UpixApplication.kt        持有 AppContainer（手工 DI，实例化所有 repository）
├── MainActivity.kt           唯一 Activity（edge-to-edge）
├── UpixApp.kt                顶层 Composable + NavHost 宿主
├── ui/                       screens(含 home/) · components · navigation · theme · scanner · input
├── viewmodel/                Main / Device / Models + Processing 五件套
│                              （ViewModel + Mappers + UiModels + QueueController + RunEngine + RunPlanner）
├── data/                     model/（数据类）· repository/（仓库层：本地推理/分布式/队列/checkpoint/模型/设备/日志等）· thumbnails/
├── service/                  ProcessingKeepAliveService（前台保活 FGS dataSync）
│                              + ProcessingForegroundCoordinator（进度共享总线，AppContainer 单例）
├── network/                  CoordinatorClient（调 JNI）· JoinInvite（连接卡解析）
├── nativelib/                NCNNEngine.kt（单例 object：本地推理+分布式提交）
│                              NativeWorker.kt（实例类：连接/模型共享等长驻会话操作）
└── util/                     StringProvider · UriUtils
```

**关键事实**：Android 端的**真正网络通信在 C++ JNI 层完成**（`jni_bridge.cpp`：
加密握手、配对、提交、进度循环全部在 native）。Kotlin `network/` 包只做封装。
手机是**提交方不是算力节点**——本地超分用本机引擎（GPU Vulkan / CPU），远程卸载把
图发给 PC。

**边界规则（2026-09-15 架构评审立）**：jni_bridge 是 Android 客户端专用胶水，
可复用原语（协议帧/加密会话/socket/引擎/TileProcessor）都在 share/ 与 PC 共享；
会话**策略**（流水深度/分块上传/降级重试/取消轮询）目前在 native，但只有 Kotlin
一个消费者——**新会话策略一律加在 Kotlin 侧消费既有导出，不再进 native**；若将来
出现第二个 C++ 客户端（如 PC CLI 客户端），先把客户端会话提取到
`share/core/distributed/` 再接，不许让 jni_bridge 长成第二份实现。

**批处理健壮性**：队列持久化、checkpoint 断点续传、failed-only 重试、备份恢复、
OOM 预检与降级、原子写、Zip Slip 防护、payload 上限、双探针输出校验、
前台保活+通知（`service/`：处理期间前台服务 dataSync + PARTIAL_WAKE_LOCK——锁屏/
切后台进程不被冻结、WiFi 传输不中断；前台通知低频显示批次进度与 PC 推理百分比，
批次终态各发一条完成/失败/取消通知；批次间不重启服务，active=false 或用户划掉
最近任务时自停并释放 WakeLock。服务不承载处理逻辑——run 仍在 viewModelScope，
经 ProcessingForegroundCoordinator 共享 StateFlow 单向驱动）。相册多选合并单批次：
系统相册 N 张图经 `InputSourceSpec.encode` 编成 `upix://multi:` 单源一次入队
（一次规划/一份 checkpoint/一行队列；解析、输出目录与断点续传语义同文件夹批次，
`source` 为普通字符串字段，旧单 URI 队列文件格式不变）。

checkpoint 的 `taskId` 标识一次运行，nullable `queueItemId` 标识它关联的队列行；两者不互相推导。
手动恢复只有在该队列行仍存在且运行选项匹配时才更新原行；旧 checkpoint 缺少关联字段时作为独立任务恢复。
队列与 checkpoint 仓库串行化读写和清除，通过 `AtomicSnapshotFile` 的暂存、备份与原子替换保留上一份有效快照，
中断写入后优先读取已提交主文件，主文件缺失时恢复备份；磁盘提交成功后才发布仓库 StateFlow。

**Android 本地超分调用链**：
```
UI → ProcessingViewModel → NativeImageProcessingRepository
   → NCNNEngine.nativeProcessImage (JNI)
   → jni_bridge.cpp → share/core 的 Engine + TileProcessor（与 PC 同一份代码）
```

---

## 6. 构建系统

### 6.1 xmake 工程结构

根 `xmake.lua` 是唯一工程：`set_project("upix")`、`set_version()` 是 **CLI
版本真源**（与 `apps/app/build.gradle.kts` 的 Android `versionName` 独立），
C++23。option：`native_isa`（默认开）、`upx`（默认关）、`avif-encoder` /
`avif-decoder`（默认关，见下）。ncnn 钉 `20260526` runtime 变体（不带在线编译器）。

| target | 定义于 | 说明 |
|--------|--------|------|
| `upix-core` | `share/xmake.lua` | 静态库，glob 收集 `core/*.cpp`、`core/distributed/*.cpp`、`utils/*.cpp`（unix_socket/android_utils 排除）；serve/ 与 unix_socket 仅 Linux/macOS；`spirv_pack.S` 仅 host + arm64；Linux 链 `dl` |
| `upix-cli` | `cli/xmake.lua` | 二进制；`on_load` 注入 `UPIX_VERSION="v<版本><能力位>"`（project 模块仅脚本域可用） |
| `upix-native` | `apps/xmake.lua` | Android shared；编 `jni_bridge.cpp` + 链 `upix-core`；`after_build` 把 .so 及依赖包 .so 拷入 `app/src/main/jniLibs/<abi>/`（含 NDK `libc++_shared.so`） |
| `upix-perf` | `tools/xmake.lua` | 独立探针二进制（target 名 `upix-perf-tool`，basename 还原为 `upix-perf`），链接 `share/perf/` 的 `upix-perf` 静态库 |
| `dump-spirv` | `tools/xmake.lua` | host-only，链 glslang 变体 ncnn，离线生成 `spirv_pack.bin`；默认不构建 |
| `test_*` ×9 | `cli/xmake.lua` | test_crypto / test_protocol / test_distributed_session / test_image_avif / test_sr_marker / test_exif / test_gpu_precision / test_model_downloader / test_model_catalog |

**SPIR-V pack 生成链**：`share/xmake.lua` 的 `before_build` 钩子（唯一钩子——多个
before_build 注册只有第一个生效）负责两件事：Android 配置下 pack 缺失时，用隔离的
`XMAKE_CONFIGDIR=build/host-dump-config` 起一套 host 配置跑 dump-spirv（不覆盖进行中
的 android 配置）；pack 比 `.S` 新时自动 touch `.S`（`.incbin` 不追踪 blob 依赖，
不 touch 会静默发陈旧 pack）。

### 6.2 AVIF 可选编解码

默认 lean 构建：CLI 运行时 dlopen 系统 libavif；Android 无设备端 AVIF 编码。两个
独立编译期选项打包真实编解码器（libavif+aom 编 / libavif+dav1d 解），定义
`UPIX_WITH_AVIF_ENCODER/DECODER` 宏切换 `share/utils/image_avif.cpp` 的链接分支。
**xmake option 跨 `xmake f` 粘连**，所以一切入口（`scripts/build_cli.sh`、
`scripts/build_native.sh`、Gradle）总是显式传 `--avif-*=y/n`，以最后显式声明为准。

### 6.3 Android Gradle 接线

- `apps/app/build.gradle.kts` 注册 Exec 任务 `xmakeNative`，挂在
  `merge{Debug,Release}JniLibFolders` 上（打包期，**不是** `preDebugBuild`——否则
  `testDebugUnitTest` 会拖一整条 NDK/xmake/ncnn 编译）。调用
  `apps/scripts/build_native.sh <mode> arm64-v8a --avif-encoder=? --avif-decoder=?`，
  AVIF 状态由 gradle 属性 `-Pupix.avifEncoder=true` / `-Pupix.avifDecoder=true`
  决定——**属性是唯一事实来源**，避免 xmake option 粘连。任务声明了 share/cpp
  输入和 `jniLibs/arm64-v8a` 输出，本地增量构建可跳过未变的 native。
- 版本钉死在 `apps/gradle/libs.versions.toml`：AGP 9.3.0、Kotlin 2.4.10、Gradle
  9.7.1、compileSdk/targetSdk 37、minSdk 26、Compose BOM 2026.08.00、material3
  显式钉 `1.5.0-alpha26`（Expressive API 所需）、adaptive 1.2.0、NDK 29.0.14206865。
- release 构建强制独立签名（`UPIX_RELEASE_*` 环境变量；缺失即拒绝，除非
  `-Pupix.allowDebugSignedRelease`）。
- 镜像：`apps/settings.gradle.kts` 先列阿里云镜像。

---

## 7. 技术栈与依赖

| 层 | 技术 |
|----|------|
| 推理 | ncnn 20260526（Vulkan），自写 GLSL compute kernel（预编译 SPIR-V pack） |
| 图像 | libpng / libjpeg / libwebp / libdeflate /（可选）libavif+aom/dav1d |
| 构建 | xmake（双端 C++）+ Gradle 9.7.1 / AGP 9.3.0（Android 壳） |
| Android UI | Jetpack Compose（BOM 2026.08）+ Material 3 Expressive（1.5.0-alpha26）+ adaptive 1.2 |
| CLI 解析 | CLI11 |
| 序列化 | glaze（config JSON / release manifest） |
| 加密 | libsodium（X25519 / Ed25519 / XSalsa20-Poly1305） |
| 二维码 | qr-code-generator-cpp（PC 生成）/ CameraX + zxing（Android 扫描） |
| 下载 | libcurl + openssl + libzip（模型分发） |
| 协议 | 自定义二进制（14B 帧头 + CRC32 / Poly1305） |

依赖统一经 `xmake require`（`add_requires`）拉取；Android 侧由 `apps/xmake.lua` 引用
同一批包，产物 `.so` 由 after_build 拷入 jniLibs（构建产物，不入 git）。

---

## 8. 架构特点与已知限制

### 特点

1. **源码级跨平台复用**：share/ 一份代码、一个 xmake 工程编两端，引擎/协议/图像逻辑
   强一致，双端字节级验证过（GPU 管线）。
2. **GPU 优化深入**：预编译 SPIR-V、整图一次上传的 queue 流水线、alpha 旁路、batch
   合并、按设备类冻结的精度 profile、Adreno SSBO 规避。
3. **安全配对**：ECDH + 双向 Ed25519 + transcript 签名 + TOFU 信任库 + 指纹确认 +
   交互批准，无需预共享密码。
4. **Android 健壮性**：checkpoint 断点续传、队列持久化、OOM 预检降级、原子写等。
5. **daemon 热引擎**：单图 2-15× 加速，全自动生命周期。

### 已知限制

1. **Vulkan 进程级单例**：同一进程内引擎/模型加载互斥串行（ncnn 约束，推理本身可多
   引擎并发）；GPU 目录模式按 VRAM 准入最多 4 引擎，prepare→SR→save 三段流水线，
   解码/编码不占引擎线程。
2. **serve daemon 不加密**：本机 AF_UNIX，无 EncryptedSession；仅 Linux/macOS。
3. **一对一配对**：一个 coordinator 服务一台手机的会话模型；多手机同时提交各自独立
   连接可行，但没有多机协同调度（历史 worker 集群已删，见 §1 注记）。
4. **Android AVIF 编码是纯软件**（无手机 SoC 带 AV1 硬编码器），大图慢且吃内存
   （50MP 峰值 ~1GB），默认建议走 PC 或 WebP。

---

## 9. 关键调用链速查

**单机超分（PC 单图，daemon 不在）**：见 §5.1 末尾。

**单机超分（PC 单图，daemon 在）**：
```
cmd_process → 探测 daemon → 连 Unix socket
  → send SERVE_REQUEST[路径+参数] → daemon: process_image_u8 → recv SERVE_RESPONSE → 存盘
```

**远程卸载（Android → PC）**：
```
UI → NativeImageProcessingRepository → CoordinatorClient（持久会话 + 协商深度 submit-ahead 流水线，PC 报 pipeline_depth=2）
  → nativeOpenDistributedSession (JNI: TCP + KEY_HELLO ×2 + AUTH + CONNECT 配对，每批一次)
  → 每张图: nativeSendDistributedTask（CLIENT_SUBMIT 上传；>4MB 为 struct 帧
            + ≤8MB CLIENT_DATA 续帧，旧 PC 拒绝时自动降级单帧重发一次）
            nativeReceiveDistributedResult（收 TASK_PROGRESS* → 回调进 UI → CLIENT_RESPONSE(+DATA*)）
            [上传 N+1 → 写盘 N，与 PC 处理重叠]
  → 批末 nativeCloseDistributedSession
取消: cancelProcessing → job.cancel + cancelRemoteTask → nativeCancelDistributedTask(置原子)
  → 接收轮询发现 → 接收线程发 TASK_CANCEL → PC 工作线程停算(tile 边界) → "canceled" 应答
  → awaitResult 抛 CancellationException → run 协程按取消收尾（存 checkpoint、关会话）
```

`RemoteUploadWindow` 在批次开始固定协商深度：第 0 张只预上传一次，之后按输入索引
提交、按 FIFO 接收；深度 0 在前一结果返回后才上传下一张。发前上传作业串行衔接，
防止同一会话的多帧上传交错。`CoordinatorClient` 在同一门闩下记录每个句柄的接收
计数和延后关闭集合；阻塞接收不持门闩，关闭遇到在途接收时由该句柄最后一次接收
收尾释放，旧会话的收尾不能释放新会话。

**Android 连接/模型共享（NativeWorker）**：
```
DeviceViewModel → CoordinatorClient → NativeWorker.nativeRequestConnection
  (JNI: 握手+配对，TOFU 返回学到的 PC 公钥/指纹)
  → nativeListPCModels / nativeUploadModelToPC / nativeDownloadModelFromPC
  （模型共享为一次性连接：握手+配对+单操作帧）
```

模型安装先写不可被扫描的暂存文件，校验同名 `.param`/`.bin` 配对，再备份旧文件并
依次就位；任一步失败恢复本次移走的旧文件。CLI 导入、PC 接收上传和 Android 发行
下载均遵守此规则。两文件 rename 不构成崩溃事务；进程被强杀时仍可能留备份，
备份恢复失败时必须保留旧内容，不能把失败当作安装成功。

---

## 10. 相关文档

| 文档 | 内容 |
|------|------|
| `README.md` | 用户向使用说明（构建、CLI 用法、分布式模式） |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 开发规范：构建/测试/发布流程与已知坑 |
| [DESIGN_LANGUAGE.md](DESIGN_LANGUAGE.md) | Android 端 UI 设计语言（规范性） |
| [desktop/DESIGN.md](../desktop/DESIGN.md) | 桌面端 Qt Quick 设计语言与架构（组件层、token、动效、无头验证） |
| [serve-tests-design.md](serve-tests-design.md) | serve E2E 测试套件取舍记录与非确定性断言的定标依据 |
| `models/README.md` | 模型分发、格式与发版流程 |

历史设计文档（serve daemon 设计、架构简化 ADR、性能报告）已随仓库精简移除，见 git 历史。
