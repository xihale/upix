# serve 守护进程简化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 砍掉 serve 守护进程的管理面(HELLO/STOP/status)和协议自找的复杂度(手工 frame 拼装、response 二义读法、双 fork),保留核心架构(daemon + UnixSocket + lazy reload + 自动委托),净减约 220 行,零功能损失。

**Architecture:** 协议收到 2 消息类型(REQUEST/RESPONSE)+ 2 结构体,所有线协议统一用已有的 `Protocol::serialize`(payload = `[结构体][图像]` 连续 buffer)。守护进程生命周期改 pid 文件 + SIGTERM(Unix 惯用法)。委托块从 ~155 行降到 ~70 行。`UnixSocket`/`UnixServer` 不动。

**Tech Stack:** C++23, ncnn, AF_UNIX socket, POSIX signals, xmake build。

**Spec:** `docs/decisions/specs/2026-07-23-serve-daemon-simplification-design.md`

---

## File Structure

| 文件 | 责任 | 变更 |
|---|---|---|
| `share/core/distributed/protocol.hpp` | 消息类型 + 结构体定义 | 删 HELLO/STOP 类型,删 ServeHello 结构体 |
| `share/core/serve/daemon.hpp` | ServeDaemon 类声明 | 删 `hello()`、`start_time_unix_` |
| `share/core/serve/daemon.cpp` | ServeDaemon 实现 | handle_client 简化(删 HELLO/STOP);response 改连续 payload |
| `cli/src/main.cpp` | CLI 入口 | 委托块重写(用 serialize);start 改单 fork+pid;stop 改 pid+SIGTERM;删 status |
| `share/utils/unix_socket.{hpp,cpp}` | AF_UNIX 封装 | 不动 |

---

## Task 1: 协议层简化 — 删 HELLO/STOP/ServeHello

**Files:**
- Modify: `share/core/distributed/protocol.hpp:33-40`(MessageType 枚举)
- Modify: `share/core/distributed/protocol.hpp:125-164`(serve 结构体)

- [ ] **Step 1: 删除 MessageType 中的 HELLO 和 STOP**

在 `share/core/distributed/protocol.hpp` 找到(约第 33-40 行):

```cpp
    MSG_SERVE_HELLO = 0x0600,     // daemon→client, sent right after accept
    MSG_SERVE_REQUEST = 0x0601,   // client→daemon: upscale request header
    MSG_SERVE_RESPONSE = 0x0602,  // daemon→client: upscale result header
    MSG_SERVE_STOP = 0x0603,      // client→daemon: request graceful shutdown
```

替换为(只留 REQUEST/RESPONSE):

```cpp
    MSG_SERVE_REQUEST = 0x0601,   // client→daemon: upscale request
    MSG_SERVE_RESPONSE = 0x0602,  // daemon→client: upscale result
```

- [ ] **Step 2: 删除 ServeHello 结构体**

在 `share/core/distributed/protocol.hpp` 找到 `ServeHello` 结构体(约第 134-140 行,带 `pid`、`model_name[64]`、`scale`、`device`、`start_time_unix` 字段)及其上方注释,整段删除。保留 `ServeRequest` 和 `ServeResponse`。

同时更新这两个结构体上方的注释块(约第 125-129 行),把 "Local serve daemon messages" 的说明改为反映简化后协议。新注释:

```cpp
// ── Local serve daemon messages ─────────────────────────────────────────────
// The daemon holds a warm engine; clients send an image, receive the upscaled
// result. On the wire: Protocol header + payload, where
// payload = [Serve{Request,Response} struct][img_length bytes] contiguous.
// All integers little-endian (host order on POSIX serve targets).
```

`ServeRequest` 结构体的字段**不变**(保留 `model_name` 支持 lazy reload):

```cpp
struct ServeRequest {
    char model_name[64];     // 空名 = 用守护进程当前模型;非空且不同 → lazy reload
    int32_t tile_size;
    int32_t tile_overlap;
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t img_length;
};

struct ServeResponse {
    int32_t ok;              // 1 = success, 0 = error
    int32_t width;
    int32_t height;
    int32_t channels;
    uint32_t img_length;
    char error[128];
};
```

- [ ] **Step 3: 编译验证(share 单独编译,确认无残留引用)**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/share && xmake 2>&1 | tail -20
```

Expected: 编译通过。若报 `MSG_SERVE_HELLO`/`MSG_SERVE_STOP`/`ServeHello` 未定义,说明 daemon.cpp 或 main.cpp 还有引用 —— 这是预期的,会在后续 Task 修复。**此时 share 库本身不应引用这三个符号**(它们只在 daemon.cpp/main.cpp 用),所以 share 应编译通过。若 share 报错,定位并修复。

- [ ] **Step 4: Commit**

```bash
git add share/core/distributed/protocol.hpp
git commit -m "refactor(serve): drop HELLO/STOP/ServeHello from protocol

