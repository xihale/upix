# 第1层：外壳清理残留 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除当前 HEAD 上仅存的死代码（`ConnectionInfo` 死 struct + `MSG_IMAGE_TILE` 死枚举），并把 PC/Android 两处手写的 share 源文件枚举改为自动收集，消除新增 `.cpp` 时的双向漂移。

**Architecture:** 纯删除 + 构建脚本重构，不改任何运行时逻辑。每个任务独立编译验证、独立提交。所有删除项已通过 grep 在 HEAD `475b151` 上实测确认零外部引用。

**Tech Stack:** C++23 (xmake, PC) / C++17 (CMake/NDK, Android) / xmake.lua / CMake

---

## 背景与重要说明

原始 spec（`docs/superpowers/specs/2026-07-28-architecture-simplification-design.md`）的 §1
引用了 `docs/PONYTAIL_AUDIT.md` 的 26 项死代码。但 2026-07-28 逐项核查发现 **audit 报告
基于旧版本，绝大多数项早已删除**：

| Audit 声称的死代码 | HEAD `475b151` 实际 |
|---|---|
| crypto.cpp 死函数 ~150行 | 已瘦身至 3 函数全在用 |
| ModelManager 死方法 ~90行 | 接口已无这些方法 |
| ncnn_engine.hpp 死壳 ~65行 | 文件已删 |
| expected.hpp 60行手写 | 已是 std::expected 别名（13行） |
| config.cpp 手写 JSON | 已用 glaze |
| logger.hpp 死配置 ~120行 | 已清理（138行，4级，无CRITICAL/set_*） |
| benchmark PerfStats ~30行 | 已删 |
| MessageHandler.kt / parseLegacyInvite | 已删 |
| MessageType 12个死枚举 | 只剩 1 个（MSG_IMAGE_TILE） |
| cxxopts / Ktor 死依赖 | 已从构建文件删除 |
| set_models_directory / unregister_device / get_memory_info | **audit 事实错误，实际在用，保留** |

**因此第1层只剩下 3 项真实工作**（本计划的 Task 1-3）。构建统一（Task 3）是其中
唯一有实质改进的项——它解决"新增 .cpp 需改两处"的真实维护痛点。

---

## Task 1: 删除 `MSG_IMAGE_TILE` 死枚举

`MSG_IMAGE_TILE = 0x0401` 在 `protocol.hpp` 声明，但全仓库 0 处使用
（`MSG_TILES_BATCH = 0x0403` 才是实际使用的批量 tile 消息）。

**Files:**
- Modify: `share/core/distributed/protocol.hpp:27`

- [ ] **Step 1: 确认零引用（执行时复核，防止过时）**

Run:
```bash
grep -rn "MSG_IMAGE_TILE" --include="*.cpp" --include="*.hpp" --include="*.kt" share/ cli/ apps/app/src/main/cpp/ apps/app/src/main/java/ 2>/dev/null | grep -v third_party
```
Expected: 只命中 `protocol.hpp` 的定义行（1 行）。若有其他命中，**停止**——说明已被启用，本任务取消。

- [ ] **Step 2: 删除枚举行**

在 `share/core/distributed/protocol.hpp`，删除这一行：

```cpp
    MSG_IMAGE_TILE = 0x0401,
```

（位于 `MSG_TASK_RESULT = 0x0304,` 之后、`MSG_TILES_BATCH = 0x0403,` 之前。
注意保留 `MSG_TILES_BATCH`，它在用。）

- [ ] **Step 3: 编译验证**

Run:
```bash
cd cli && xmake
```
Expected: 编译成功，无错误。

- [ ] **Step 4: 提交**

```bash
git add share/core/distributed/protocol.hpp
git commit -m "refactor(protocol): drop unused MSG_IMAGE_TILE enum (0x0401)

MSG_TILES_BATCH (0x0403) is the actual batch-tile message in use;
MSG_IMAGE_TILE had zero references."
```

---

## Task 2: 删除 `ConnectionInfo` 死 struct 及其序列化方法

`ConnectionInfo` struct（`coordinator.hpp:29-41`）+ `glz::meta` 特化（`coordinator.hpp:46-57`）
+ 4 个序列化方法（`protocol.cpp:65-102`）**整体零外部使用**。CLI 的 QR 生成
（`cmd_server.cpp` 的 `print_connection_card`）自己手拼 JSON，不走这个 struct。

