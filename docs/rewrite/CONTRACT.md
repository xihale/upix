# upix Rust 重写契约

> 分支 `rust-rewrite`。一次完成的大调：C++23 核心 → Rust workspace，唯一 C++ FFI 岛 = ncnn 引擎；
> CLI（clap）与 GUI（nsh）都是一等公民，直连同一份核心。旧树（`share/ cli/ apps/ desktop/`）在本分支
> 保留到 cutover 阶段，由集成 owner 统一删除；移植 agent 不得改动（engine-ffi 的文件搬移除外）。

## 0. 总则

- 语义等价 > Rust 惯用法：逐字对照对应 C++ 源，行为（边界、错误路径、注释里的实测坑）保持一致。
- 线程模型沿用 C++：`std::thread` + `Mutex`/`Condvar`/原子。禁 tokio/async。
- 库 crate 禁 panic（`Result` 化）；禁 `todo!`/`unimplemented!`；注释英文，保留 C++ 注释中的坑位语义。
- 只 `cargo check/test -p <自己的crate>`；禁 workspace 级构建/测试/fmt/clippy；不 `git commit`。
- 共享契约文件（不许改，需变更写在交付报告里）：
  - `crates/upix-core/src/engine.rs` — Engine trait（engine.hpp + tile_types.hpp 的 Rust 镜像）
  - `crates/upix-net/src/protocol.rs` — 全部 wire 类型（protocol.hpp 镜像，repr(C) 布局是契约）
  - `crates/upix-img/src/lib.rs` — `EncoderOptions`（encoder_options.hpp 镜像）
- 环境：rustc 1.94.1 / edition 2024；Manjaro，24 核；GPU：Intel iGPU + NVIDIA RTX 4060；`DISPLAY=:0`。

## 1. 环境事实

| 项 | 事实 |
|---|---|
| ncnn | 无系统包；xmake 缓存 `~/.xmake/packages/n/ncnn/20260526/<hash>/{include,lib}`，多个 hash = 不同平台变体，用 `manifest.txt` 辨认 linux/x86_64 版 |
| Vulkan | 系统 `/usr/include/vulkan` 若缺，查 xmake 缓存 `~/.xmake/packages/v/vulkan-headers`；loader 链接 `-lvulkan` |
| 加密 | 系统 libsodium 1.0.22（只给金标 dumper 用，Rust 侧用 RustCrypto） |
| libdeflate/zlib | 系统头文件就绪 |
| 模型缓存 | `~/.local/share/upix/models`（44 个，flat `<name>.param/.bin`） |
| 预编译 C++ CLI | `build/linux/x86_64/release/upix-cli`（金测参照黑盒，勿动勿重建） |
| GTK4 | 4.22.4（nsh 要求 ≥4.14）✓ |
| nsh | v0.4.0（crates.io 已发布，API 每版本都 breaking → 锁 `=0.4.0`） |

## 2. crate 分工

| crate | agent | 移植源（C++） | 依赖 |
|---|---|---|---|
| `upix-core` | CorePort | `share/core/{tile_processor,tile_optimizer,engine_pool,engine_setup,model_manager,model_graph,model_metadata,config}` + `image.cpp` 的 `bicubic_scale_u8` | — |
| `upix-img` | ImgPort | `share/utils/{image,image_avif,exif,sr_marker,encoder_options}` | — |
| `upix-net` | NetPort | `share/core/distributed/{protocol,crypto_sodium,crypto_session,identity}` + `share/utils/{network,stream_socket,unix_socket}` | img |
| `upix-host` | HostPort | `share/core/distributed/{coordinator,coordinator_admin}` + `share/core/serve/daemon` | core, net, img |
| `upix-engine-ffi` | EngineFfi | `share/core/{ncnn_engine*,ncnn_gpu_init,ncnn_gpu_pipeline,vulkan_shaders,groupnorm_layer,spirv_pack*,gpu_precision,tile_types}` + `utils/{expected,logger}`（仅头） | core |
| `upix-gui` | GuiNsh | 新写（nsh app），参照 `apps/`（Compose）与 `desktop/`（Qt）的页面与交互 | core, net, img, host |
| `upix-cli` | CliIntegrator（第二批） | `cli/src`（8K 行） | 全部 |