消息类型 4→2,结构体 3→2。REQUEST/RESPONSE 协议不变(已是连续 payload)。
daemon.cpp/main.cpp 的引用在后续 task 修复。"
```

---

## Task 2: daemon.hpp 瘦身 — 删 hello()/start_time_unix_

**Files:**
- Modify: `share/core/serve/daemon.hpp:33,62-64,82-84`

- [ ] **Step 1: 删除 hello() 方法和 start_time_unix_ 成员**

在 `share/core/serve/daemon.hpp` 删除 `hello()` 方法的声明(约第 62-63 行):

```cpp
    // Snapshot for logging: model name, scale, device, pid.
    auto hello() const -> ServeHello;
```

删除成员变量 `start_time_unix_`(约第 84 行):

```cpp
    int64_t start_time_unix_ = 0;
```

删除前向声明 `struct ServeHello;`(约第 31 行)—— 简化后 daemon 不再用它。

- [ ] **Step 2: 编译验证**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/share && xmake 2>&1 | tail -20
```

Expected: share 不编译 daemon.cpp(它是 cli 依赖的一部分),但 header 应能被 include 不报错。若 daemon.cpp 因 hello()/start_time_unix_ 报错,**这是预期的**(Task 3 修)。此步只验证 header 本身语法正确。

- [ ] **Step 3: Commit**

```bash
git add share/core/serve/daemon.hpp
git commit -m "refactor(serve): drop hello() and start_time_unix_ from ServeDaemon

只给 status 子命令用的字段,简化后删除。"
```

---

## Task 3: daemon.cpp 简化 handle_client + response 连续 payload

**Files:**
- Modify: `share/core/serve/daemon.cpp:44-55,63-140,163-259`(start、handle_client)

- [ ] **Step 1: 删除 start() 里的 start_time_unix_ 初始化**

在 `daemon.cpp` 的 `ServeDaemon::start()` 开头(约第 67-70 行)删除:

```cpp
    start_time_unix_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
```

同时删除该文件顶部的 `#include <chrono>`(若不再被其他地方用;grep 确认)。

- [ ] **Step 2: 重写 handle_client(删 HELLO 发送 + STOP 分支,response 改连续)**

把整个 `handle_client` 方法(约第 163-259 行)替换为简化版。核心改动:
1. 删步骤 1(发 HELLO)。
2. 删 STOP 分支判断。
3. response payload 改成 `[ServeResponse][image]` 连续 buffer,一次 `send_via`。

新 `handle_client`(完整):

```cpp
auto ServeDaemon::handle_client(std::unique_ptr<UnixSocket> sock) -> void {
    // sock owns the fd; closed exactly once on return.

    // ── 1. Read request header ──────────────────────────────────────────────
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    if (!sock->receive_all(hdr_buf.data(), hdr_buf.size())) return;
    auto parsed = Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
    if (!parsed || parsed->type != MessageType::MSG_SERVE_REQUEST ||
        parsed->payload_length < sizeof(ServeRequest)) {
        // Malformed: send a minimal error response and bail.
        ServeResponse r{};
        r.ok = 0;
        std::strncpy(r.error, "expected MSG_SERVE_REQUEST", sizeof(r.error) - 1);
        send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
        return;
    }

    // ── 2. Read payload = [ServeRequest][image] contiguous ──────────────────
    std::vector<uint8_t> req_buf(parsed->payload_length);
    if (!sock->receive_all(req_buf.data(), req_buf.size())) return;
    ServeRequest req{};
    std::memcpy(&req, req_buf.data(), sizeof(ServeRequest));

    if (req.img_length > MAX_SERVE_IMAGE_BYTES ||
        sizeof(ServeRequest) + req.img_length > req_buf.size()) {
        ServeResponse r{};
        r.ok = 0;
        std::strncpy(r.error, "image too large", sizeof(r.error) - 1);
        send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
        return;
    }
    const uint8_t* img_in = req_buf.data() + sizeof(ServeRequest);

    // ── 3. Inference under engine lock (Vulkan serial constraint) ───────────
    std::vector<uint8_t> out_bytes;
    int out_w = 0, out_h = 0, out_c = req.channels;
    std::string err_msg;
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        if (ensure_model_loaded(req.model_name) != 0) {
            ServeResponse r{};
            r.ok = 0;
            std::strncpy(r.error, "model load failed", sizeof(r.error) - 1);
            send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
            return;
        }
        int scale = engine_->get_scale_factor();
        out_w = req.width * scale;
        out_h = req.height * scale;
        out_bytes.resize(static_cast<size_t>(out_w) * out_h * out_c);

        TileConfig tc;
        tc.tile_size = req.tile_size;
        tc.overlap = req.tile_overlap;
        tc.batch_size = 1;

        std::atomic<bool> cancel{false};
        auto res = processor_->process_image_u8(
            img_in, out_bytes.data(),
            req.width, req.height, req.channels,
            *engine_, tc, {}, cancel, /*enable_alpha_bicubic=*/true);
        if (!res) {
            err_msg = res.error().message;
            out_bytes.clear();
        }
    }

    // ── 4. Send response: payload = [ServeResponse][image] contiguous ───────
    ServeResponse resp{};
    if (err_msg.empty()) {
        resp.ok = 1;
        resp.width = out_w;
        resp.height = out_h;
        resp.channels = out_c;
        resp.img_length = static_cast<uint32_t>(out_bytes.size());
    } else {
        resp.ok = 0;
        std::strncpy(resp.error, err_msg.c_str(), sizeof(resp.error) - 1);
    }

    // Assemble contiguous payload so Protocol::serialize produces one frame.
    size_t payload_len = sizeof(resp) + (resp.ok ? out_bytes.size() : 0);
    std::vector<uint8_t> payload(payload_len);
    std::memcpy(payload.data(), &resp, sizeof(resp));
    if (resp.ok && !out_bytes.empty()) {
        std::memcpy(payload.data() + sizeof(resp), out_bytes.data(), out_bytes.size());
    }
    send_via(*sock, MessageType::MSG_SERVE_RESPONSE, payload.data(), payload.size());
}
```

