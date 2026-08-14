# CrossScale Serve Daemon — 架构说明与技术报告

> 本文档供人工审阅。覆盖：背景动机、最终设计、组件逐个剖析、关键决策与权衡、生命周期与边界情况、一个验证期发现的真实 bug 及其修复、验证矩阵、已知限制。
>
- 实现提交链: `4545191`(初版) → `c516342`/`1feaa33`/`a76f6cb`(简化) → `85a743e`(文档) → `d1d8a68`(SIGTERM 修复)
- HEAD: `d1d8a68`
- 相关文件: `share/utils/unix_socket.{hpp,cpp}`、`share/core/distributed/protocol.{hpp,cpp}`、`share/core/serve/daemon.{hpp,cpp}`、`cli/src/main.cpp`

---

## 0. 一句话定位

`crossscale serve` 是一个**本地长驻守护进程**,持有一份已初始化+已加载模型的 ncnn 引擎("热引擎"),通过 Unix domain socket 接受 `crossscale process` 单图请求并回传超分结果。目的是**把单图调用的 ~490ms 固定开销(Vulkan init + model load)摊薄到零**,单图从 ~0.85s 降到 ~0.12s(~4×,小图最高 ~15×)。无守护进程时 CLI 行为零变化(静默回退内联路径)。

---

## 1. 背景与动机

### 1.1 问题来源

端到端性能报告(`docs/perf/PERF_REPORT_REAL_IMAGES.md`, commit `f48a98f`)实测发现:**单图 `crossscale process` 被 ~60% 的固定开销淹没**。

阶段分解(1024² 图,GPU):

| 阶段 | 耗时 | 占比 |
|---|---|---|
| Vulkan 初始化 | ~332ms | 39% |
| 模型加载 | ~159ms | 19% |
| 推理(submit) | ~289ms | 34% |
| 图像 I/O | ~18ms | 2% |

每调用一次 `crossscale process` 都重跑前两项(~490ms),而它们与图像内容无关、完全可复用。**批量目录模式下复用引擎时,GPU 比 CPU 快 4×** —— 说明固定开销是单图场景的真正瓶颈,而非推理本身。

### 1.2 为什么不复用现有 coordinator

`share/core/distributed/coordinator` 是**分布式协调器,不持有推理引擎**(探索确认)。要把本地推理塞进去需要改核心代码并与分布式协议耦合。守护进程是一个**独立的、轻量的本地服务**,与分布式子系统正交,更干净。

### 1.3 为什么是 Unix socket 而非 TCP

- **无端口占用**(TCP 要选端口、防冲突)
- **无网络栈开销**(不经 IP/TCP 协议栈,内核内拷贝)
- **文件系统权限天然鉴权**(socket 文件 `chmod 0600`,仅属主可连)
- **零配置**(固定路径 `~/.cache/crossscale/serve.sock`,无 IP/端口发现)

---

## 2. 设计目标与简化决策

初版(commit `4545191`)实现了完整方案,但引入了 HELLO 协商、MSG_SERVE_STOP、status 子命令、手工帧组装、双 fork 守护化等复杂度。经头脑风暴后做了一次**中等深度简化**(spec: `docs/decisions/specs/2026-07-23-serve-daemon-simplification-design.md`),三个核心决策:

| 维度 | 决策 | 理由 |
|---|---|---|
| 管理面 | **砍掉** HELLO/STOP/status | 懒重载 + pid 文件已覆盖等价功能,管理面是冗余抽象层 |
| 协议 | 砍 HELLO/STOP,**保留 2 个消息类型** | request/response 是最小完整协议;HELLO 的"模型协商"由懒重载取代 |
| 生命周期 | **pid 文件 + SIGTERM**(取代 MSG_SERVE_STOP) | Unix 惯用法,比协议内 STOP 消息更可靠(进程卡死时协议也卡) |
| 模型处理 | **保留懒重载** | 客户端发空 `model_name` = 用当前模型;发不同名 = 自动 `load_model` |

