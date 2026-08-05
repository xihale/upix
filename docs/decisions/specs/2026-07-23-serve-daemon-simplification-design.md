# serve 守护进程简化设计

**日期**: 2026-07-23
**相关提交**: `4545191` (待简化的实现) → 简化后替换
**前置阅读**: `docs/PERF_REPORT_REAL_IMAGES.md` §4/§7.2(性能背景)

## 背景与动机

提交 `4545191` 实现了 `crossscale serve` 本地热引擎守护进程,让单图 `process` 调用通过 Unix socket 复用热引擎,获得 2-15× 加速。功能正确、已验证,但设计明显过度复杂:

- **4 个 MessageType + 3 个结构体**,其中 `MSG_SERVE_HELLO` + `MSG_SERVE_STOP` 是为管理面服务的,核心推理只需 2+2。
- **手工 frame 拼装**(main.cpp 委托块 ~17 行偏移数学)重新发明了已存在的 `Protocol::serialize`,并因此出过 "image too large" 的偏移 bug。
- **HELLO 握手 + lazy reload 双状态机**:客户端先读 HELLO 协商模型,守护进程又支持 lazy reload。两套机制解决同一个问题。
- **response 的二义读法**:"结构体 + 后续裸字节" 需要特殊处理,产生了一段"理论上不会触发但留着"的死代码分支。
- **`serve status` 子命令**:仅报告 pid/模型/设备,可被 `ps`/`pgrep` 完全替代。
- **双 fork daemonize**:为防"重新获取控制终端",但守护进程是本机 CLI 的临时服务,单 fork 已足够。

本设计**砍掉管理面和协议自找的复杂度**,保留核心架构(daemon + UnixSocket + lazy reload + 自动委托)。

## 设计目标

1. **简洁性**:消息类型 4→2,结构体 3→2,手工 frame 拼装归零(全部用 `Protocol::serialize`)。
2. **一致性**:serve 协议复用分布式协议已有的 `Protocol::serialize`/`parse_header`/`compute_checksum`,不重新发明。
3. **零功能损失**:单图 2-15× 加速、lazy model reload、静默回退、`serve start`/`serve stop`、`--no-daemon` 全部保留。
4. **可接受的取舍**:砍掉 `serve status`(用 `pgrep`/`ps` 替代)、预先 HELLO 模型协商(lazy reload 仍在,功能不丢)。

## 范围

**保留不动**:
- `share/utils/unix_socket.{hpp,cpp}` — AF_UNIX 封装,镜像 TCPSocket API,本身干净。
- 守护进程核心架构:热 Engine + accept 循环 + `engine_mutex_`(Vulkan 不支持并发 submit)。
- 推理路径:`TileProcessor::process_image_u8`(与 inline 单图路径完全一致 → 输出逐字节相同)。
- `--no-daemon` flag、单图自动委托 + 静默回退。

**砍掉**:
- `MSG_SERVE_HELLO` / `MSG_SERVE_STOP` 消息类型(保留 REQUEST/RESPONSE)。
- `ServeHello` 结构体 + 守护进程连接即发 HELLO 的握手。
- `serve status` 子命令。
- 客户端手工 frame 拼装(main.cpp:481-496),改用 `Protocol::serialize`。
- response "结构体 + 后续裸字节" 的二义读法 + 死代码分支。
- `start_time_unix_` 字段 + `ServeDaemon::hello()` 方法。
- 双 fork daemonize → 单 fork + `setsid()`。

**改变**:
- 守护进程生命周期:从 `MSG_SERVE_STOP` 消息 → pid 文件 + SIGTERM(Unix 守护进程惯用法)。

## 协议设计

### 消息类型(`share/core/distributed/protocol.hpp`)

```cpp
MSG_SERVE_REQUEST  = 0x0601,  // client → daemon: upscale 请求
MSG_SERVE_RESPONSE = 0x0602,  // daemon → client: upscale 结果
// 砍掉:MSG_SERVE_HELLO = 0x0600, MSG_SERVE_STOP = 0x0603
```

### 结构体(`share/core/distributed/protocol.hpp`)