注意:`send_via`(文件顶部匿名命名空间,约第 30-40 行)原样保留 —— 它调 `Protocol::serialize`,正确处理 checksum 在 offset 11、payload 在 offset 15。

- [ ] **Step 3: 删除 hello() 方法实现**

删除 `daemon.cpp` 末尾的 `ServeDaemon::hello()` 实现(约第 286-293 行,用 `getpid`/`get_scale_factor`/`device_`/`start_time_unix_` 的那段)。

- [ ] **Step 4: 编译验证(完整 CLI 构建)**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/cli && xmake 2>&1 | tail -30
```

Expected: share + cli 全部编译通过。若 main.cpp 报 `MSG_SERVE_HELLO`/`MSG_SERVE_STOP`/`ServeHello` 未定义 —— **这是预期的**(Task 4-6 修)。此步只要 daemon.cpp 本身无错。

若报 `MAX_SERVE_IMAGE_BYTES` 未定义:它在 daemon.cpp 匿名命名空间(约第 27 行),保留不动,确认仍在。

- [ ] **Step 5: Commit**

```bash
git add share/core/serve/daemon.cpp
git commit -m "refactor(serve): simplify handle_client — drop HELLO/STOP, contiguous response payload

- 删步骤 1 发 HELLO
- 删 STOP 分支(stop 改 pid+SIGTERM,Task 5)
- response 改成 [ServeResponse][image] 连续 payload,一次 send_via
- 删 hello() 实现"
```

---

## Task 4: 委托块重写 — 用 Protocol::serialize,删 HELLO 协商

**Files:**
- Modify: `cli/src/main.cpp:413-576`(serve-daemon delegation block)

这是简化最大的一块。现状委托块 ~155 行(含手工 frame 拼装、HELLO 协商、response 二义读法),目标 ~70 行。

- [ ] **Step 1: 重写委托块**

在 `cli/src/main.cpp` 找到注释 `// ── Serve-daemon delegation (single-file only)` 开始的整个 `if (!is_dir && !no_daemon) { ... }` 块(约第 413-576 行),整体替换为:

```cpp
        // ── Serve-daemon delegation (single-file only) ──────────────────────
        // If a local serve daemon is running, send it the image and let the
        // warm engine do inference — skipping the ~490 ms init+load fixed
        // cost. Falls back silently (returns nullopt) if no daemon or the
        // connection fails; on a real daemon-side error returns 1.
        if (!is_dir && !no_daemon) {
            auto delegate = [&]() -> std::optional<int> {
                // 1. Probe: no socket file / connect fails → no daemon, fall back.
                std::string sock_path = crossscale::default_serve_socket_path();
                if (sock_path.empty()) return std::nullopt;
                std::error_code ec;
                if (!std::filesystem::exists(sock_path, ec)) return std::nullopt;

                crossscale::UnixSocket sock;
                auto conn = sock.connect(sock_path);
                if (!conn) return std::nullopt;  // stale socket → no daemon

                // 2. Load image; on failure let the inline path report the error.
                auto img_res = crossscale::Image::load(input_path);
                if (!img_res) return std::nullopt;
                auto& image = img_res.value();
                if (image.size() == 0) return std::nullopt;

                // 3. Build contiguous payload = [ServeRequest][image], one frame.
                crossscale::ServeRequest req{};
                std::memset(req.model_name, 0, sizeof(req.model_name));
                model_name.copy(req.model_name, sizeof(req.model_name) - 1);
                req.tile_size = tile_size;
                req.tile_overlap = tile_overlap;
                req.width = image.width();
                req.height = image.height();
                req.channels = image.channels();
                req.img_length = static_cast<uint32_t>(image.size());

                std::vector<uint8_t> payload(sizeof(req) + image.size());
                std::memcpy(payload.data(), &req, sizeof(req));
                std::memcpy(payload.data() + sizeof(req), image.data(), image.size());

                crossscale::MessageHeader h{};
                h.magic = crossscale::Protocol::MAGIC;
                h.version = crossscale::Protocol::VERSION;
                h.type = crossscale::MessageType::MSG_SERVE_REQUEST;
                h.payload_length = static_cast<uint32_t>(payload.size());
                h.checksum = 0;  // filled by serialize
                auto frame = crossscale::Protocol::serialize(h, payload.data());
                if (!sock.send_all(frame.data(), frame.size())) return std::nullopt;

                // 4. Read response header, then contiguous payload
                //    [ServeResponse][image].
                std::vector<uint8_t> rhdr(crossscale::Protocol::HEADER_SIZE);
                if (!sock.receive_all(rhdr.data(), rhdr.size())) {
                    LOG_ERROR("Engine", "daemon connection lost mid-request");
                    return 1;
                }
                auto rparsed = crossscale::Protocol::parse_header(rhdr.data(), rhdr.size());
                if (!rparsed || rparsed->type != crossscale::MessageType::MSG_SERVE_RESPONSE ||
                    rparsed->payload_length < sizeof(crossscale::ServeResponse)) {
                    LOG_ERROR("Engine", "malformed daemon response");
                    return 1;
                }
                std::vector<uint8_t> rpayload(rparsed->payload_length);
                if (!sock.receive_all(rpayload.data(), rpayload.size())) {
                    LOG_ERROR("Engine", "truncated daemon response");
                    return 1;
                }

                crossscale::ServeResponse resp{};
                std::memcpy(&resp, rpayload.data(), sizeof(resp));
                if (!resp.ok) {
                    LOG_ERROR("Engine", "daemon error: " + std::string(resp.error));
                    return 1;
                }
                const uint8_t* out = rpayload.data() + sizeof(resp);
                size_t out_size = rparsed->payload_length - sizeof(resp);
                if (out_size != resp.img_length) {
                    LOG_ERROR("Engine", "daemon response length mismatch");
                    return 1;
                }

                // 5. Wrap into Image and save (same path as inline single-file).
                crossscale::Image out_image(resp.width, resp.height, resp.channels);
                std::memcpy(out_image.data(), out, out_size);

                crossscale::ProcessingConfig config;
                if (!config_file.empty()) {
                    auto cfg = crossscale::ProcessingConfig::load(config_file);
                    if (cfg) config = cfg.value();
                }
                if (cli_output_quality >= 0) config.output_quality = cli_output_quality;
                if (!cli_output_format.empty()) config.output_format = cli_output_format;

                auto final_path = crossscale::resolve_output_path(output_path, config);
                if (!final_path.parent_path().empty()) {
                    std::filesystem::create_directories(final_path.parent_path());
                }
                auto save_res = out_image.save(final_path, config.output_quality);
                if (!save_res) {
                    LOG_ERROR("Engine", "Failed to save: " + save_res.error().message);
                    return 1;
                }
                LOG_INFO("Engine", "Output saved (via warm daemon): " + final_path.string());
                LOG_INFO("Engine", "Processing complete: " + std::to_string(resp.width) +
                                   "x" + std::to_string(resp.height));
                return 0;
            };

            auto delegated = delegate();
            if (delegated.has_value()) return delegated.value();
            // else: no daemon / probe failed → fall through to inline path
        }
```

- [ ] **Step 2: 编译验证**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/cli && xmake 2>&1 | tail -30
```

Expected: 委托块本身编译通过。`serve` 子命令(start/stop/status,约第 2356-2490 行)会因引用 `MSG_SERVE_HELLO`/`MSG_SERVE_STOP`/`ServeHello` 报错 —— **这是预期的**(Task 5-6 修)。此步只要委托块无错。

- [ ] **Step 3: Commit**

```bash
git add cli/src/main.cpp
git commit -m "refactor(serve): rewrite delegation block with Protocol::serialize