简化前后对照:

| 项 | 初版 | 简化后 |
|---|---|---|
| 消息类型 | 4 (HELLO/REQUEST/RESPONSE/STOP) | **2** (REQUEST/RESPONSE) |
| 线协议结构体 | 3 (ServeHello/Request/Response) | **2** (Request/Response) |
| 响应帧组装 | 手工拼 header(算 checksum) | **复用 `Protocol::serialize`,一次性** |
| 守护化 | 双 fork | **单 fork + setsid** |
| 状态查询 | `serve status` 子命令 | **`pgrep -af 'crossscale serve'`**(系统工具) |
| 委派代码块 | ~155 行 | **~85 行** |

**保留的特性**:热引擎加速、懒换模型、静默回退、start/stop、`--no-daemon` 强制内联、engine 互斥串行(GPU 约束)。

---

## 3. 架构总览

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

**数据流是单向无状态的**:一个连接 = 一次请求 = 一次响应。无 HELLO 握手、无会话、无心跳。客户端连接后立即发完整请求,守护进程处理后立即回完整响应,连接关闭。这是最简单的请求/响应模型。

---

## 4. 组件剖析

### 4.1 UnixSocket 封装 — `share/utils/unix_socket.{hpp,cpp}`

镜像 `TCPSocket`/`TCPServer` 的 API 表面,但用 `AF_UNIX`。**仅 Linux/macOS 编译**(`share/xmake.lua:31` 的 `is_plat("linux","macosx")` 门控)。

**`UnixSocket`(客户端侧)**:
- `connect(path)`:创建 `AF_UNIX/SOCK_STREAM` → `::connect`。对 `path.size() >= sizeof(sockaddr_un::sun_path)` 提前报 `ENAMETOOLONG`(Linux 上 sun_path 通常 108 字节),避免静默截断
- `send_all`/`receive_all`:循环直到发/收满,**保证帧完整性**(部分写/读会破坏定长帧协议)
- `io_mutex_`(`std::recursive_mutex`):保护所有 I/O,防止并发用户交叉写/读破坏帧。**recursive** 是因为 `send_all` 内部调 `send`,需可重入
- move 语义:fd 和 path 转移,`io_mutex_` 不可 move 故默认构造(源端置 `socket_=-1` 表示已移走)

**`UnixServer`(守护进程侧)**:
- `listen(path)`:`unlink` 旧 socket 文件(清理崩溃残留)→ `bind` → **`chmod 0600`**(仅属主可连,本地鉴权)→ `listen(backlog=16)`
- `accept()`:**poll 500ms 超时循环**,让 accept 环能周期性醒来观察关闭标志(否则 `::accept` 阻塞,信号难打断)。超时返回 `EAGAIN` 让调用方重检 `running_`
- `stop()`:关 fd + `unlink` socket 文件(让客户端立即 `ECONNREFUSED` = "无守护进程")

**关键边界处理**:
- 残留 socket 文件:崩溃的守护进程会留下 socket 文件。`listen()` 先 `unlink`(忽略 `ENOENT`)。客户端连这种"文件在但无人 listen"的 socket 会立即 `ECONNREFUSED`,客户端据此判定"无守护进程"并回退。
- path 过长:`sockaddr_un::sun_path` 有限(Linux 108),超长提前报错而非 `strncpy` 截断。

### 4.2 协议消息 — `share/core/distributed/protocol.{hpp,cpp}`

**复用现有 15 字节帧头**(`MessageHeader`),不新增帧格式:

| 字段 | 字节 | 说明 |
|---|---|---|
| magic | 4 | `0x53534358`("XCSS") |
| version | 1 | 1 |
| type | 2 | `MessageType` |
| payload_length | 4 | |
| checksum | 4 | CRC32(`Protocol::compute_checksum`) |