**Files:**
- Modify: `share/core/distributed/coordinator.hpp:29-57`（删 struct + glz::meta）
- Modify: `share/core/distributed/protocol.cpp:65-102`（删 4 个方法定义）
- Modify: `share/core/distributed/protocol.cpp:6`（可能删 `<glaze/json.hpp>` include）

- [ ] **Step 1: 确认 ConnectionInfo 零外部引用**

Run:
```bash
grep -rn "ConnectionInfo" --include="*.cpp" --include="*.hpp" --include="*.kt" share/ cli/ apps/app/src/main/cpp/ apps/app/src/main/java/ 2>/dev/null | grep -v third_party | grep -v "coordinator.hpp\|protocol.cpp"
```
Expected: **空输出**（零外部引用）。若有命中，**停止**——说明已被启用。

- [ ] **Step 2: 确认 to_json/from_json/to_short_string/from_short_string 零外部调用**

Run:
```bash
grep -rn "\.to_json()\|\.from_json(\|\.to_short_string()\|\.from_short_string(" --include="*.cpp" --include="*.hpp" share/ cli/ 2>/dev/null | grep -v third_party | grep -v "protocol.cpp"
```
Expected: **空输出**。

- [ ] **Step 3: 删除 coordinator.hpp 中的 struct 与 glz::meta**

在 `share/core/distributed/coordinator.hpp`，删除从 `struct ConnectionInfo {`
到 `glz::meta` 特化结束的整块（即 `// JSON key 映射...` 注释及其后的 template 块）。
具体删除 `coordinator.hpp:29-57` 这 29 行：

```cpp
struct ConnectionInfo {
    int version = 1;
    std::string ip;
    uint16_t port{};
    std::string auth_key;
    std::string server_name;
    std::string current_model;

    auto to_json() const -> std::string;
    static auto from_json(const std::string& json) -> expected<ConnectionInfo, CoordinatorError>;
    auto to_short_string() const -> std::string;
    static auto from_short_string(const std::string& str) -> expected<ConnectionInfo, CoordinatorError>;
};

} // namespace crossscale

// JSON key 映射：保持与 Android JoinInvite 解析一致的历史字段名
template <>
struct glz::meta<crossscale::ConnectionInfo> {
    using T = crossscale::ConnectionInfo;
    static constexpr auto value = glz::object(
        "v", &T::version,
        "ip", &T::ip,
        "port", &T::port,
        "key", &T::auth_key,
        "name", &T::server_name,
        "model", &T::current_model
    );
};

namespace crossscale {
```

删除后，`coordinator.hpp` 中 `CoordinatorError` struct 之后应直接接 `struct WorkerConnection`
（原本被 `} // namespace crossscale` ... `namespace crossscale {` 包裹的 ConnectionInfo 区块
整个移除，需保证前后命名空间大括号仍配平）。

删除后该区域应为：
```cpp
struct CoordinatorError {
    int code;
    std::string message;
};

struct WorkerConnection {
    std::unique_ptr<TCPSocket> socket;
    DeviceProfile profile;
    std::string device_id;
    ...
```

- [ ] **Step 4: 删除 protocol.cpp 中的 4 个方法定义**

在 `share/core/distributed/protocol.cpp`，删除 `ConnectionInfo::to_json`、`from_json`、
`to_short_string`、`from_short_string` 四个方法定义（`protocol.cpp:65-102`，共 38 行）。

具体删除这一整块：
```cpp
auto ConnectionInfo::to_json() const -> std::string {
    std::string buf;
    (void)glz::write_json(*this, buf); // POD 序列化不会失败
    return buf;
}

auto ConnectionInfo::from_json(const std::string& json)
    -> expected<ConnectionInfo, CoordinatorError> {

    ConnectionInfo info;
    auto ec = glz::read_json(info, json);
    if (ec) {
        return std::unexpected(CoordinatorError{1, "Failed to parse connection info"});
    }
    return info;
}

auto ConnectionInfo::to_short_string() const -> std::string {
    return ip + ":" + std::to_string(port) + "#" + auth_key;
}

auto ConnectionInfo::from_short_string(const std::string& str)
    -> expected<ConnectionInfo, CoordinatorError> {
    
    auto colon_pos = str.find(':');
    auto hash_pos = str.find('#');
    
    if (colon_pos == std::string::npos || hash_pos == std::string::npos) {
        return std::unexpected(CoordinatorError{1, "Invalid connection string format"});
    }
    
    ConnectionInfo info;
    info.ip = str.substr(0, colon_pos);
    info.port = static_cast<uint16_t>(std::stoi(str.substr(colon_pos + 1, hash_pos - colon_pos - 1)));
    info.auth_key = str.substr(hash_pos + 1);
    
    return info;
}

```