- 删手工 frame 拼装 + checksum_slot 偏移数学(~17 行)
- 删 HELLO 读 + 模型协商(~10 行)
- 删 response 'extra 字节' 死代码分支(~20 行)
- payload/request/response 全部用 Protocol::serialize 一次成帧
- 委托块 155→70 行

serve 子命令引用在 Task 5-6 修复。"
```

---

## Task 5: serve start/stop 重写 — 单 fork + pid 文件 + SIGTERM

**Files:**
- Modify: `cli/src/main.cpp:2356-2490`(整个 `if (command == "serve")` 块)

- [ ] **Step 1: 重写整个 serve 子命令块**

找到 `if (command == "serve") {` 到对应的闭合 `}`(约第 2356-2490 行)。整体替换为:

```cpp
    if (command == "serve") {
        std::string subcommand = argc > 2 ? argv[2] : "start";
        std::string model_name;
        std::string models_dir = "./models";
        std::string socket_path = crossscale::default_serve_socket_path();
        bool force_cpu = false;
        bool foreground = false;

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--model" && i + 1 < argc) model_name = argv[++i];
            else if (arg == "--models-dir" && i + 1 < argc) models_dir = argv[++i];
            else if (arg == "--socket" && i + 1 < argc) socket_path = argv[++i];
            else if (arg == "--cpu") force_cpu = true;
            else if (arg == "--fg") foreground = true;
            else if (arg == "--help" || arg == "-h") {
                std::cout << "CrossScale serve - local warm-engine daemon\n\n"
                          << "Usage:\n"
                          << "  crossscale serve start [--model <name>] [--socket <path>] [--cpu] [--fg]\n"
                          << "  crossscale serve stop  [--socket <path>]\n\n"
                          << "Holds a warm ncnn engine so that `crossscale process` single-image calls\n"
                          << "reuse it and skip the ~490ms init+load fixed cost (~4x faster).\n\n"
                          << "Options:\n"
                          << "  --model <name>   Initial model (default: first available)\n"
                          << "  --socket <path>  Unix socket path (default: " << crossscale::default_serve_socket_path() << ")\n"
                          << "  --cpu            Force CPU (default: GPU with CPU fallback)\n"
                          << "  --fg             Run in foreground (default: background)\n\n"
                          << "Status: `pgrep -af 'crossscale serve'`\n";
                return 0;
            }
        }

        if (socket_path.empty()) {
            LOG_ERROR("Serve", "no default socket path available (set XDG_CACHE_HOME or HOME)");
            return 1;
        }

        std::string pid_path = socket_path + ".pid";

        if (subcommand == "stop") {
            // Read pid, send SIGTERM. Daemon's signal handler unlinks socket+pid.
            std::ifstream pf(pid_path);
            if (!pf) {
                LOG_ERROR("Serve", "no pid file at " + pid_path + " (daemon not running?)");
                return 1;
            }
            pid_t pid;
            pf >> pid;
            if (::kill(pid, SIGTERM) < 0) {
                LOG_ERROR("Serve", std::string("kill pid ") + std::to_string(pid) +
                                   " failed: " + std::strerror(errno));
                // Stale pid file: clean it up so next start isn't confused.
                if (errno == ESRCH) std::filesystem::remove(pid_path);
                return 1;
            }
            LOG_INFO("Serve", "stopped pid " + std::to_string(pid));
            return 0;
        }

        if (subcommand != "start") {
            LOG_ERROR("Serve", "unknown subcommand: " + subcommand + " (try start|stop)");
            return 1;
        }

        // ── start ───────────────────────────────────────────────────────────
        // Guard: if a pid file exists and the pid is alive, refuse (already running).
        {
            std::ifstream pf(pid_path);
            if (pf) {
                pid_t existing;
                pf >> existing;
                if (existing > 0 && ::kill(existing, 0) == 0) {
                    LOG_ERROR("Serve", "daemon already running, pid " + std::to_string(existing));
                    return 1;
                }
                // pid file present but process dead → stale, overwrite below.
            }
        }

        // Background via single fork + setsid (detaches controlling terminal).
        if (!foreground) {
            pid_t pid = ::fork();
            if (pid < 0) { LOG_ERROR("Serve", "fork failed"); return 1; }
            if (pid > 0) return 0;  // parent exits
            ::setsid();
            ::close(STDIN_FILENO);
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
        }

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        crossscale::ServeDaemon daemon;
        int rc = daemon.start(socket_path, models_dir, model_name,
                              force_cpu ? crossscale::ServeDevice::CPU
                                        : crossscale::ServeDevice::GPU);
        if (rc != 0) {
            std::filesystem::remove(pid_path);
            return rc;
        }

        // Write pid file after successful start (so stop can find us).
        std::ofstream(pid_path) << ::getpid();

        daemon.run();  // returns when stop_requested_ is set (signal)

        // Cleanup on shutdown: daemon destructor unlinks the socket; we unlink pid.
        std::filesystem::remove(pid_path);
        LOG_INFO("Serve", "daemon shutting down");
        return 0;
    }