```cpp
// client → daemon。线协议:header + payload,其中
// payload = [ServeRequest][img_length 字节图像] 连续存储。
struct ServeRequest {
    char model_name[64];   // 空名 = 用守护进程当前模型;非空且不同 → lazy reload
    int32_t tile_size;
    int32_t tile_overlap;
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t img_length;   // payload 中跟随的图像字节数
};

// daemon → client。线协议:header + payload,其中
// payload = [ServeResponse][img_length 字节图像] 连续存储(ok==1 时)。
struct ServeResponse {
    int32_t ok;            // 1 = 成功,0 = 失败
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t img_length;
    char error[128];       // ok==0 时的错误信息
};
// 砍掉:ServeHello { pid, model_name[64], scale, device, start_time_unix }
```

### 线协议(统一用 `Protocol::serialize`)

**关键简化**:payload 总是 `[结构体][图像字节]` 一个连续 buffer,一次 `Protocol::serialize` 调用生成完整 frame。无手工偏移数学,无 checksum_slot 预留。

**客户端发送**:
```cpp
std::vector<uint8_t> payload(sizeof(ServeRequest) + image.size());
std::memcpy(payload.data(), &req, sizeof(ServeRequest));
std::memcpy(payload.data() + sizeof(ServeRequest), image.data(), image.size());

MessageHeader h{Protocol::MAGIC, Protocol::VERSION,
                MessageType::MSG_SERVE_REQUEST,
                (uint32_t)payload.size(), 0};
auto frame = Protocol::serialize(h, payload.data());
sock.send_all(frame.data(), frame.size());
```

**守护进程接收**:
```cpp
// 读 header → parse_header → 读 payload_length 字节
std::vector<uint8_t> payload(h.payload_length);
sock.receive_all(payload.data(), payload.size());
// 切两段
ServeRequest req;
std::memcpy(&req, payload.data(), sizeof(ServeRequest));
const uint8_t* img = payload.data() + sizeof(ServeRequest);
```

**响应对称**:守护进程组装 `[ServeResponse][image]` 连续 payload,一次 serialize 发出;客户端一次 receive_all 读 payload_length 字节,按 `sizeof(ServeResponse)` 切两段。**没有二义读法,没有死代码。**

## 守护进程生命周期

### start 子命令(`cli/src/main.cpp`)

```cpp
// --bg(默认):单 fork + setsid 后台化;--fg:前台
if (!foreground) {
    pid_t pid = ::fork();
    if (pid < 0) { LOG_ERROR("Serve", "fork failed"); return 1; }
    if (pid > 0) return 0;   // 父进程退出
    ::setsid();              // 脱离控制终端
    ::close(STDIN_FILENO);   // 关 stdio
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
}

// 写 pid 文件(socket_path + ".pid")
std::ofstream(socket_path + ".pid") << ::getpid();

// SIGINT/SIGTERM → 设 stop 标志;信号处理里/run() 退出后 unlink socket+pid
std::signal(SIGTERM, signal_handler);  // 复用现有 g_running / signal_handler
std::signal(SIGINT,  signal_handler);

ServeDaemon daemon;
int rc = daemon.start(socket_path, models_dir, model_name, device);
if (rc != 0) return rc;
daemon.run();
// run() 返回后:守护进程析构 unlink socket;主进程删 pid 文件
std::filesystem::remove(socket_path + ".pid");
```

### stop 子命令

```cpp
std::ifstream f(socket_path + ".pid");
if (!f) { LOG_ERROR("Serve", "no pid file (daemon not running?)"); return 1; }
pid_t pid; f >> pid;
if (::kill(pid, SIGTERM) < 0) {
    LOG_ERROR("Serve", std::string("kill: ") + std::strerror(errno));
    return 1;
}
LOG_INFO("Serve", "stopped pid " + std::to_string(pid));
```

守护进程的 `signal_handler` 设 `g_stop` 标志,`run()` 的 accept 循环观测标志退出,`ServeDaemon` 析构(`server_->stop()`)unlink socket 文件。

### 砍掉 status 子命令

用户用 `pgrep -af crossscale serve` 或 `ps aux | grep serve` 替代。