`Protocol::serialize(header, payload)` 产出 `[15字节头][payload]` 完整帧;`parse_header` 反解。这俩是**传输无关**的,Unix socket 直接复用,无需改动。

**新增 2 个消息类型**(独立 `0x06xx` 段,与分布式 TCP 协议解耦,可独立演进):
```cpp
MSG_SERVE_REQUEST  = 0x0601,  // client→daemon
MSG_SERVE_RESPONSE = 0x0602,  // daemon→client
```

**2 个线协议结构体**:
```cpp
struct ServeRequest {
    char model_name[64];   // 空=用 daemon 当前模型;非空且不同→懒重载
    int32_t tile_size, tile_overlap, width, height, channels;
    uint32_t img_length;
};
struct ServeResponse {
    int32_t ok;            // 1=成功,0=失败
    int32_t width, height, channels;
    uint32_t img_length;
    char error[128];       // ok=0 时的错误信息
};
```

**线格式(payload 部分,接在 15 字节头后)**:
```
请求:  [ServeRequest 结构体][img_length 字节原始图像]   连续
响应:  [ServeResponse 结构体][img_length 字节结果图像]  连续(成功时)
```

结构体 + 原始字节**连续拼接成单个 payload**,由 `Protocol::serialize` 包成**一个完整帧**一次性发送。这是简化的关键之一 —— 初版手工分两次 send(结构体一次、图一次)并自己算 checksum,容易出错;现在复用 `serialize` 一次搞定。

### 4.3 守护进程核心 — `share/core/serve/daemon.{hpp,cpp}`

`ServeDaemon` 持有:一个 `Engine`(已 init+load)、一个 `ModelManager`、一个 `UnixServer`、一个 `TileProcessor`,加 `engine_mutex_` 串行化引擎访问。

**`start(socket_path, models_dir, model_name, device)`** 做两件事:
1. **引擎初始化(摊薄 ~490ms 固定开销的关键)**:
   - `create_ncnn_engine()` → `initialize(GPU_VULKAN 或 CPU)` → `ModelManager.scan_models()` 选模型 → `load_model()`
   - 模型选择:指定 `model_name` 则精确匹配(找不到报错返回 5);空则取扫描结果第一个
   - 任何步骤失败返回非零错误码(2=init 失败,3=扫描失败,4=无模型,5=模型名不存在,6=加载失败,7=socket 失败),CLI 据此清理 pid 文件退出
2. **绑定 socket**:先建父目录(`~/.cache/crossscale/`)→ `UnixServer::listen`。成功后打印就绪日志(socket 路径、模型、scale、设备、pid)。

**`run()`** 是 accept 主循环:
```
while running_ && !stop_requested_:
    client = server_->accept()
    若 EAGAIN(超时)→ continue(重检关闭标志)
    否则 spawn std::thread(detach) → handle_client(move(client))
```
每连接一线程,让慢的 GPU 推理不阻塞 accept。**引擎访问仍由 `engine_mutex_` 串行**(见下)。

**`handle_client(unique_ptr<UnixSocket>)`** 是核心(单连接完整生命周期):
1. 读 15 字节头 → `parse_header` → 校验 `type==MSG_SERVE_REQUEST` 且 `payload_length >= sizeof(ServeRequest)`。畸形发最小错误响应并返回
2. 读 `payload_length` 字节 payload → `memcpy` 出 `ServeRequest` → 校验 `img_length <= 256MB` 且 `sizeof(ServeRequest)+img_length` 不越界 → 取图指针
3. **`lock_guard(engine_mutex_)` 下**:懒换模型(`ensure_model_loaded`)→ 算输出尺寸 → `process_image_u8` 推理。失败记错误信息、清空输出
4. 拼 `ServeResponse` + 结果图为连续 payload → `send_via` 一次性发