```

**关键变化对比现状**:
- 删 `serve status` 整块(约 30 行)。
- `stop`:从读 HELLO + 发 MSG_SERVE_STOP(~17 行)→ 读 pid + kill SIGTERM(~12 行)。
- `start`:从双 fork(~17 行)→ 单 fork(~8 行);新增 pid 文件写入 + 已运行 guard。

- [ ] **Step 2: 确认 includes 齐全**

`cli/src/main.cpp` 顶部需有(检查已有):`<csignal>`(signal_handler/SIGTERM)、`<unistd.h>`(fork/setsid/getpid/kill)、`<fstream>`(ofstream/ifstream 读 pid)、`<cerrno>`(errno)、`<cstring>`(strerror)。若缺则补。`#include <fstream>` 很可能已存在(其他子命令可能用),grep 确认:

```bash
grep -n "#include <fstream>\|#include <csignal>\|#include <unistd.h>\|#include <cerrno>" cli/src/main.cpp
```

- [ ] **Step 3: 编译验证(完整构建)**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/cli && xmake 2>&1 | tail -30
```

Expected: 全部编译通过(share + cli)。无 `MSG_SERVE_HELLO`/`MSG_SERVE_STOP`/`ServeHello` 残留引用。

- [ ] **Step 4: Commit**

```bash
git add cli/src/main.cpp
git commit -m "refactor(serve): pid file + SIGTERM lifecycle, drop status subcommand

- start: 单 fork + setsid(替代双 fork);写 pid 文件;已运行 guard
- stop: 读 pid + kill SIGTERM(替代 MSG_SERVE_STOP 消息)
- 删 serve status 子命令(用 pgrep 替代)
- SIGTERM/SIGINT 经现有 signal_handler 设 g_running,daemon.run() 观测退出"
```

---

## Task 6: 清理 daemon.cpp 残留 includes + 用法文档更新

**Files:**
- Modify: `share/core/serve/daemon.cpp`(顶部 includes)
- Modify: `docs/PERF_REPORT_REAL_IMAGES.md` §9(serve 用法)

- [ ] **Step 1: 清理 daemon.cpp 不再需要的 includes**

Task 3 后,`daemon.cpp` 可能残留未用的 include。检查:

```bash
grep -n "#include" share/core/serve/daemon.cpp
```

`<csignal>` —— 检查:daemon.cpp 自己不直接调 signal(在 main.cpp 注册),但 `stop()` 被信号 handler 间接调用。若 daemon.cpp 无 `std::signal`/`std::raise` 调用,删除 `<csignal>`。grep 确认:

```bash
grep -n "signal\|raise(" share/core/serve/daemon.cpp
```

若无匹配,删除 `#include <csignal>`。

`<chrono>` —— Task 3 Step 1 已删 start_time_unix_ 初始化,若 `<chrono>` 无其他用处则删。grep 确认:

```bash
grep -n "chrono\|system_clock\|duration" share/core/serve/daemon.cpp
```

若无匹配,删除 `#include <chrono>`。

- [ ] **Step 2: 重新编译确认无警告**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/cli && xmake -r 2>&1 | tail -30
```

Expected: 全量重建,无未用 include 警告,无错误。

- [ ] **Step 3: 更新 PERF_REPORT_REAL_IMAGES.md §9 serve 用法**

找到 `docs/PERF_REPORT_REAL_IMAGES.md` 里 serve 用法小节(若有 `crossscale serve status` 命令,删除该行)。把用法示例更新为:

```bash
# 启动守护进程(后台,默认 GPU;--fg 前台)
crossscale serve start
# 现在 crossscale process 单图自动复用热引擎(2-15x 加速)

# 停止
crossscale serve stop

# 查看是否在运行
pgrep -af 'crossscale serve'

# 强制单图走内联路径(不委托守护进程)
crossscale process -i in.png -o out.png --no-daemon
```

(若文档无 serve 小节,在 §9 末尾追加。)

- [ ] **Step 4: Commit**

```bash
git add share/core/serve/daemon.cpp docs/PERF_REPORT_REAL_IMAGES.md
git commit -m "chore(serve): clean unused includes, update usage docs