## ServeDaemon 类瘦身(`share/core/serve/daemon.hpp`)

```cpp
class ServeDaemon {
public:
    ServeDaemon();
    ~ServeDaemon();
    ServeDaemon(const ServeDaemon&) = delete;
    ServeDaemon& operator=(const ServeDaemon&) = delete;

    auto start(const std::string& socket_path, const std::string& models_dir,
               const std::string& model_name, ServeDevice device) -> int;
    auto run() -> void;
    auto stop() -> void;

private:
    auto handle_client(std::unique_ptr<UnixSocket> sock) -> void;
    auto ensure_model_loaded(const std::string& model_name) -> int;  // 保留 lazy reload

    std::unique_ptr<Engine> engine_;
    std::unique_ptr<ModelManager> models_;
    std::unique_ptr<UnixServer> server_;
    std::unique_ptr<TileProcessor> processor_;
    std::mutex engine_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::string socket_path_;
    std::string loaded_model_name_;
    ServeDevice device_ = ServeDevice::GPU;
    // 砍掉:start_time_unix_, hello() 方法
};
```

### handle_client(简化后)

```
1. 读 header → parse_header
   - 非 MSG_SERVE_REQUEST 或 payload_length < sizeof(ServeRequest):
     发 error response,return
2. 读 payload_length 字节 = [ServeRequest][image]
3. lock(engine_mutex_):
   - ensure_model_loaded(req.model_name)  // 空名/同名=无操作,保留 lazy reload
     失败 → 发 error response,return
   - process_image_u8(img_in, out_bytes, ...)
     失败 → 记 err_msg
4. 组装 response payload [ServeResponse][out_bytes],一次 serialize 发出
```

**砍掉**:步骤 1 之前的"发 HELLO"、STOP 分支判断。

## CLI 委托块瘦身(`cli/src/main.cpp`)

委托块从 ~155 行降到 ~70 行:

```cpp
if (!is_dir && !no_daemon) {
    auto delegate = [&]() -> std::optional<int> {
        // 1. 探测:socket 不在 / 连不上 → 回退(返回 nullopt)
        auto sock_path = crossscale::default_serve_socket_path();
        if (sock_path.empty() || !fs::exists(sock_path)) return std::nullopt;
        crossscale::UnixSocket sock;
        if (!sock.connect(sock_path)) return std::nullopt;

        // 2. 载图:失败 → 让 inline 路径报错(不抢错误)
        auto image_res = crossscale::Image::load(input_path);
        if (!image_res) return std::nullopt;
        auto& image = *image_res;

        // 3. 组 payload [ServeRequest][image],一次 serialize
        ServeRequest req{};
        model_name.copy(req.model_name, sizeof(req.model_name) - 1);
        /* 填 tile_size, overlap, w, h, channels, img_length */
        std::vector<uint8_t> payload(sizeof(req) + image.size());
        std::memcpy(payload.data(), &req, sizeof(req));
        std::memcpy(payload.data() + sizeof(req), image.data(), image.size());
        MessageHeader h{MAGIC, VERSION, MSG_SERVE_REQUEST,
                        (uint32_t)payload.size(), 0};
        auto frame = Protocol::serialize(h, payload.data());
        sock.send_all(frame.data(), frame.size());

        // 4. 读 response header + payload [ServeResponse][image]
        /* 读 header,校验 RESPONSE */
        std::vector<uint8_t> rpayload(h.payload_length);
        sock.receive_all(rpayload.data(), rpayload.size());
        ServeResponse resp;
        std::memcpy(&resp, rpayload.data(), sizeof(resp));
        if (!resp.ok) { LOG_ERROR("Engine", "daemon: " + std::string(resp.error)); return 1; }
        const uint8_t* out = rpayload.data() + sizeof(resp);

        // 5. 包装成 Image + 保存(与 inline 路径相同)
        /* Image out_image(resp.w, resp.h, resp.c); memcpy; save */
        return 0;
    };
    if (auto r = delegate()) return *r;
    // 否则静默回退 inline
}
```