## 3. §core

**移植清单**：`tile_processor.{hpp,cpp}`（布局计算、三段流水线 Load/Proc/Save 两个有界队列+condvar、
`blend_tile` 二次衰减 w*w 归一化、`extract_tile_u8` 逐 tile 转 float/RGBA 解交错）、
`tile_optimizer.hpp`（候选 {64..512} 实测 tiles/sec + 内存预算淘汰、×1.05 容忍带、按
`engine.hardware_id()` 的持久化缓存——读源码确认落盘路径/格式）、`engine_pool`、`engine_setup`
（GPU/CPU/GPU_THEN_CPU/默认四策略 + 回退 + `switch_model` 懒换模）、`model_manager`（递归扫描、
param 解析、文件名 xN）、`model_graph`（无网络图检测：Interp 级/反卷积/PixelShuffle BFS/Reorg 除子、
`scale_from_filename`）、`model_metadata`（场景标注表）、`config.cpp`（serde JSON **字段名与 glaze
输出逐字一致** + validate 边界，常量：tile 256/16/1、min 64、max 512、quality 90、tile∈[16,2048]、
batch∈[1,64]）、`bicubic_scale_u8`（Mitchell-Netravali，来自 image.cpp，alpha 通路用）。

**API pin**（命名可微调，语义不得减）：

```rust
pub struct ProcessingConfig { /* 字段名对齐 config.cpp 的 JSON schema */ }
pub fn sanitize_model_for_filename(&str) -> String
pub fn resolve_output_path(requested: &Path, cfg: &ProcessingConfig) -> PathBuf
pub fn compute_tile_layout(width: i32, height: i32, tile: i32, overlap: i32) -> Vec<TileRegion>
pub fn process_image_u8(engine: &dyn Engine, cfg: &ProcessingConfig, input: &[u8],
    width: i32, height: i32, channels: i32,
    progress: Option<&dyn Fn(&ProcessingProgress)>, cancel: &AtomicBool)
    -> Result<ScaledImage, EngineError>   // GPU 快速路径 → OOM 缩瓦片阶梯 → 通用三段流水线
pub fn initialize_engine(strategy: EngineStrategy) -> Box<dyn Engine>   // engine_setup
pub struct TileSizeOptimizer { pub fn find_optimal_tile_size(...) -> i32 }  // auto-tile + 缓存
pub struct EnginePool { acquire()/release() }
pub struct ModelManager { pub fn scan_models(dir: &Path) -> Vec<ModelInfo> }
pub mod model_graph { pub fn scale_from_filename(&str) -> Option<i32>; pub fn detect_from_param(&Path) -> Option<i32> }
```

**关键语义**（照搬 engine.hpp/ARCHITECTURE 注释）：
- `model_input_channels()==3 && channels==4` → alpha 按 stride 解交错丢出推理，host 侧
  `bicubic_scale_u8` 放大后并回第 4 通道；RGB 输入走 GPU 快速管线。
- GPU 快速路径失败 → 通用路径（同一引擎，不重载 CPU）；OOM（`kErrOutOfMemory=230`、
  空 GPU 输出、`is_oom_error` 消息匹配）→ 缩瓦片阶梯，不是保证成功的 CPU 回退。
- batch：GPU 串行、CPU 并发（trait 默认实现已按 C++ `std::async` 语义写好）。

**验收**：`cargo test -p upix-core`：布局边界/clamp；blend 接缝连续；RGBA alpha 拆分；
MockEngine（确定性变换）跑通三段流水线 + GPU 快速路径分支；真实模型目录扫描 44 个、
scale 与文件名一致；config JSON roundtrip（手写 fixture，字段名对齐 config.cpp schema）。

## 4. §img