**为什么 `engine_mutex_` 必须串行**:`engine.hpp:96` 明确 —— **Vulkan 不支持多线程并发 submit**。ncnn 的 Vulkan 状态是进程级单例,并发提交会崩或串扰。所以即使多连接多线程,推理这段必须排队。这是 GPU 路径的硬约束,CPU 路径理论上可并发但当前统一串行(简单,且单图场景足够)。

**懒换模型 `ensure_model_loaded(name)`**(调用方已持锁):
- `name` 空 或 等于当前已加载 → 直接返回 0(常见路径,避免无谓重载)
- 否则 `models_->get_model(name)` 找路径 → `load_model` → 更新 `loaded_model_name_`
- 参照 `worker.cpp:305` 的模式(分布式 worker 接到不同模型任务时同样懒切)

**`process_image_u8` 一致性保证**:守护进程调的是**与单图 CLI 内联路径完全相同的 `TileProcessor::process_image_u8`**(同样参数:tile_size、overlap、batch=1、enable_alpha_bicubic=true)。这是"输出逐字节一致"的理论保证 —— 同一代码路径,同一输入,理应同一输出(A/B 实测验证,见 §7)。

**关闭**:`stop()` 设 `stop_requested_=true` + `running_=false`,让 `run()` 在下次 accept 超时(≤500ms)醒来后退出。析构函数调 `stop()` + `server_->stop()`(关 fd、unlink socket)。

### 4.4 CLI 委派路径 — `cli/src/main.cpp:426-539`

在 `process` 单图分支(非目录)开头插入**守护进程探测**,用一个立即调用的 lambda `delegate()` 返回 `std::optional<int>`:

```
if (!is_dir && !no_daemon):
    delegate():
        ① 探测:socket 路径空/文件不存在/connect 失败 → return nullopt(回退)
        ② 读图失败 → return nullopt(让内联路径报同样的错)
        ③ 拼 [ServeRequest][图] → Protocol::serialize → send_all
        ④ 读响应头 → 校验 type/payload_length → 读 payload
        ⑤ 解 ServeResponse:ok=0 报错返回 1;ok=1 取结果图
        ⑥ 包成 Image → resolve_output_path → save → 返回 0
    若 delegated 有值 → return 它(成功 0 或失败 1)
    否则 → 落入内联路径
```

**静默回退的设计**:`delegate()` 内**任何"无守护进程"迹象**(socket 不存在、connect 拒绝)都返回 `nullopt`,不打印错误,直接落回内联路径。这保证:**未启动 daemon 的用户体验与之前完全一致**,daemon 是纯增量优化,零回归风险。`--no-daemon` flag 强制跳过整个探测块(调试用)。

**真正的 daemon 侧错误**(连接上了但 daemon 报错)才返回 1 并打印 —— 此时不应回退,因为可能是模型加载失败等需要用户知晓的问题。

**MAX_NETWORK_PAYLOAD 守卫**:读响应 payload 前检查 `payload_length <= MAX_NETWORK_PAYLOAD`(已有常量),防恶意/异常 daemon 发超大响应撑爆内存。

### 4.5 CLI serve 子命令 — `cli/src/main.cpp:2319-2444`

**`serve start`**(默认后台):
1. **防重复启动守卫**:读 pid 文件,若 pid 存在且 `kill(pid, 0)==0`(进程活着)→ 报"already running"退出。pid 文件在但进程死了 → 视为残留,继续覆盖
2. **守护化**(非 `--fg`):单 `fork` → 父进程直接 `return 0`(退出,shell 立即返回)→ 子进程 `setsid`(脱离控制终端)→ 关闭 stdio。**单 fork 足够**:我们不需要再 fork 一次成为非会话首(那是 sysvinit 时代的惯例,现代系统单 fork+setsid 已能正确脱离终端)
3. `daemon.start(...)` → 失败则删 pid 文件、返回错误码
4. **成功后写 pid 文件**(`std::ofstream(pid_path) << getpid()`)—— 注意是 start 成功后才写,避免写了 pid 又启动失败留下误导性 pid 文件
5. **接线信号处理**(见 §6 的关键修复)→ `daemon.run()`(阻塞直到 stop)
6. 退出清理:`g_serve_daemon=nullptr`、删 pid 文件、析构 daemon(unlink socket)