- 删 daemon.cpp 残留 <csignal>/<chrono>(简化后不再需要)
- §9 serve 用法更新为 pid+SIGTERM 生命周期,删 status 示例"
```

---

## Task 7: 验证 — 正确性 + 加速 + 回退 + 生命周期 + 并发 + lazy reload

**Files:** 无(纯运行验证)

前置:需要测试图。若 `/tmp/srtest/` 不存在,用 `scripts/gen_test_images.py` 生成(确定性,seed 42):

```bash
ls /tmp/srtest/*.png 2>/dev/null || python3 scripts/gen_test_images.py /tmp/srtest
```

- [ ] **Step 1: 启动守护进程(前台,便于观察)**

Run:
```bash
cd /home/xihale/Desktop/learning/android/crossscale/cli
# 前台启动一个终端;或后台:
./build/linux/x86_64/release/crossscale-cli serve start --fg &
sleep 2
ls -la ~/.cache/crossscale/serve.sock ~/.cache/crossscale/serve.sock.pid
```

Expected: socket 文件 + pid 文件都存在。pid 文件内容是守护进程 pid。

- [ ] **Step 2: 正确性验证 — 守护进程输出 vs inline 输出,md5 逐字节相同**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
OUT=/tmp/srtest/verify_simplify
mkdir -p $OUT/daemon $OUT/inline
for img in /tmp/srtest/photo_256.png /tmp/srtest/text_400.png /tmp/srtest/anime_512.png; do
  base=$(basename $img .png)
  $CLI process -i $img -o $OUT/daemon/$base.png            # 走守护进程
  $CLI process -i $img -o $OUT/inline/$base.png --no-daemon # 强制 inline
  echo "$base: daemon=$(md5sum $OUT/daemon/$base.png | cut -d' ' -f1) inline=$(md5sum $OUT/inline/$base.png | cut -d' ' -f1)"
done
```

Expected: 每行 daemon 和 inline 的 md5 **相同**(逐字节一致)。若有不同 → 严重 bug,停,排查(检查 lazy reload 是否改变了某全局状态、检查 out_size 校验逻辑)。

- [ ] **Step 3: 加速验证 — wall time 对比**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
for img in /tmp/srtest/photo_256.png /tmp/srtest/photo_1024.png; do
  base=$(basename $img .png)
  echo "=== $base ==="
  echo -n "daemon: "; /usr/bin/time -f "%e s" $CLI process -i $img -o /tmp/$base.daemon.png 2>&1 | tail -1
  echo -n "inline: "; /usr/bin/time -f "%e s" $CLI process -i $img -o /tmp/$base.inline.png --no-daemon 2>&1 | tail -1
done
```

Expected: daemon 明显快于 inline(photo_256: daemon ~0.04s vs inline ~0.5s;photo_1024: ~0.21s vs ~0.88s)。加速比应与简化前持平(2-15×)。

- [ ] **Step 4: 回退验证 — stop 后 process 静默回退 inline**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
$CLI serve stop
sleep 1
ls ~/.cache/crossscale/serve.sock 2>&1   # 应:No such file
ls ~/.cache/crossscale/serve.sock.pid 2>&1   # 应:No such file
# 此时无守护进程,process 应回退 inline
$CLI process -i /tmp/srtest/photo_256.png -o /tmp/fallback.png
md5sum /tmp/fallback.png /tmp/photo_256.inline.png 2>/dev/null
# 若有之前的 inline md5 参照,应一致
echo "fallback OK if process succeeded and no daemon-related error"
```

Expected: socket 和 pid 文件都被清理;`process` 正常完成(走 inline 路径),无守护进程相关报错。

- [ ] **Step 5: 生命周期验证 — stale pid/socket 处理**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
# 5a. 残留 pid 文件(pid 已死)→ start 应覆盖
echo "99999" > ~/.cache/crossscale/serve.sock.pid   # 假 pid(不存在)
$CLI serve start
sleep 2
cat ~/.cache/crossscale/serve.sock.pid   # 应是新真实 pid,非 99999
$CLI serve stop
sleep 1
# 5b. 残留 socket 文件(无守护进程)→ 客户端应静默回退
touch ~/.cache/crossscale/serve.sock   # 假 socket(无人 listen)
$CLI process -i /tmp/srtest/photo_256.png -o /tmp/stale.png
echo "stale socket fallback OK if process succeeded"
rm -f ~/.cache/crossscale/serve.sock
# 5c. 已运行 guard → start 应拒绝
$CLI serve start &
sleep 2
$CLI serve start   # 应报 "already running"
$CLI serve stop
sleep 1
```

Expected:
- 5a: start 覆盖假 pid,启动成功。
- 5b: 客户端 connect 失败 → 静默回退 inline,正常输出。
- 5c: 第二次 start 报 "daemon already running",不启动第二个。

- [ ] **Step 6: 并发验证 — 3 客户端同时跑,engine_mutex_ 串行**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
$CLI serve start
sleep 2
# 同时起 3 个,各自处理不同图
for img in photo_256 text_400 anime_512; do
  $CLI process -i /tmp/srtest/$img.png -o /tmp/conc_$img.png &
done
wait
# 全部应成功,且 md5 与 inline 一致
for img in photo_256 text_400 anime_512; do
  $CLI process -i /tmp/srtest/$img.png -o /tmp/conc_${img}_inline.png --no-daemon
  echo "$img: $(md5sum /tmp/conc_$img.png | cut -d' ' -f1) vs $(md5sum /tmp/conc_${img}_inline.png | cut -d' ' -f1)"
done
$CLI serve stop
```

Expected: 3 个并发请求都成功完成,md5 与各自 inline 输出一致。守护进程存活(stop 能正常停)。

- [ ] **Step 7: lazy reload 验证 — 请求不同模型触发守护进程重载**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
# 列出可用模型
ls models/*.param 2>/dev/null || ls /tmp/srtest/models/*.param 2>/dev/null
# 假设有两个模型 A 和 B
MODEL_A=$(ls models/*.param 2>/dev/null | head -1 | xargs -I{} basename {} .param)
MODEL_B=$(ls models/*.param 2>/dev/null | sed -n 2p | xargs -I{} basename {} .param)
if [ -z "$MODEL_B" ]; then echo "SKIP: only one model available"; exit 0; fi

# 以 A 启动
$CLI serve start --model $MODEL_A
sleep 2
# 请求 B(守护进程应 lazy reload)
$CLI process -i /tmp/srtest/photo_256.png -o /tmp/reload_B.png -m $MODEL_B
# 请求 A(应无 reload,用已加载)
$CLI process -i /tmp/srtest/photo_256.png -o /tmp/reload_A.png -m $MODEL_A
$CLI serve stop
echo "lazy reload OK if both succeeded"
```

Expected: 两个请求都成功(若仅一个模型则 SKIP)。守护进程日志应显示 "switched model" 一次(B 请求时)。

- [ ] **Step 8: 最终清理**

Run:
```bash
CLI=./build/linux/x86_64/release/crossscale-cli
$CLI serve stop 2>/dev/null
pkill -9 -f 'crossscale.*serve' 2>/dev/null
rm -f ~/.cache/crossscale/serve.sock ~/.cache/crossscale/serve.sock.pid
ls ~/.cache/crossscale/ 2>/dev/null
git status --short   # 应只有 docs/ 下可能的验证产物(无),工作树干净
```

Expected: 无残留守护进程,无残留 socket/pid 文件,工作树干净(或仅有验证输出文件,应清理)。

---

## Self-Review

**Spec coverage** —— 对照 spec 各节:
- §协议设计(MSG_SERVE_REQUEST/RESPONSE + 连续 payload)→ Task 1(类型/结构体)+ Task 3(daemon 侧)+ Task 4(客户端侧)。✓
- §守护进程生命周期(pid+SIGTERM,单 fork,删 status)→ Task 5。✓
- §ServeDaemon 类瘦身(删 hello/start_time_unix_)→ Task 2+3。✓
- §CLI 委托块瘦身 → Task 4。✓
- §不做的事(UnixSocket 不动等)→ 各 Task 均未触及。✓
- §验证计划(6 项)→ Task 7 全覆盖(正确性/加速/回退/生命周期/并发/lazy reload)。✓
- §文件变更清单 → Task 1-6 逐文件对应。✓

**Placeholder scan** —— 无 TODO/TBD/"implement later"/"add error handling"。每步含具体代码或具体命令。✓

**Type consistency** ——
- `ServeRequest`/`ServeResponse` 字段名在 Task 1(定义)、Task 3(daemon 读写)、Task 4(客户端读写)一致。✓
- `MSG_SERVE_REQUEST`/`MSG_SERVE_RESPONSE` 在 Task 1(定义)、3、4、5 一致。✓
- `signal_handler`/`g_running`(Task 5 用)—— 这是 main.cpp 现有的全局(第 45、48 行确认),非新引入。✓
- `crossscale::default_serve_socket_path()`、`crossscale::ServeDaemon::start/run`、`ServeDevice` 在 Task 4、5 用,均在 daemon.hpp 已有(简化保留)。✓
- `send_via`(Task 3 daemon 用)—— daemon.cpp 匿名命名空间已有,保留不动。✓

无问题,plan 可执行。