**移植清单**：`image.{hpp,cpp}`（decode png/jpg/webp/avif、encode、EXIF 烘焙入口、
`bicubic_scale_u8` 归 core）、`image_avif.*`（**运行时 dlopen 系统 libavif**，`libloading`；
能力探测 → `--version` 报 `avif:system-dlopen`；缺失时 AVIF 编解码返回明确错误，不 crash）、
`exif.*`（orientation 1-8，JPEG APP1/TIFF 扫描语义：无 EXIF/非 JPEG 零行为变化）、
`sr_marker.{hpp,cpp}`（`urn:upix:sr:1`；`build_xmp` XML 属性转义；`probe` = 原始字节 needle
扫描，四种容器通用；scale/ts 必须可解析否则 None）、`encoder_options.hpp`（已在骨架里）。

**codec 选型（已定，照做）**：png → `png` crate（XMP 落**未压缩 iTXt**；crate 不支持就编码后手工
append chunk）；jpeg → `jpeg-encoder` crate（quality、subsampling 420/422/444、XMP 走 APP1
`urn:uuid:...` 包裹与 C++ 一致——读 image.cpp 确认确切字节布局）；webp → `webp` crate（vendored
libwebp；method/quality/lossless 直映）；avif → libavif C API via libloading（speed/subsampling/
threads 直映）。

**API pin**：

```rust
pub struct Image { pub width: i32, pub height: i32, pub channels: i32, pub data: Vec<u8> } // u8 interleaved
pub fn decode(data: &[u8]) -> Result<Image, ImgError>                    // 按魔数判格式，含 AVIF(dlopen)
pub fn decode_path(p: &Path) -> Result<(Image, Option<u8>), ImgError>    // (像素, EXIF orientation 1-8)
pub fn apply_orientation(img: &mut Image, o: u8)
pub fn encode(img: &Image, format: &str, quality: i32, opts: &EncoderOptions,
    xmp: Option<&str>) -> Result<Vec<u8>, ImgError>
pub fn avif_available() -> bool                                            // dlopen 探测
pub mod sr_marker { pub const NAMESPACE_URN; pub fn build_xmp(model,scale,ts)->String;
    pub fn probe(bytes: &[u8]) -> Option<SrMarker> }
```

**验收**：每格式 encode→decode roundtrip；quality/knob 生效（尺寸随 quality 变化）；XMP 写入后
`probe` 命中且 scale/ts 解析正确；截断/外来字节 → None；AVIF 无系统库时优雅报错（本机有 libavif
则跑真实路径）。

## 5. §net

**移植清单**：`protocol.cpp`（serialize/parse_header/verify_frame/compute_checksum；**加密帧
checksum 置 0 且必须跳过校验**——明文 CRC 过密文会泄字节，Poly1305 已覆盖完整性）、
`crypto_sodium.cpp`（X25519 共享点 → **BLAKE2b + 源码里的 domain separator/personalization
逐字节照抄** → 32B 会话密钥；Ed25519 sign/verify；secretbox seal/open）、`crypto_session.cpp`
（wire = `[14B 头][nonce 24B][密文+16B MAC]`）、`identity.cpp`（`~/.upix/identity` 0600，
HOME 未设回退 `%USERPROFILE%`；fingerprint = sha256(pubkey) 前 8 字节大写 hex——以源码为准）、
TrustStore（`~/.upix/trusted_clients` 每行一个 base64 公钥）、`network.cpp`（TCPSocket/TCPServer、
**收/发两把锁**（半发半收防死锁）、首字节限时 poll、本机 IP 枚举 getifaddrs→`if-addrs`）、
`unix_socket.cpp`、`stream_socket.cpp`（fd 基类 + I/O 锁）。

**RustCrypto 对映**：`x25519-dalek(static_secrets)` / `ed25519-dalek` / `blake2`（personalization
按源码）/ `crypto_secretbox`（XSalsa20-Poly1305，与 libsodium secretbox 逐字节兼容）/ `sha2` /
`crc32fast`（zlib 同款）。

**金标向量（本 crate 的核心交付）**：
- dumper 源码放 `crates/upix-net/vector_gen/`（临时 C++，用 g++ 直接编
  `share/core/distributed/{protocol,crypto_sodium,crypto_session,identity}.cpp` + `-I share
  -I <ncnn 不需要>` + `-lsodium`，只产出 JSON，不入 workspace 构建）。