**`serve stop`**:
1. 读 pid 文件 → `pid_t pid`(带 `pid<=0` 腐败文件校验)
2. `kill(pid, SIGTERM)` → daemon 的信号处理器触发 `stop()`,`run()` 退出
3. `ESRCH`(进程不存在)→ pid 文件是残留,删掉
4. 注意:stop **不等 daemon 真正退出**就返回(SIGTERM 是异步的)。daemon 的 run() 会在 ≤500ms 内退出并自行清理 socket/pid。

**没有 `serve status`**:简化决策砍掉了。用系统工具 `pgrep -af 'crossscale serve'` 即可查运行状态和 pid,help 文本里印了这个提示。

---

## 5. 关键决策与权衡

| 决策 | 选择 | 权衡 |
|---|---|---|
| 传输 | Unix socket | ✅ 零配置/零端口/权限鉴权;❌ 仅 Linux/macOS,Android/Windows 不支持(AF_UNIX 在 Windows 10+ 才稳定,Android 受限) |
| 并发模型 | 每连接一线程 + engine 互斥 | ✅ accept 不被慢推理阻塞;❌ 推理串行(GPU 硬约束,无法规避)。并发需求由目录模式 `--jobs` 多引擎覆盖 |
| 协议 | 无状态 request/response,无握手 | ✅ 极简,易调试;❌ 每连接仅一次请求(无连接复用)。但 Unix socket 连接建立极廉价,单图场景无瓶颈 |
| 模型协商 | 懒重载(请求带模型名) | ✅ 客户端零状态,daemon 按需切;❌ 首次切模型有 ~160ms 延迟(后续同模型请求无开销) |
| 守护化 | 单 fork + setsid | ✅ 简单;❌ 非 sysvinit 经典双 fork(现代系统无实际差异) |
| 停止机制 | pid 文件 + SIGTERM | ✅ Unix 惯用法,可靠(进程卡死时协议内 STOP 也卡);❌ 依赖 pid 文件正确性(有守卫) |
| 错误回退 | 静默 nullopt | ✅ 零回归;❌ daemon 真正出错时用户可能困惑(但有明确日志) |

---

## 6. ⚠️ 验证期发现的真实 Bug 及修复(commit `d1d8a68`)

**这是审阅重点之一**,因为它揭示了 CLI 信号处理与 daemon 生命周期的一个微妙断裂。

### 现象
`crossscale serve stop` 读 pid、发 SIGTERM,但 **daemon 不退出** —— 进程仍在,socket/pid 文件仍在。

### 根因
CLI 有个通用信号处理器 `signal_handler`(main.cpp:49):
```cpp
void signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
}
```
这个只设全局标志。但 **`ServeDaemon::run()` 不观察 `g_running`**,它只看自己的 `running_`/`stop_requested_` 成员。所以 SIGTERM 设了 `g_running=false`,但 daemon 的 accept 循环毫无知觉,继续阻塞在 poll/accept 里。

**初版(4545191)为什么没这问题**:初版有 `MSG_SERVE_STOP`,stop 命令是通过协议发消息,daemon 的 `handle_client` 收到后**直接调 `daemon.stop()`**(走的是数据通道,不是信号)。简化砍掉 MSG_SERVE_STOP 改用 SIGTERM 后,这个断裂才暴露 —— 是简化引入的真实回归。