删除后，`Protocol::compute_checksum` 之后应直接是 `} // namespace crossscale`。

- [ ] **Step 5: 检查 protocol.cpp 的 glaze include 是否还需要**

Run:
```bash
grep -n "glz::" share/core/distributed/protocol.cpp
```
Expected: 若 Step 4 后已无 `glz::` 引用（只剩 include），则删除 `protocol.cpp:6` 的
`#include <glaze/json.hpp>`。若仍有其他 glz:: 使用，保留 include。

- [ ] **Step 6: 编译验证**

Run:
```bash
cd cli && xmake
```
Expected: 编译成功。若报 `glz::meta<ConnectionInfo>` 未定义之类错误，说明有遗漏的
引用——回到 Step 1 复查。

- [ ] **Step 7: 运行时行为验证（可选但推荐）**

确认 `crossscale server start` 的 QR/connection card 仍正常打印（它不走 ConnectionInfo，
而是 `print_connection_card` 手拼 JSON）：

Run:
```bash
cd cli && timeout 2 xmake run crossscale-cli server start --fg 2>&1 | head -20 || true
```
Expected: 正常打印连接信息（IP/端口/key/QR），无崩溃。

- [ ] **Step 8: 提交**

```bash
git add share/core/distributed/coordinator.hpp share/core/distributed/protocol.cpp
git commit -m "refactor(protocol): drop unused ConnectionInfo struct and its serializers

ConnectionInfo + glz::meta + to_json/from_json/to_short_string/from_short_string
had zero external references. CLI's print_connection_card builds its own JSON
for the QR code; nothing constructs or reads ConnectionInfo."
```

---

## Task 3: 构建系统统一 —— PC xmake.lua 改用 glob 自动收集

当前 `share/xmake.lua` 手写枚举 15 个 `.cpp`。新增/删除源文件需手动同步，易漏。
改为 `os.files` 自动 glob，配合平台门控。

**Files:**
- Modify: `share/xmake.lua:7-24`

- [ ] **Step 1: 记录当前手写清单（作为 glob 后的基线对照）**

Run:
```bash
sed -n '7,24p' share/xmake.lua
```
记录输出的 15 个文件路径。glob 完成后，编译产物应包含完全相同的文件集（用 `xmake -v` 复核）。

- [ ] **Step 2: 改写 share/xmake.lua 的 add_files 为 glob**

将 `share/xmake.lua` 中这一段：

```lua
    add_files(
        "core/ncnn_engine.cpp",
        "core/tile_processor.cpp",
        "core/model_manager.cpp",
        "core/config.cpp",
        "core/engine_pool.cpp",
        "core/distributed/scheduler.cpp",
        "core/distributed/worker.cpp",
        "core/distributed/coordinator.cpp",
        "core/distributed/protocol.cpp",
        "utils/image.cpp",
        "utils/network.cpp",
        "utils/benchmark.cpp",
        "utils/crypto.cpp",
        "utils/mdns.cpp",
        "core/serve/daemon.cpp"
    )
```

替换为：

```lua
    -- Auto-collect all share sources. New .cpp files are picked up automatically;
    -- no need to edit this list when adding files. Platform gating handled below.
    add_files(
        "core/*.cpp",
        "core/distributed/*.cpp",
        "core/serve/*.cpp",
        "utils/*.cpp"
    )
```

- [ ] **Step 3: 复核平台门控逻辑仍正确**

原有的平台门控块（紧跟 add_files 之后）应保持不变：