- 向量：① 多个消息类型的 serialize 精确字节；② parse_header 往返；③ checksum/verify 样本；
  ④ 固定 eph/id 密钥对 → 会话密钥字节（两侧一致）；⑤ KeyHello transcript 的 Ed25519 签名
  （确定性）；⑥ EncryptedSession 固定 key+nonce 的 seal 字节 + open(seal(x))==x；
  ⑦ 固定公钥的 fingerprint；⑧ TrustStore 解析样本。JSON 提交到 `crates/upix-net/tests/golden/`，
  `tests/golden.rs` 断言 Rust 实现逐字节一致。

**API pin**：

```rust
pub mod frame { serialize_frame(MessageHeader, &[u8]) -> Vec<u8>;
    parse_header(&[u8]) -> Option<MessageHeader>;
    verify_frame(header: &[u8; HEADER_SIZE], payload: &[u8]) -> bool;
    compute_checksum(&[u8]) -> u32 }
pub mod crypto { 组密钥/签名/验签/secretbox 封装，语义对齐 crypto_sodium.cpp }
pub struct EncryptedSession { /* client/server 角色; seal_frame/open_frame; 分块续帧同理 */ }
pub struct Identity { load_or_create() -> Identity; public_key()->[u8;32]; sign(&[u8])->[u8;64];
    fingerprint()->String; device_id()->String }
pub struct TrustStore { is_trusted(&[u8;32]) -> bool; add(&[u8;32]); /* 追加前查询，握手不新增信任 */ }
pub mod tcp { TcpSocket{ connect(timeout), set_recv_timeout, recv_exact, recv_timeout,
    recv_poll(first_byte_timeout), send_all /*收/发独立锁*/ };
    TcpServer{ bind, accept }; local_ips() -> Vec<IpAddr> }
pub mod uds { UnixServer/UnixSocket（daemon 用） }
```

**验收**：金标全绿；身份文件权限 0600；TrustStore 追加/查询单元测试；`recv_poll` 首字节超时
语义测试（不弃半帧）。

## 6. §host

**移植清单**：`coordinator.{hpp,cpp}` + `coordinator_admin.cpp`：连接上限 8、握手接收超时 10s、
会话帧间空闲 60s；全客户端必须先 `CONNECT_REQUEST`；可信 peer 自动批准、首连阻塞等批准
（`--auto-approve` 跳过）、拒绝后重连仍需审批、`stop()` 先拒绝未决审批再回收线程；
CLIENT_SUBMIT 双格式（legacy 单帧 / struct+DATA 续帧，按 `input_data_length` **预分配**、
峰值 ≈ 总长+2×8MB、续帧被打断 → per-task 错误且会话保活）；EXIF 竖拍烘焙；
`tile_size=0` → `TileSizeOptimizer` 实测选优（进程级缓存，每模型一次）；进度走原子槽 + 轮询线程
每 tick 至多一帧（**不得**由 worker 直发）；任务级取消（~200ms poll、下一个 tile 边界停、
迟到 CANCEL 容忍、success=0/"canceled" 应答、其他连接不受影响、worker 由 handler join、
stop 沿 client_threads→handler→worker 确定性回收）；>100MB 结果走 DATA 续帧；
解码/编码失败 per-task 错误不摔连接；模型 list/upload/download 处理；
`pipeline_depth=2`（一张在算 + 一张完整在途；旧客户端 0）。
`serve/daemon.cpp`：UDS 0x0601-0604、请求含路径（无图像字节过 socket）、懒换模、
idle 600s 自退、`ServeProgress` 双 stage、输出路径派生。

**新增 API（GUI/CLI 共用，pin）**：

```rust
pub struct Coordinator { /* new(bind_addr, port, models_dir, engine_factory, auto_approve);
    start(); stop(); */ 
    // GUI 模式：审批回调经 channel 交 UI（C++ 是 stdin [y/n]；CLI 模式保留 stdin 回调）
}
pub fn pairing_qr_payload(...) -> String   // 与 cli/cmd_host.cpp + qrprovider.h 的卡片 payload 逐字节一致
pub mod modelctl { list_remote() -> Vec<RemoteModel>; download(name, dir); import(paths) }  // ureq，GitHub release
pub mod serve { run_daemon(cfg); ensure_running(); status(); stop(); request(...) }
```