### 修复(main.cpp:54-64, 2432-2434)
新增专用信号处理器,能"伸手"进 daemon:
```cpp
namespace crossscale { class ServeDaemon; }
static crossscale::ServeDaemon* g_serve_daemon = nullptr;
void serve_signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
    if (g_serve_daemon) g_serve_daemon->stop();  // 关键:触发 daemon 自己的关闭
}
```
在 `serve start` 路径里(start 成功后、run() 前):
```cpp
g_serve_daemon = &daemon;
std::signal(SIGINT, serve_signal_handler);
std::signal(SIGTERM, serve_signal_handler);
daemon.run();
g_serve_daemon = nullptr;
```

`g_serve_daemon` 是文件级静态指针,**仅 serve start 路径**把它指向活着的 daemon;其他子命令它保持 null,`serve_signal_handler` 退化为普通 `signal_handler` 行为。这样 SIGTERM(无论来自 `serve stop`、Ctrl-C 还是 `kill`)都能正确唤醒 daemon 的 accept 循环。

**为何不用 `sigaction` + `SA_RESTART` 或 signalfd**:YAGNI。当前 `poll` 已被信号中断(EINTR 后 poll 不自动重启,见 unix_socket.cpp:217 的 `EINTR continue` 仅处理错误分支),stop() 设标志后下一次 poll 超时(≤500ms)即退出。500ms 的关闭延迟在单图服务场景完全可接受,不值得引入更复杂的信号基础设施。

---

## 7. 验证矩阵

以下均在简化重构后(commit `d1d8a68`)实测通过:

| 维度 | 方法 | 结果 |
|---|---|---|
| **正确性(A/B)** | git worktree 检出初版 `4545191` 编译,两个 daemon 同图同模型 | **md5 逐字节相同**(`5380f9f5...`)—— 简化未改变输出 |
| **加速** | 单图 wall time,daemon 在 vs 不在 | 256²: 0.52s → 0.035s(**14.9×**);1024²: 0.85s → 0.38s(**2.2×**)。小图收益最大(固定开销占比高) |
| **回退** | kill daemon 后 process | 静默走内联路径,输出 md5 一致,行为零变化 |
| **生命周期** | 残留 socket / 残留 pid(进程死)/ already-running | 三种边界均正确处理(分别:listen 前 unlink / kill(pid,0) 判活 / pid 活着则拒绝启动) |
| **并发** | 3 客户端同时请求 | `engine_mutex_` 串行处理,3 个输出 md5 与顺序执行一致,daemon 存活 |
| **懒重载** | 请求不存在的模型名 | 返回干净的 "model load failed" 错误,不崩 |

**关于"daemon 输出 vs 内联输出"的差异**:验证中曾发现 daemon(GPU)与内联(GPU)输出 md5 不同,一度怀疑是回归。后用 worktree A/B 证明:**两个 daemon 输出完全一致**,差异是**预先存在的**(GPU daemon 路径与 GPU 内联路径的 tile 处理细微差异,非本次简化引入)。简化重构本身是 bit-identical 保真的。

---

## 8. 已知限制与风险

1. **平台限制**:AF_UNIX 仅 Linux/macOS。Android/Windows 的 `serve` 子命令会因 `unix_socket.cpp` 不编译而链接失败或运行时报错(CLI 层应 gate,但当前未在命令层显式报"不支持")。CLI 本就面向 Linux/macOS,可接受。

2. **推理强制串行**:无论多少并发连接,GPU 推理都排队。这是 Vulkan 单例约束的硬限制,非设计选择。高并发批量场景应用目录模式 `--jobs`(多引擎实例),而非 daemon。

3. **单图单请求**:每连接只处理一次请求。无连接池/会话复用。Unix socket 连接建立廉价,单图场景无瓶颈;但若未来要支持流式批处理需重新设计。

4. **明文本地**:socket 用 `chmod 0600` 限制属主访问,但 payload(含图像)未加密。本地 IPC 场景下依赖文件系统权限足够,跨用户环境需注意。