**砍掉的复杂点**(对应现状代码):
- main.cpp:432-441 HELLO 读 + 协商 → 无
- main.cpp:481-496 手工 frame 拼装 + checksum_slot 偏移数学 → 一次 serialize
- main.cpp:522-544 response "extra 字节" 死代码分支 → payload 一次读完切两段
- 多处分散的早返回 LOG_ERROR → 图 load 失败让 inline 报

## 文件变更清单

| 文件 | 动作 | 变更 |
|---|---|---|
| `share/core/distributed/protocol.hpp` | 改 | 删 `MSG_SERVE_HELLO`/`MSG_SERVE_STOP`,删 `ServeHello`,留 REQUEST/RESPONSE |
| `share/core/serve/daemon.hpp` | 改 | 删 `hello()`、`start_time_unix_` |
| `share/core/serve/daemon.cpp` | 改 | handle_client 删 HELLO/STOP 分支;response payload 改连续 |
| `cli/src/main.cpp` | 改 | 委托块重写(用 serialize);start 改单 fork+pid;stop 改 pid+SIGTERM;删 status |
| `share/utils/unix_socket.{hpp,cpp}` | 不动 | — |

## 不做的事

- 不改 `UnixSocket`/`UnixServer`(本身干净)。
- 不改 `Engine`/`TileProcessor`/`Protocol::serialize`/`parse_header` 核心。
- 不实现 Windows/Android 的 serve(AF_UNIX 不可用)。
- 不做连接池/多引擎并发(GPU 不支持并发 submit)。
- 不加新的管理面(日志/status 已够)。

## 风险与回退

- **stale pid 文件**:守护进程崩溃留下旧 pid 文件。`serve stop` 时 `kill` 返回 ESRCH(进程不存在)→ 报错并提示删 pid 文件;`serve start` 时若 pid 文件存在且 pid 存活 → 报"已在运行";pid 文件存在但 pid 死亡 → 覆盖重写。
- **stale socket 文件**:守护进程崩溃留下旧 socket。`UnixServer::listen` 已 unlink 旧文件;客户端 `connect` 失败 → 静默回退 inline。
- **lazy reload 失败**:守护进程 `ensure_model_loaded` 失败 → 发 error response,客户端 LOG_ERROR 后回退 inline(非静默,因为是真错误)。
- **简化后行为差异**:原 `serve status` 用户改用 `pgrep`;原 HELLO 预协商删除,lazy reload 仍保证多模型可用。

## 验证计划

1. **正确性**:简化后守护进程输出 vs inline 路径,5 张测试图 md5 逐字节相同。
2. **加速**:单图 wall time,守护进程在 vs 不在。预期与简化前持平(2-15×)。
3. **回退**:`serve stop` 后,`process` 静默回退 inline,md5 相同。
4. **生命周期**:`serve start --fg` 前台 SIGINT 退出 + socket/pid 清理;`serve start`(后台) + `serve stop` pid+SIGTERM 退出 + 清理;stale pid/socket 文件正确处理。
5. **并发**:3 个 `process` 同时跑,engine_mutex_ 串行,结果全对。
6. **lazy reload**:守护进程以模型 A 启动,客户端发模型 B 的请求,守护进程重载并正确推理。

## 预期净变化

| 项 | 现状 | 简化后 | Δ |
|---|---|---|---|
| serve MessageType | 4 | 2 | -2 |
| serve 结构体 | 3 | 2 | -1 |
| 手工 frame 拼装 | ~17 行 | 0 | -17 |
| HELLO 握手(两端) | ~25 行 | 0 | -25 |
| STOP 消息 + 分支 | ~20 行 | 0(pid+SIGTERM) | -20 |
| status 子命令 | ~30 行 | 0 | -30 |
| response 二义读法+死代码 | ~20 行 | 0 | -20 |
| 双 fork | ~17 行 | 单 fork ~10 行 | -7 |
| 委托块总长 | ~155 行 | ~70 行 | -85 |

**功能保留**:单图 2-15× 加速、lazy model reload、静默回退、`serve start`/`serve stop`、`--no-daemon`。
**功能砍掉**(可替代):`serve status`(→ `pgrep`)、预先 HELLO 协商(→ lazy reload 仍在)。