**验收**：进程内集成测试：起 coordinator（auto_approve）+ 用 upix-net 原语做真实握手/配对/
分块提交（小 PNG + MockEngine）→ 收到 success 响应；取消路径（提交后立刻 cancel → "canceled"）；
chunked 打断 → per-task 错误且会话可复用；daemon：UDS 请求→进度帧→响应（MockEngine）。

## 7. §engine-ffi

**文件搬移（git mv，唯一被允许动 share/ 的 agent）**：
`share/core/{ncnn_engine.cpp, ncnn_engine_impl.hpp, ncnn_gpu_init.cpp, ncnn_gpu_pipeline.cpp,
vulkan_shaders.hpp, groupnorm_layer.hpp, spirv_pack.hpp, spirv_pack.cpp, spirv_pack.bin,
gpu_precision.hpp, tile_types.hpp}` → `crates/upix-engine-ffi/csrc/upix/core/`；
`share/utils/{expected.hpp, logger.hpp}` → `csrc/upix/utils/`（保 include 相对风格，build.rs
加 `-I csrc/upix`）。`.S/.incbin` 弃用：Rust 侧 `include_bytes!` + shim setter
`upix_engine_set_spirv_pack(const uint8_t*, size_t)` 注入（spirv_pack.cpp 最小 diff：extern
符号换成 setter 存的指针）。

**build.rs（cc crate）**：`-std=c++23`；include：csrc、ncnn 缓存 include、vulkan 头；链接：
静态 ncnn（`manifest.txt` 辨认 linux/x86_64 变体）、`vulkan`、`deflate`、`z`、`pthread`、`dl`；
`UPIX_NCNN_DIR` 环境变量可覆盖发现。ncnn 静态库传播的传递依赖按 `pkg-config`/试错补齐。

**shim（csrc/shim.{h,cpp}）**：extern "C" 覆盖 Engine 全表面：create/destroy、initialize(preferred)、
list_devices、load_model、unload_model、process、process_u8、process_batch（指针数组）、
process_tiles_gpu / _u8 / _u8_queue（任务数组）、debug_blob_scan、benchmark、current_device、
is_initialized、model_name、get/set_scale_factor、scale_factor_detected、model_input_channels、
set_inference_threads、get_recommended_tile_size、get_device_memory_budget_mb、get_memory_info、
hardware_id、supports_* 三旗。规则：**C++ 异常一个都不许穿过 FFI**（catch-all → `{code, message}`）；
cancel 用 `const std::atomic<bool>*`；progress 用 `fn ptr + ctx`。保持 C++ 源码原样
（Adreno SSBO 打包、GroupNorm 双路径、全局 Vulkan 加载锁、双缓冲提交线程——**一字不改语义**）。

**Rust 侧**：手写 `extern "C"` 声明（不用 bindgen）+ 安全封装 `NcnnEngine` 实现
`upix_core::Engine`。

**验收**：`cargo test -p upix-engine-ffi`：CPU initialize + 加载 `NomosUni-SPAN-x2` +
小图 process 两次结果逐字节一致（确定性）；`Real-ESRGANv3-anime-x2` 的
`scale_factor_detected()`/`get_scale_factor()` 合理；`get_recommended_tile_size()` > 0；
GPU 路径测试 `#[ignore]`（`UPIX_GPU_TESTS=1` 时跑）。测试模型缺失时优雅跳过。

## 8. §gui
> **方向变更（2026-11-06）：** 桌面 GUI 将从 nsh 迁至直接使用 GPUI，目标平台为
> Linux、macOS 和 Windows；Android 保留 `main` 分支的 Kotlin/Compose 原生应用。
> 下列 nsh 内容描述的是当前实现与原验收目标，并非新的技术选型。迁移尚未完成：
> Rust 网络层和 host 存在 Unix 专用代码，ncnn FFI 构建目前也只针对 Linux；
> 不能仅凭 GPUI 跨平台就宣称完整应用支持三平台。