```lua
    if is_plat("android") then
        add_files("utils/android_utils.cpp")
    end

    -- AF_UNIX serve daemon is Linux/macOS only. The UnixSocket helper compiles
    -- only on POSIX-with-AF_UNIX platforms; on others the daemon source is a no-op stub.
    if is_plat("linux", "macosx") then
        add_files("utils/unix_socket.cpp")
    end
```

确认 `utils/android_utils.cpp` 和 `utils/unix_socket.cpp` 不被上面的 glob 误纳入其他平台
（它们本身在 `utils/*.cpp` glob 范围内，但靠 `is_plat` 门控只在对的平台编译——
这里有个**陷阱**：`utils/*.cpp` 会把 `unix_socket.cpp` 列入所有平台的文件集，
只是它在非 linux/macosx 平台编译会失败）。

**修正方案**：glob 时排除这两个平台专属文件，让 `is_plat` 块单独添加。改为：

```lua
    -- Auto-collect share sources, excluding platform-specific files handled below.
    add_files(
        "core/*.cpp",
        "core/distributed/*.cpp",
        "core/serve/*.cpp",
        "utils/*.cpp|unix_socket.cpp|android_utils.cpp"
    )

    if is_plat("android") then
        add_files("utils/android_utils.cpp")
    end

    if is_plat("linux", "macosx") then
        add_files("utils/unix_socket.cpp")
    end
```

（xmake 的 `|` 语法在 add_files 中表示排除模式。若该语法不被支持，改用
`add_files("utils/*.cpp")` 后用 `remove_files("utils/unix_socket.cpp", "utils/android_utils.cpp")`
再按平台加回。执行时验证哪种生效。）

- [ ] **Step 4: 编译并核对文件集**

Run:
```bash
cd cli && xmake clean && xmake -v 2>&1 | grep -E "\.cpp" | sort
```
Expected: 编译的 `.cpp` 文件集与 Step 1 记录的基线一致（在 linux 上应含 `unix_socket.cpp`，
不含 `android_utils.cpp`）。

- [ ] **Step 5: 完整运行验证**

Run:
```bash
cd cli && xmake run crossscale-cli --help
```
Expected: 正常打印帮助，无错误。

- [ ] **Step 6: 提交**

```bash
git add share/xmake.lua
git commit -m "build(share): auto-collect sources via glob in xmake.lua

Replace hand-maintained 15-file list with core/**, distributed/**, serve/**,
utils/** globs. Platform-specific files (unix_socket, android_utils) are
excluded from the glob and added via is_plat gates. New .cpp files no longer
require editing this list."
```

---

## Task 4: 构建系统统一 —— Android CMakeLists.txt 改用 glob

镜像 Task 3，把 Android 的 `apps/app/src/main/cpp/CMakeLists.txt` 中手写的
share 源文件枚举（117-135行）改为 `file(GLOB ...)`。

**Files:**
- Modify: `apps/app/src/main/cpp/CMakeLists.txt:115-135`

- [ ] **Step 1: 记录当前手写清单**

Run:
```bash
sed -n '115,141p' apps/app/src/main/cpp/CMakeLists.txt
```
记录其中的 share `.cpp` 文件列表（约 9 个文件 + ncnn/stub 条件分支）。

- [ ] **Step 2: 改写为 file(GLOB)**

将 `apps/app/src/main/cpp/CMakeLists.txt` 中的 share 源文件枚举块
（`# Source files from shared core` 注释下到 `add_library` 之前）替换为：

```cmake
# Source files from shared core — auto-collected. Mirrors share/xmake.lua.
# Note: file(GLOB) requires re-running CMake configure when files are added/removed.
set(SHARE_DIR ${CMAKE_SOURCE_DIR}/../../../../../share)

file(GLOB SHARE_CORE_SRCS
    ${SHARE_DIR}/core/*.cpp
    ${SHARE_DIR}/core/distributed/*.cpp
)
file(GLOB SHARE_UTILS_SRCS
    ${SHARE_DIR}/utils/*.cpp
)
# Exclude platform-specific files handled below.
list(FILTER SHARE_UTILS_SRCS EXCLUDE REGEX "unix_socket\\.cpp$")
list(FILTER SHARE_UTILS_SRCS EXCLUDE REGEX "android_utils\\.cpp$")
```