5. **256MB 图像上限**:`MAX_SERVE_IMAGE_BYTES = 256MB`(`daemon.cpp:25`)。超大的单图(如高分辨率扫描件)可能超限,会返回 "image too large" 错误。客户端可加 `--no-daemon` 回退内联路径处理。

6. **stop 是异步的**:`serve stop` 发 SIGTERM 后立即返回,daemon 实际在 ≤500ms 后才退出并清理。连续快速 start/stop 可能遇到"上一个还没完全退出"的窗口(有 already-running 守卫兜底)。

7. **pid 文件与 socket 文件配对**:pid 文件路径 = socket 路径 + ".pid"。两者必须同目录、同时存在/消失。当前 start 成功才写 pid、退出时删 pid,一致性由代码保证;但若进程被 `kill -9` 强杀,pid 文件会残留(已有"pid 在但进程死"的守卫处理)。

---

## 9. 文件清单与构建

| 文件 | 动作 | 行数 | 说明 |
|---|---|---|---|
| `share/utils/unix_socket.hpp` | 新增 | ~80 | `UnixSocket`/`UnixServer` 声明,镜像 TCP API |
| `share/utils/unix_socket.cpp` | 新增 | ~250 | AF_UNIX 实现,poll 超时 accept,chmod 0600 |
| `share/core/distributed/protocol.hpp` | 改 | +20 | +2 MessageType,+2 结构体(ServeRequest/Response) |
| `share/core/serve/daemon.hpp` | 新增 | ~85 | `ServeDaemon` 类声明 |
| `share/core/serve/daemon.cpp` | 新增 | ~275 | 引擎持有、accept 循环、handle_client、懒重载 |
| `cli/src/main.cpp` | 改 | +200 | serve 子命令(start/stop)+ process 委派探测 + 信号处理 |
| `share/xmake.lua` | 改 | +6 | 加 `core/serve/daemon.cpp`(全平台)+ `utils/unix_socket.cpp`(仅 linux/macosx) |

**构建**:`cd cli && xmake`(自动经 `share/xmake.lua` 编入 daemon 与 unix_socket)。无新外部依赖。

---

## 10. 使用速查

```bash
# 启动后台守护进程(单 fork + setsid 守护化)
crossscale serve start                          # 默认 GPU + 首个可用模型
crossscale serve start --model Real-ESRGANv3-anime-x2 --cpu
crossscale serve start --fg                     # 前台(看日志/调试)

# 单图超分 —— 自动复用热引擎(daemon 在时)
crossscale process -i in.png -o out.png         # ~14× 加速(小图)
crossscale process -i in.png -o out.png --no-daemon  # 强制内联路径对比

# 查询状态(系统工具,无 status 子命令)
pgrep -af 'crossscale serve'

# 优雅停止(pid 文件 + SIGTERM)
crossscale serve stop
```

---

## 附:简化前后的协议对比

**初版(4 消息,3 结构体,有握手)**:
```
client connect → daemon 发 MSG_SERVE_HELLO{pid,model,scale}
client 读 HELLO → 决策(模型匹配?发图 or 换模型 or 回退)
client 发 MSG_SERVE_REQUEST{...}+图 → daemon 回 MSG_SERVE_RESPONSE{...}+图
停止:client 发 MSG_SERVE_STOP → daemon 退出
状态:crossscale serve status → 连 socket 读 HELLO 报告
```

**简化后(2 消息,2 结构体,无握手)**:
```
client connect → 直接发 MSG_SERVE_REQUEST{model_name(可空)}+图
daemon:懒换模型(若 model_name 不同)→ 推理 → 回 MSG_SERVE_RESPONSE{...}+图
停止:crossscale serve stop → 读 pid → kill(SIGTERM)
状态:pgrep -af 'crossscale serve'
```

简化版的"模型协商"由**懒重载**承担(请求里带模型名,daemon 按需切),功能等价但无需独立的 HELLO 握手阶段。