> **状态：进行中（2026-11-06 恢复）。** `crates/upix-gui` 已回到 workspace
> members。工具链是仓库根 `rust-toolchain.toml` 的 1.98.1（nsh-core 0.4.0
> 需要 rustc ≥ 1.95）。默认 feature `integration` 直连 core/img/host/engine-ffi：
> 本地放大、Coordinator、`modelctl` 下载。`--no-default-features` 仍走
> MockBackend，供无模型时的选图→进度→对比。

**框架**：nsh v0.4.0（锁 `=0.4.0`），纯 Rust 模式（无 TSX/QuickJS，参照 `examples/gallery-rs`）。
先装 CLI（`curl -fsSL https://nsh.dev/install.sh | sh`），`nsh init /tmp/nsh-probe --template rust`
读模板拿到确切依赖名/版本写入 `crates/upix-gui/Cargo.toml`；再 `npx skills add longbridge/nsh`
把技能装上照做。依赖：upix-core/net/img/host（直连，不子进程包 CLI）。

**页面**（对齐 apps/ Compose 与 desktop/ 的现有 UX，中文文案）：
1. **Home**：选图（nsh 若无文件对话框则 drag-drop + 路径输入兜底）、本地放大入口、格式/质量。
2. **进度**：tile %/ETA/encoding stage（工作线程 channel → signal；UI 永不阻塞）。
3. **结果对比**：可拖分割线（对齐 desktop 的 drag tracking/clamps/单列接缝语义）。
4. **Host**：启/停 coordinator、配对 QR 图（`qrcode` crate → PNG → nsh image；
   payload 用 `upix_host::pairing_qr_payload`）、审批弹窗、指纹显示、信任列表。
5. **模型**：已装列表（ModelManager，场景徽标/★）、release 下载（`upix_host::modelctl`）、本地导入。

主题：亮/暗（nsh shadcn tokens），`UPIX_THEME=light|dark` 环境变量优先。
引擎调用全部在工作线程（Engine trait `&self` + core API），取消按钮 → `AtomicBool`。

**Android**：`nsh` 的 android target 步骤写入 `crates/upix-gui/ANDROID.md`（真机验证不在本期，
明确标注）；minSdk 影响：nsh 要求 API 28（现状 26）。

**验收**：`cargo check -p upix-gui`；`nsh run --target linux` 启动无崩溃（超时运行 + 日志干净）；
MockEngine 打通 选图→进度→对比 全流程；engine-ffi 已就绪则用真实模型（CPU）跑一次真放大截图/日志
证明；QR 页渲染出真实 payload 的二维码。

## 9. §cli（第二批 CliIntegrator）

移植 `cli/src` 全命令树（8K 行）：隐式 upscale（无动词首选形态）、flag 形态、`--config` JSON、
`--jobs`/`--gpu`/`--cpu`/`--no-daemon`、全部 encoder knobs（png-level/jpeg-subsampling/
webp-method/webp-lossless/avif-speed/avif-subsampling/avif-threads）、`-f/--force`（SR marker
跳过语义）、`model list/list-remote/download/import`、`host`/`host info`（QR 卡片打印 +
`--auto-approve`）、`daemon status/start/stop/--fg/--idle-timeout` + 首次放大自动拉起、
shell completions、`--version`（编解码能力位）、进度行 + 机器可读进度契约（对照 cmd_*.cpp 保持
shape，≥ v0.2.0 语义）、退出码、`UPIX_MODELS_DIR`/`--models-dir`。
**验收**：与预编译 C++ CLI 黑盒对拍：同一小图 + `NomosUni-SPAN-x2` CPU，输出**像素级一致**
（容器字节可不同，浮点 CPU 路径必须一致）；daemon status/stop 往返；completions 三 shell 输出；
金标 wire（远端模式起 coordinator 自连）。并负责跨 crate 编译修复。

## 10. cutover（集成 owner 专属，agent 勿动）

全部验证后：删 `share/`（已搬空的残余）、`cli/`、`apps/`、`desktop/`、`xmake.lua`、
`CMakeLists.txt`、gradle 残留；`tools/` 暂留源码不保证构建（perf/dump-spirv 待后续 Rust 化，
文档标注）；更新 `README.md`/`docs/ARCHITECTURE.md`/`DEVELOPMENT.md` 反映新拓扑与 cargo 构建。