然后 `add_library` 处把原来的手写列表替换为变量引用，保留 ncnn/stub 条件：

```cmake
# ncnn engine: real impl when CROSSSCALE_USE_NCNN, stub otherwise.
if(CROSSSCALE_USE_NCNN)
    set(ENGINE_SRCS
        ${SHARE_DIR}/core/ncnn_engine.cpp
        ${SHARE_DIR}/core/tile_processor.cpp
    )
else()
    set(ENGINE_SRCS ${CMAKE_SOURCE_DIR}/ncnn_engine_stub_android.cpp)
endif()

add_library(crossscale-native SHARED
    ${SHARE_CORE_SRCS}
    ${SHARE_UTILS_SRCS}
    ${ENGINE_SRCS}
    ${CMAKE_SOURCE_DIR}/jni_bridge.cpp
)
```

**注意**：原清单中 `config.cpp`/`model_manager.cpp`/`protocol.cpp`/`scheduler.cpp`/
`worker.cpp`/`image.cpp`/`benchmark.cpp`/`network.cpp`/`crypto.cpp` 都在 glob 范围内，
会被自动收集。`coordinator.cpp` 原清单没有（Android 不做 coordinator）——确认这一点：

- [ ] **Step 3: 确认 Android 是否需要 coordinator.cpp**

原手写清单不含 `coordinator.cpp`。但 glob 会把它收集进来。需判断：
- 若 Android 确实不链接 coordinator（只做 worker/client），需排除：
  ```cmake
  list(FILTER SHARE_CORE_DIST_SRCS EXCLUDE REGEX "coordinator\\.cpp$")
  ```
- 若 Android 其实可以编入 coordinator（无副作用），则不排除，让其自动纳入。

Run（判断 coordinator 是否被 Android 代码引用）:
```bash
grep -rn "Coordinator\|coordinator.hpp" apps/app/src/main/cpp/ 2>/dev/null | grep -v third_party
```
若零命中，说明 Android 不用 coordinator，**排除它**（保持原有为行为）。

- [ ] **Step 4: 验证 Gradle 构建**

Run:
```bash
cd apps && ./gradlew :app:externalNativeBuildDebug 2>&1 | tail -20
```
Expected: NDK 编译成功。若失败，检查 glob 路径与排除规则。

> **若环境无 Android NDK / 无法构建**：本任务可标记为"待 Android 环境验证"，
> 先提交 PC 侧（Task 3），Android 侧留作后续。在 commit message 注明。

- [ ] **Step 5: 提交**

```bash
git add apps/app/src/main/cpp/CMakeLists.txt
git commit -m "build(android): auto-collect share sources via file(GLOB)

Replace hand-maintained source list with file(GLOB) over share/core,
share/utils. Mirrors the PC xmake.lua glob. New .cpp files no longer
require editing this list (note: CMake configure must re-run)."
```

---

## 验收标准（整层）

完成 Task 1-4 后：

- [ ] `cd cli && xmake clean && xmake` 通过，`xmake -v` 显示的编译文件集合理。
- [ ] `cd cli && xmake run crossscale-cli --help` 正常。
- [ ] `cd cli && xmake run crossscale-cli server start --fg`（timeout 2s）正常打印连接卡。
- [ ] （若有 Android 环境）`cd apps && ./gradlew :app:externalNativeBuildDebug` 通过。
- [ ] 对一张测试图，`crossscale process -i ... -o ...` 输出与重构前 md5 一致。
- [ ] `git log --oneline` 显示 4 个独立提交。
- [ ] 新增一个空 `.cpp` 到 `share/core/`，`xmake` 无需改 xmake.lua 即自动编入（验证 glob 生效）。

---

## 风险

| 风险 | 缓解 |
|---|---|
| ConnectionInfo 有未检测到的反射/间接使用 | Task 2 Step 1-2 的 grep 兜底；编译失败会立即暴露 |
| glob 误纳入平台专属文件导致跨平台编译失败 | Task 3 Step 3 的排除模式 + Task 4 的 list(FILTER) |
| Android 无 NDK 环境无法验证 Task 4 | Task 4 Step 4 注明可跳过，先落 PC 侧 |
| xmake `|` 排除语法不被支持 | Task 3 Step 3 给出 remove_files 备选方案 |
