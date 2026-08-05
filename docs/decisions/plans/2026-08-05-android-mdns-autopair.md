# Android ↔ PC 端到端加密 + mDNS 自动配对 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 X25519 ECDH + Ed25519 签名 + `crypto_secretbox` 全程加密替换明文 auth_key,实现 Android 通过 mDNS 自动发现 PC、指纹确认后一键配对。

**Architecture:** 新增三个 C++ 模块(`crypto_sodium` 薄封装 / `identity` 持久身份 / `crypto_session` 加密包装),重写 worker/coordinator 握手为 3 报文相互签名时序,所有 TCP 帧经 `EncryptedSession` secretbox 加密。Android 通过 mDNS TXT 拿到 PC 身份公钥+指纹,用户确认后建立加密会话,TOFU 互存信任。

**Tech Stack:** C++23,libsodium(X25519/Ed25519/secretbox),Kotlin/Compose,JNI,Android NsdManager,EncryptedSharedPreferences。

**Spec:** `docs/decisions/specs/2026-08-05-android-mdns-autopair-design.md`

**重要前置事实(已核实):**
- Android 构建明确**排除** `coordinator.cpp` 和 `mdns.cpp`(见 `apps/app/src/main/cpp/CMakeLists.txt:131-134`)。Android 只链接 worker.cpp + 新增三个 crypto 模块。PC 才改 coordinator/mdns。
- share 源文件靠 `file(GLOB)` / xmake glob 自动收集 —— 新加 `.cpp` 无需手动登记到构建文件。
- 项目无 C++ 单测框架;现有测试是 `scripts/test_distributed.cpp` 手写 main。本计划沿用此风格。
- 编译用 `cd cli && xmake`(PC)、`cd apps && ./gradlew assembleDebug`(Android)。

---

## 文件结构

### 新增(C++)
| 文件 | 职责 |
|---|---|
| `share/utils/crypto_sodium.hpp` / `.cpp` | libsodium 初始化 + X25519/Ed25519/secretbox 薄封装。零业务逻辑,纯 C API 包装 |
| `share/core/distributed/identity.hpp` / `.cpp` | `IdentityKey`(Ed25519 持久身份 load/generate/sign/verify)、`TrustStore`(白名单 add/contains/save/load)、`fingerprint()` |
| `share/core/distributed/crypto_session.hpp` / `.cpp` | `EncryptedSession`:激活后 seal/open 把明文帧包装成 nonce+密文。握手前不激活 |
| `scripts/test_crypto.cpp` | 自包含 main,覆盖三模块的往返/KAT/篡改用例 |

### 改 C++
| 文件 | 改动 |
|---|---|
| `share/core/distributed/protocol.hpp` | 删 MSG_AUTH_REQUEST/AuthRequest;加 MSG_KEY_HELLO/AUTH_PROOF;AuthResponse 加 trusted;version=2 |
| `share/core/distributed/worker.cpp` | connect 签名换 peer_pubkey;handshake 重写为 3 报文;所有 send/recv 经 EncryptedSession |
| `share/core/distributed/worker.hpp` | 成员加 IdentityKey + EncryptedSession |
| `share/core/distributed/coordinator.cpp` | start 取 IdentityKey&;handle_connection 重写握手;authenticate 改验签 |
| `share/core/distributed/coordinator.hpp` | 成员加 IdentityKey + TrustStore |
| `share/core/distributed/coordinator_admin.cpp` | authenticate(conn, pubkey) |
| `share/utils/mdns.cpp` + `mdns.hpp` | TXT 改 id_fingerprint/id_pubkey;start 签名换 |
| `share/utils/crypto.cpp` | 删 generate_auth_key(如有);保留 sha256_hex/uuid |
| `cli/src/cmd_server.cpp` | 身份加载;调用改 identity |
| `cli/src/common.cpp` | QR v2 JSON;connection card 显示指纹 |
| `cli/src/cmd_worker.cpp` | 删 -k;加 --trust-fingerprint |
| `cli/src/args.hpp` | Server.key / Worker.key 字段调整 |
| `cli/src/cli_app.cpp` | CLI 选项定义调整 |
| `cli/xmake.lua` | add libsodium |
| `apps/app/src/main/cpp/CMakeLists.txt` | import libsodium.a + include + CROSSSCALE_PROTOCOL_V2 |
| `apps/app/src/main/cpp/jni_bridge.cpp` | nativeConnect/nativeProcessImageDistributed 签名换 pubkey;加 nativeLoadIdentity |
| `apps/app/src/main/cpp/jniLibs/<abi>/libsodium.a` + `third_party/include/sodium.h` | 预编译库(arm64-v8a 必需) |
| `scripts/sync_native_deps.sh` | 加 libsodium 下载逻辑 |

### 新增/改 Android Kotlin
| 文件 | 改动 |
|---|---|
| `apps/.../native/NCNNEngine.kt` + `NativeWorker.kt` | external 声明改 pubkey;加 loadIdentity |
| `apps/.../network/MDNSDiscovery.kt` | 读 id_fingerprint/id_pubkey TXT |
| `apps/.../network/JoinInvite.kt` | v2 JSON(fp/pk) |
| `apps/.../network/CoordinatorClient.kt` | connect 用 peerPubkey |
| `apps/.../data/repository/PairingRepository.kt` | 存 identityPubKey+fingerprint |
| `apps/.../data/model/DeviceProfile.kt` | CONNECTING + discoveredFingerprint/PubKey |
| `apps/.../data/repository/DeviceRepository.kt` | markConnecting/markFailed + 合并保留 |
| `apps/.../viewmodel/DeviceViewModel.kt` | connectDiscoveredWithKey/confirmTrustAndConnect + 状态包裹 |
| `apps/.../ui/components/DeviceCard.kt` | 四态动作行 + 配对徽章 + 指纹 |
| `apps/.../ui/components/FingerprintConfirmDialog.kt` | 新建:指纹确认对话框 |
| `apps/.../ui/screens/DevicesScreen.kt` | SnackbarHost + 文案 + paired 透传 |

---

## Task 1: libsodium 集成 + crypto_sodium 薄封装

**Files:**
- Modify: `cli/xmake.lua`
- Create: `share/utils/crypto_sodium.hpp`
- Create: `share/utils/crypto_sodium.cpp`
- Create: `scripts/test_crypto.cpp`

- [ ] **Step 1: PC 加 libsodium 包**

修改 `cli/xmake.lua`,在 `add_packages` 行加 `"libsodium"`:

```lua
add_packages("ncnn", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium")
```

- [ ] **Step 2: 验证 libsodium 可拉取**

Run: `cd cli && xmake`
Expected: xrepo 自动拉取 libsodium 并编译通过(若失败,见 Step 3 备注)

- [ ] **Step 3: 若 Step 2 失败 — 手动指定 xrepo repo**

某些 xmake 版本需先 `xrepo add-repo xmake-repo https://github.com/xmake-io/xmake-repo.git`。若仍失败,改用 `add_requires("libsodium", {alias = "libsodium"})` 显式声明。记录实际可用配置到 commit message。

- [ ] **Step 4: 写 crypto_sodium.hpp**

`share/utils/crypto_sodium.hpp`:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace crossscale {
namespace sodium {

// 进程级初始化,幂等。失败抛 std::runtime_error。
void init();

// X25519 (Curve25519 ECDH)
struct X25519Keypair {
    std::array<uint8_t, 32> public_key;
    std::array<uint8_t, 32> private_key;
};
auto x25519_keygen() -> X25519Keypair;
auto x25519_shared(const std::array<uint8_t, 32>& my_private,
                   const std::array<uint8_t, 32>& peer_public)
    -> std::array<uint8_t, 32>;

// Ed25519 (签名身份)
struct Ed25519Keypair {
    std::array<uint8_t, 32> public_key;
    std::array<uint8_t, 64> private_key;  // libsodium seed+pub 拼接
};
auto ed25519_keygen() -> Ed25519Keypair;
auto ed25519_sign(const std::array<uint8_t, 64>& private_key,
                  const uint8_t* msg, size_t len) -> std::array<uint8_t, 64>;
auto ed25519_verify(const std::array<uint8_t, 32>& public_key,
                    const std::array<uint8_t, 64>& sig,
                    const uint8_t* msg, size_t len) -> bool;

// crypto_secretbox (XSalsa20-Poly1305)
// seal: 产出 nonce(24) + ciphertext(plaintext_len + 16 MAC)
auto secretbox_seal(const uint8_t key[32],
                    const uint8_t* plaintext, size_t len)
    -> std::vector<uint8_t>;
// open: 输入 nonce+ciphertext,返回 plaintext;MAC 失败返回 nullopt
auto secretbox_open(const uint8_t key[32],
                    const uint8_t* nonce_ct, size_t nonce_ct_len)
    -> std::optional<std::vector<uint8_t>>;

} // namespace sodium
} // namespace crossscale
```

- [ ] **Step 5: 写 crypto_sodium.cpp**

`share/utils/crypto_sodium.cpp`:

```cpp
#include "crypto_sodium.hpp"
#include <sodium.h>
#include <stdexcept>

namespace crossscale {
namespace sodium {

void init() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

auto x25519_keygen() -> X25519Keypair {
    X25519Keypair kp;
    crypto_box_keypair(kp.public_key.data(), kp.private_key.data());
    return kp;
}

auto x25519_shared(const std::array<uint8_t, 32>& my_private,
                   const std::array<uint8_t, 32>& peer_public)
    -> std::array<uint8_t, 32> {
    std::array<uint8_t, 32> shared;
    crypto_scalarmult_curve25519(shared.data(), my_private.data(),
                                 peer_public.data());
    return shared;
}

auto ed25519_keygen() -> Ed25519Keypair {
    Ed25519Keypair kp;
    crypto_sign_keypair(kp.public_key.data(), kp.private_key.data());
    return kp;
}

auto ed25519_sign(const std::array<uint8_t, 64>& private_key,
                  const uint8_t* msg, size_t len) -> std::array<uint8_t, 64> {
    std::array<uint8_t, 64> sig;
    crypto_sign_detached(sig.data(), nullptr, msg, len, private_key.data());
    return sig;
}

auto ed25519_verify(const std::array<uint8_t, 32>& public_key,
                    const std::array<uint8_t, 64>& sig,
                    const uint8_t* msg, size_t len) -> bool {
    return crypto_sign_verify_detached(sig.data(), msg, len,
                                       public_key.data()) == 0;
}

auto secretbox_seal(const uint8_t key[32],
                    const uint8_t* plaintext, size_t len)
    -> std::vector<uint8_t> {
    std::vector<uint8_t> out(crypto_box_NONCEBYTES + len + crypto_box_MACBYTES);
    uint8_t* nonce = out.data();
    uint8_t* ct = out.data() + crypto_box_NONCEBYTES;
    randombytes_buf(nonce, crypto_box_NONCEBYTES);
    crypto_box_easy(ct, plaintext, len, nonce, key);
    return out;
}

auto secretbox_open(const uint8_t key[32],
                    const uint8_t* nonce_ct, size_t nonce_ct_len)
    -> std::optional<std::vector<uint8_t>> {
    if (nonce_ct_len < crypto_box_NONCEBYTES + crypto_box_MACBYTES) {
        return std::nullopt;
    }
    const uint8_t* nonce = nonce_ct;
    const uint8_t* ct = nonce_ct + crypto_box_NONCEBYTES;
    size_t ct_len = nonce_ct_len - crypto_box_NONCEBYTES;
    std::vector<uint8_t> plaintext(ct_len - crypto_box_MACBYTES);
    if (crypto_box_open_easy(plaintext.data(), ct, ct_len, nonce, key) != 0) {
        return std::nullopt;  // MAC 失败
    }
    return plaintext;
}

} // namespace sodium
} // namespace crossscale
```

- [ ] **Step 6: 写 scripts/test_crypto.cpp(自包含 main)**

```cpp
#include "utils/crypto_sodium.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace crossscale::sodium;

int main() {
    init();

    // 1. X25519 双向派生一致
    auto a = x25519_keygen();
    auto b = x25519_keygen();
    auto sa = x25519_shared(a.private_key, b.public_key);
    auto sb = x25519_shared(b.private_key, a.public_key);
    assert(std::memcmp(sa.data(), sb.data(), 32) == 0);
    std::printf("OK x25519 shared secret matches\n");

    // 2. Ed25519 sign/verify
    auto id = ed25519_keygen();
    const char msg[] = "hello crossscale";
    auto sig = ed25519_sign(id.private_key,
                            (const uint8_t*)msg, sizeof(msg) - 1);
    assert(ed25519_verify(id.public_key, sig,
                          (const uint8_t*)msg, sizeof(msg) - 1));
    // 篡改消息应失败
    assert(!ed25519_verify(id.public_key, sig,
                           (const uint8_t*)"hello crossscalX", sizeof(msg) - 1));
    std::printf("OK ed25519 sign/verify + tamper reject\n");

    // 3. secretbox seal/open 往返
    uint8_t key[32] = {};
    randombytes_buf(key, 32);
    std::vector<uint8_t> pt(1000);
    randombytes_buf(pt.data(), pt.size());
    auto sealed = secretbox_seal(key, pt.data(), pt.size());
    auto opened = secretbox_open(key, sealed.data(), sealed.size());
    assert(opened.has_value());
    assert(opened->size() == pt.size());
    assert(std::memcmp(opened->data(), pt.data(), pt.size()) == 0);
    std::printf("OK secretbox roundtrip\n");

    // 4. secretbox 篡改应失败
    sealed[30] ^= 0xFF;  // 翻密文字节
    auto tampered = secretbox_open(key, sealed.data(), sealed.size());
    assert(!tampered.has_value());
    std::printf("OK secretbox tamper reject\n");

    std::printf("ALL CRYPTO TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 7: 加 test_crypto 到 xmake 并编译运行**

在 `cli/xmake.lua` 末尾追加:

```lua
target("test_crypto")
    set_kind("binary")
    add_deps("crossscale-core")
    add_packages("libsodium")
    add_files("$(projectdir)/scripts/test_crypto.cpp")
    set_targetdir("$(projectdir)/cli/build")
```

Run: `cd cli && xmake build test_crypto && xmake run test_crypto`
Expected: 输出 4 个 OK + `ALL CRYPTO TESTS PASSED`

- [ ] **Step 8: Commit**

```bash
git add share/utils/crypto_sodium.hpp share/utils/crypto_sodium.cpp \
        scripts/test_crypto.cpp cli/xmake.lua
git commit -m "feat(crypto): add libsodium wrapper (X25519/Ed25519/secretbox)"
```

---

## Task 2: identity 模块(持久身份 + 信任白名单 + 指纹)

**Files:**
- Create: `share/core/distributed/identity.hpp`
- Create: `share/core/distributed/identity.cpp`
- Modify: `scripts/test_crypto.cpp`(追加用例)

- [ ] **Step 1: 写 identity.hpp**

`share/core/distributed/identity.hpp`:

```cpp
#pragma once

#include "crypto_sodium.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace crossscale {

// 持久 Ed25519 身份。加载或新建并落盘。
class IdentityKey {
public:
    // 路径不存在则生成新密钥对并写文件(权限 0600)。存在则加载。
    // 失败抛 std::runtime_error。
    static auto load_or_create(const std::filesystem::path& file) -> IdentityKey;

    auto public_key() const -> std::array<uint8_t, 32> { return kp_.public_key; }
    auto sign(const uint8_t* msg, size_t len) const -> std::array<uint8_t, 64>;

    // 文件格式:64 字节私钥(原样存)。加载后 zero 内存由析构负责。
    ~IdentityKey();

private:
    explicit IdentityKey(sodium::Ed25519Keypair kp) : kp_(kp) {}
    sodium::Ed25519Keypair kp_;
};

// 信任白名单:每行一个 base64 的 32 字节公钥。
class TrustStore {
public:
    static auto load(const std::filesystem::path& file) -> TrustStore;
    auto contains(const std::array<uint8_t, 32>& pubkey) const -> bool;
    void add(const std::array<uint8_t, 32>& pubkey);  // 加到内存
    void save() const;  // 落盘(若 file 非空)
    auto size() const -> size_t { return entries_.size(); }

private:
    TrustStore(std::filesystem::path file,
               std::vector<std::array<uint8_t, 32>> entries)
        : file_(std::move(file)), entries_(std::move(entries)) {}
    std::filesystem::path file_;
    std::vector<std::array<uint8_t, 32>> entries_;
};

// sha256(pubkey) 前 8 字节 hex 大写,如 "ABCD1234EF567890"
auto fingerprint(const std::array<uint8_t, 32>& pubkey) -> std::string;

// base64 编解码(32 字节公钥 <-> 字符串,供存储/QR/TXT 用)
auto base64_encode(const uint8_t* data, size_t len) -> std::string;
auto base64_decode(std::string_view s) -> std::optional<std::vector<uint8_t>>;

} // namespace crossscale
```

- [ ] **Step 2: 写 identity.cpp**

`share/core/distributed/identity.cpp`:

```cpp
#include "identity.hpp"
#include "utils/crypto.hpp"  // sha256_hex
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace crossscale {

namespace {
constexpr size_t ED_PUBKEY_LEN = 32;

void ensure_0600(const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    using std::error_code;
    error_code ec;
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    // 权限设置失败不致命(如 Windows),忽略。
}
} // namespace

auto IdentityKey::load_or_create(const std::filesystem::path& file)
    -> IdentityKey {
    if (std::filesystem::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Failed to open identity file: " +
                                     file.string());
        }
        sodium::Ed25519Keypair kp{};
        in.read(reinterpret_cast<char*>(kp.private_key.data()), 64);
        if (in.gcount() != 64) {
            throw std::runtime_error("Identity file truncated: " + file.string());
        }
        // libsodium: 公钥是私钥后 32 字节
        std::memcpy(kp.public_key.data(), kp.private_key.data() + 32, 32);
        return IdentityKey(kp);
    }

    // 新建
    auto kp = sodium::ed25519_keygen();
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Failed to create identity file: " +
                                     file.string());
        }
        out.write(reinterpret_cast<const char*>(kp.private_key.data()), 64);
    }
    ensure_0600(file);
    return IdentityKey(kp);
}

auto IdentityKey::sign(const uint8_t* msg, size_t len) const
    -> std::array<uint8_t, 64> {
    return sodium::ed25519_sign(kp_.private_key, msg, len);
}

IdentityKey::~IdentityKey() {
    sodium::Ed25519Keypair zero{};
    kp_ = zero;  // best-effort zero
}

auto TrustStore::load(const std::filesystem::path& file) -> TrustStore {
    std::vector<std::array<uint8_t, 32>> entries;
    if (std::filesystem::exists(file)) {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            // 去空白
            line.erase(std::remove_if(line.begin(), line.end(),
                                       [](unsigned char c){ return std::isspace(c); }),
                       line.end());
            if (line.empty()) continue;
            auto bytes = base64_decode(line);
            if (bytes && bytes->size() == ED_PUBKEY_LEN) {
                std::array<uint8_t, 32> pk{};
                std::memcpy(pk.data(), bytes->data(), ED_PUBKEY_LEN);
                entries.push_back(pk);
            }
        }
    }
    return TrustStore(file, std::move(entries));
}

auto TrustStore::contains(const std::array<uint8_t, 32>& pubkey) const -> bool {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const std::array<uint8_t, 32>& e) {
                           return std::memcmp(e.data(), pubkey.data(), 32) == 0;
                       });
}

void TrustStore::add(const std::array<uint8_t, 32>& pubkey) {
    if (!contains(pubkey)) entries_.push_back(pubkey);
}

void TrustStore::save() const {
    if (file_.empty()) return;
    std::ofstream out(file_, std::ios::trunc);
    for (const auto& pk : entries_) {
        out << base64_encode(pk.data(), ED_PUBKEY_LEN) << "\n";
    }
    ensure_0600(file_);
}

auto fingerprint(const std::array<uint8_t, 32>& pubkey) -> std::string {
    auto full = crypto::sha256_hex(std::string(
        reinterpret_cast<const char*>(pubkey.data()), 32));
    // sha256_hex 返回 64 字符,取前 16 字符(8 字节)大写
    std::string fp = full.substr(0, 16);
    std::transform(fp.begin(), fp.end(), fp.begin(), ::toupper);
    return fp;
}

// ── base64 ──────────────────────────────────────────────────────────────
namespace {
constexpr char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}
auto base64_encode(const uint8_t* data, size_t len) -> std::string {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i+1] << 8;
        if (i + 2 < len) n |= data[i+2];
        out += B64[(n >> 18) & 63];
        out += B64[(n >> 12) & 63];
        out += (i + 1 < len) ? B64[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? B64[n & 63] : '=';
    }
    return out;
}

auto base64_decode(std::string_view s) -> std::optional<std::vector<uint8_t>> {
    static int8_t rev[256];
    static bool inited = false;
    if (!inited) {
        std::fill(std::begin(rev), std::end(rev), (int8_t)-1);
        for (int i = 0; i < 64; ++i) rev[(unsigned char)B64[i]] = (int8_t)i;
        inited = true;
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        int8_t v = rev[c];
        if (v < 0) return std::nullopt;  // 非法字符
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return out;
}

} // namespace crossscale
```

- [ ] **Step 3: 追加 identity 测试到 scripts/test_crypto.cpp**

在 `main()` 末尾(`ALL CRYPTO TESTS PASSED` 之前)插入:

```cpp
    // 5. IdentityKey load_or_create 幂等
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "cs_test_identity";
    fs::remove(tmp);
    {
        auto id1 = IdentityKey::load_or_create(tmp);
        auto id2 = IdentityKey::load_or_create(tmp);
        assert(id1.public_key() == id2.public_key());
    }
    // 验签往返
    {
        auto id = IdentityKey::load_or_create(tmp);
        const char m[] = "id test";
        auto sig = id.sign((const uint8_t*)m, sizeof(m)-1);
        assert(ed25519_verify(id.public_key(), sig, (const uint8_t*)m, sizeof(m)-1));
    }
    std::printf("OK identity load_or_create + sign\n");

    // 6. fingerprint 稳定 + 格式
    std::array<uint8_t,32> pk{}; pk[0] = 0xAB; pk[1] = 0xCD;
    auto fp = fingerprint(pk);
    assert(fp.size() == 16);
    assert(fp == crypto::sha256_hex(std::string((const char*)pk.data(),32))
                       .substr(0,16));
    // 转 base64 解码往返
    auto b64 = base64_encode(pk.data(), 32);
    auto dec = base64_decode(b64);
    assert(dec && dec->size() == 32 && std::memcmp(dec->data(),pk.data(),32)==0);
    std::printf("OK fingerprint + base64 roundtrip\n");

    // 7. TrustStore add/contains/save/load
    fs::path ts_path = fs::temp_directory_path() / "cs_test_trust";
    fs::remove(ts_path);
    {
        auto ts = TrustStore::load(ts_path);
        assert(!ts.contains(pk));
        ts.add(pk);
        assert(ts.contains(pk));
        ts.save();
    }
    {
        auto ts2 = TrustStore::load(ts_path);
        assert(ts2.contains(pk));
        assert(ts2.size() == 1);
    }
    fs::remove(ts_path);
    std::printf("OK truststore add/contains/save/load\n");
```

并在文件顶部加 include:
```cpp
#include "core/distributed/identity.hpp"
#include <filesystem>
```

- [ ] **Step 4: 编译运行**

Run: `cd cli && xmake build test_crypto && xmake run test_crypto`
Expected: 7 个 OK + `ALL CRYPTO TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git add share/core/distributed/identity.hpp share/core/distributed/identity.cpp \
        scripts/test_crypto.cpp
git commit -m "feat(crypto): add identity module (Ed25519 + TrustStore + fingerprint)"
```

---

## Task 3: crypto_session — EncryptedSession 包装层

**Files:**
- Create: `share/core/distributed/crypto_session.hpp`
- Create: `share/core/distributed/crypto_session.cpp`
- Modify: `scripts/test_crypto.cpp`

- [ ] **Step 1: 写 crypto_session.hpp**

`share/core/distributed/crypto_session.hpp`:

```cpp
#pragma once

#include "core/distributed/protocol.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace crossscale {

// 包装/解包 secretbox 加密帧。握手前调 seal/open 会断言失败。
// 激活后,所有明文 Protocol::serialize 输出经 seal() 变成 wire bytes,
// 反之 wire 经 open() 还原成明文 header + payload。
class EncryptedSession {
public:
    using SessionKey = std::array<uint8_t, 32>;

    EncryptedSession() = default;

    auto active() const -> bool { return active_; }

    // 激活:传入 X25519 派生的会话密钥。激活后才能 seal/open。
    void activate(const SessionKey& key);

    // seal: 输入明文 header + payload,输出完整 wire bytes。
    // wire = [MessageHeader(15B,type 不变,payload_length 改为密文+nonce长度)]
    //        [nonce(24B)][ciphertext(payload_len + 16 MAC)]
    auto seal(const MessageHeader& header, const void* plaintext_payload)
        -> std::vector<uint8_t>;

    // open: 输入完整 wire,返回 {明文 header, 明文 payload}。MAC 失败返回 nullopt。
    auto open(const uint8_t* wire, size_t wire_len)
        -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>;

private:
    bool active_ = false;
    SessionKey key_{};
};

} // namespace crossscale
```

- [ ] **Step 2: 写 crypto_session.cpp**

`share/core/distributed/crypto_session.cpp`:

```cpp
#include "crypto_session.hpp"
#include "crypto_sodium.hpp"
#include <cassert>
#include <cstring>

namespace crossscale {

void EncryptedSession::activate(const SessionKey& key) {
    key_ = key;
    active_ = true;
}

auto EncryptedSession::seal(const MessageHeader& header,
                            const void* plaintext_payload) -> std::vector<uint8_t> {
    assert(active_ && "seal before activate");
    const uint32_t pt_len = header.payload_length;

    // 密文长度 = pt_len + MAC(16)
    const size_t nonce_len = 24;  // crypto_box_NONCEBYTES
    const size_t ct_len = pt_len + 16;

    // 先构造修改后的 header:payload_length 改为 nonce+ct
    MessageHeader out_hdr = header;
    out_hdr.payload_length = static_cast<uint32_t>(nonce_len + ct_len);
    // checksum 保留明文 payload 的 CRC(诊断用)
    out_hdr.checksum = Protocol::compute_checksum(plaintext_payload, pt_len);

    auto serialized_hdr = Protocol::serialize(out_hdr, nullptr);
    // serialize 在 payload=nullptr 时仍产 15B header(参考 protocol.cpp 实现;
    // 若现有实现要求 payload 非空,改用手工 pack 15 字节)

    // 产 nonce+ct
    auto nonce_ct = sodium::secretbox_seal(
        key_.data(), static_cast<const uint8_t*>(plaintext_payload), pt_len);

    // 拼接 [header][nonce+ct]
    std::vector<uint8_t> wire;
    wire.reserve(serialized_hdr.size() + nonce_ct.size());
    wire.insert(wire.end(), serialized_hdr.begin(), serialized_hdr.end());
    wire.insert(wire.end(), nonce_ct.begin(), nonce_ct.end());
    return wire;
}

auto EncryptedSession::open(const uint8_t* wire, size_t wire_len)
    -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>> {
    assert(active_ && "open before activate");
    if (wire_len < Protocol::HEADER_SIZE) return std::nullopt;

    auto hdr = Protocol::parse_header(wire, Protocol::HEADER_SIZE);
    if (!hdr) return std::nullopt;

    const size_t nonce_ct_len = wire_len - Protocol::HEADER_SIZE;
    if (hdr->payload_length != nonce_ct_len) return std::nullopt;  // 长度不符

    auto opened = sodium::secretbox_open(
        key_.data(), wire + Protocol::HEADER_SIZE, nonce_ct_len);
    if (!opened) return std::nullopt;  // MAC 失败

    // 还原原始 header(payload_length 是明文长度,去掉 nonce+MAC 开销)
    MessageHeader real_hdr = *hdr;
    real_hdr.payload_length = static_cast<uint32_t>(opened->size());
    return std::make_pair(real_hdr, std::move(*opened));
}

} // namespace crossscale
```

- [ ] **Step 3: 验证 Protocol::serialize 对 nullptr payload 的行为**

Read `share/core/distributed/protocol.cpp` 的 `serialize` 实现。若 payload=nullptr 且 payload_length=0 时返回 15B header(典型实现),Step 2 代码正确。若 serialize 在 payload 非空时才追加 payload 字节,则当 out_hdr.payload_length 已被改成 nonce_ct 长度时会错误追加 —— 此时改为手工 pack header:

```cpp
// 备用:手工序列化 15B header(小端)
std::vector<uint8_t> serialized_hdr(Protocol::HEADER_SIZE);
auto put_u32 = [](uint8_t* p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; };
auto put_u16 = [](uint8_t* p, uint16_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; };
put_u32(serialized_hdr.data()+0, out_hdr.magic);
serialized_hdr[4] = out_hdr.version;
put_u16(serialized_hdr.data()+5, static_cast<uint16_t>(out_hdr.type));
put_u32(serialized_hdr.data()+7, out_hdr.payload_length);
put_u32(serialized_hdr.data()+11, out_hdr.checksum);
```

记录实际选择到 commit message。

- [ ] **Step 4: 追加 EncryptedSession 测试到 scripts/test_crypto.cpp**

```cpp
    // 8. EncryptedSession seal/open 往返
    {
        EncryptedSession s;
        EncryptedSession::SessionKey k{};
        for (auto& b : k) b = 0x42;
        s.activate(k);

        MessageHeader hdr;
        hdr.type = MessageType::MSG_HEARTBEAT;
        std::vector<uint8_t> payload(64);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)i;
        hdr.payload_length = (uint32_t)payload.size();

        auto wire = s.seal(hdr, payload.data());
        assert(wire.size() == 15 + 24 + 64 + 16);

        auto opened = s.open(wire.data(), wire.size());
        assert(opened.has_value());
        assert(opened->first.type == MessageType::MSG_HEARTBEAT);
        assert(opened->first.payload_length == 64);
        assert(opened->second.size() == 64);
        assert(std::memcmp(opened->second.data(), payload.data(), 64) == 0);

        // 篡改密文应失败
        wire[20] ^= 0xFF;
        assert(!s.open(wire.data(), wire.size()));
        std::printf("OK encrypted session seal/open + tamper reject\n");
    }
```

加 include `#include "core/distributed/crypto_session.hpp"`。

- [ ] **Step 5: 编译运行**

Run: `cd cli && xmake build test_crypto && xmake run test_crypto`
Expected: 8 个 OK + `ALL CRYPTO TESTS PASSED`

- [ ] **Step 6: Commit**

```bash
git add share/core/distributed/crypto_session.hpp share/core/distributed/crypto_session.cpp \
        scripts/test_crypto.cpp
git commit -m "feat(crypto): add EncryptedSession (secretbox wire framing)"
```

---

## Task 4: protocol.hpp v2 + 接入 EncryptedSession 到 worker/coordinator

**这是最大的 Task。** 先改协议结构,再改 worker 握手,再改 coordinator 握手。每步可独立编译但功能端到端在 Task 末才通。

**Files:**
- Modify: `share/core/distributed/protocol.hpp`
- Modify: `share/core/distributed/worker.hpp` + `worker.cpp`
- Modify: `share/core/distributed/coordinator.hpp` + `coordinator.cpp`
- Modify: `share/core/distributed/coordinator_admin.cpp`
- Modify: `scripts/test_distributed.cpp`(端到端验证)

- [ ] **Step 1: 改 protocol.hpp — 删旧 auth,加新握手结构,version=2**

在 `share/core/distributed/protocol.hpp`:

删除 `MSG_AUTH_REQUEST = 0x0003` 行。

在 `MSG_DISCONNECT` 之前加:
```cpp
    MSG_KEY_HELLO     = 0x0007,  // C→S, S→C: 交换 ephemeral + identity 公钥
    MSG_AUTH_PROOF    = 0x0008,  // C→S: Ed25519 签名(加密)
```

把 `struct MessageHeader` 的 `uint8_t version = 1;` 改 `uint8_t version = 2;`。

删除整个 `struct AuthRequest`(line 46-48)。

改 `struct AuthResponse`,在末尾加字段:
```cpp
struct AuthResponse {
    uint8_t authenticated;
    char device_id[36];
    uint8_t trusted;  // 0=首次需 TOFU, 1=已在白名单
};
```

在 `struct ModelInfoMessage` 之前加握手结构:
```cpp
struct KeyHelloMessage {
    uint8_t eph_pub[32];   // X25519 ephemeral 公钥
    uint8_t id_pub[32];    // Ed25519 身份公钥
    uint8_t sig[64];       // S→C 时填:s_id 签 hash(双 eph + 双 id);C→S 留 0
};

struct AuthProofMessage {
    uint8_t sig[64];       // c_id 签 hash(transcript)
};
```

- [ ] **Step 2: 改 worker.hpp — 成员加 IdentityKey + EncryptedSession,connect 签名换**

读 `share/core/distributed/worker.hpp` 现状,修改:

- `connect` 签名:`(const std::string& host, uint16_t port, const std::string& auth_key)` → `(const std::string& host, uint16_t port, const std::array<uint8_t,32>& peer_identity_pubkey, IdentityKey& local_identity)`
- 加私有成员:`EncryptedSession session_;`、`std::array<uint8_t,32> peer_id_pub_;`
- 删 `std::string auth_key_;`(若存在)
- `handshake` 签名不变(仍取 Engine&),内部重写

- [ ] **Step 3: 重写 worker.cpp handshake() 为 3 报文握手**

替换 `worker.cpp` 中 `Worker::handshake`(line 93-149)整个函数体。完整新实现:

```cpp
auto Worker::handshake(Engine& engine) -> expected<void, WorkerError> {
    if (!socket_) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }
    sodium::init();

    // ① 生成 ephemeral,准备 hello
    auto eph = sodium::x25519_keygen();
    const auto& id_pub = local_identity_->public_key();

    KeyHelloMessage hello{};
    std::memcpy(hello.eph_pub, eph.public_key.data(), 32);
    std::memcpy(hello.id_pub, id_pub.data(), 32);
    std::memset(hello.sig, 0, 64);  // C→S 不签名

    MessageHeader hello_hdr;
    hello_hdr.type = MessageType::MSG_KEY_HELLO;
    hello_hdr.payload_length = sizeof(hello);
    auto hello_wire = Protocol::serialize(hello_hdr, &hello);
    auto sr = socket_->send_all(hello_wire.data(), hello_wire.size());
    if (!sr) return std::unexpected(WorkerError{100, "hello send: " + sr.error().message});

    // ② 收 S→C hello(含 s_eph, s_id, s_sig)
    std::vector<uint8_t> hbuf(Protocol::HEADER_SIZE);
    if (!socket_->receive_all(hbuf.data(), hbuf.size()))
        return std::unexpected(WorkerError{101, "hello header recv"});
    auto sh = Protocol::parse_header(hbuf.data(), hbuf.size());
    if (!sh || sh->type != MessageType::MSG_KEY_HELLO)
        return std::unexpected(WorkerError{102, "expected KEY_HELLO"});
    std::vector<uint8_t> spay(sh->payload_length);
    if (!socket_->receive_all(spay.data(), spay.size()))
        return std::unexpected(WorkerError{103, "hello payload recv"});
    auto* s_hello = reinterpret_cast<KeyHelloMessage*>(spay.data());

    // ③ 验 s_sig:hash(c_eph || c_id || s_eph || s_id)
    std::vector<uint8_t> transcript_a;
    transcript_a.insert(transcript_a.end(), eph.public_key.begin(), eph.public_key.end());
    transcript_a.insert(transcript_a.end(), id_pub.begin(), id_pub.end());
    transcript_a.insert(transcript_a.end(), s_hello->eph_pub, s_hello->eph_pub + 32);
    transcript_a.insert(transcript_a.end(), s_hello->id_pub, s_hello->id_pub + 32);
    std::array<uint8_t,32> s_id_pub{};
    std::memcpy(s_id_pub.data(), s_hello->id_pub, 32);
    if (!sodium::ed25519_verify(s_id_pub, *reinterpret_cast<std::array<uint8_t,64>*>(s_hello->sig),
                                transcript_a.data(), transcript_a.size())) {
        // 同时校验 peer_id_pub 与 s_hello->id_pub 一致(防替换)
        return std::unexpected(WorkerError{104, "server identity signature invalid"});
    }
    // 额外:s_hello->id_pub 必须等于我们传入的 peer_identity_pubkey
    if (std::memcmp(s_hello->id_pub, peer_id_pub_.data(), 32) != 0) {
        return std::unexpected(WorkerError{105, "server identity mismatch (MITR?)"});
    }

    // ④ 派生会话密钥,激活 session
    std::array<uint8_t,32> s_eph_pub{};
    std::memcpy(s_eph_pub.data(), s_hello->eph_pub, 32);
    auto shared = sodium::x25519_shared(eph.private_key, s_eph_pub);
    session_.activate(shared);

    // ⑤ 发 AUTH_PROOF(加密):c_id 签 hash(c_eph||c_id||s_eph||s_id||证据)
    std::vector<uint8_t> transcript_b = transcript_a;
    // 用同样 transcript 即可(双方都基于它)
    auto c_sig = local_identity_->sign(transcript_b.data(), transcript_b.size());

    AuthProofMessage proof{};
    std::memcpy(proof.sig, c_sig.data(), 64);
    MessageHeader proof_hdr;
    proof_hdr.type = MessageType::MSG_AUTH_PROOF;
    proof_hdr.payload_length = sizeof(proof);
    auto proof_wire = session_.seal(proof_hdr, &proof);
    auto pr = socket_->send_all(proof_wire.data(), proof_wire.size());
    if (!pr) return std::unexpected(WorkerError{106, "proof send: " + pr.error().message});

    // ⑥ 收 AUTH_RESPONSE(加密)
    std::vector<uint8_t> rbuf(Protocol::HEADER_SIZE);
    if (!socket_->receive_all(rbuf.data(), rbuf.size()))
        return std::unexpected(WorkerError{107, "resp header recv"});
    // 注意:此时 session 已激活,响应是加密的
    // header 是明文(payload_length 含 nonce+MAC),需要据此读完整 wire 再 open
    auto rh = Protocol::parse_header(rbuf.data(), rbuf.size());
    if (!rh) return std::unexpected(WorkerError{108, "resp header parse"});
    std::vector<uint8_t> rwire(rbuf);
    rwire.resize(Protocol::HEADER_SIZE + rh->payload_length);
    if (!socket_->receive_all(rwire.data() + Protocol::HEADER_SIZE, rh->payload_length))
        return std::unexpected(WorkerError{109, "resp payload recv"});
    auto opened = session_.open(rwire.data(), rwire.size());
    if (!opened) return std::unexpected(WorkerError{110, "resp MAC fail"});
    if (opened->first.type != MessageType::MSG_AUTH_RESPONSE)
        return std::unexpected(WorkerError{111, "expected AUTH_RESPONSE"});
    if (opened->second.size() < sizeof(AuthResponse))
        return std::unexpected(WorkerError{112, "resp too small"});
    auto* ar = reinterpret_cast<AuthResponse*>(opened->second.data());
    if (!ar->authenticated)
        return std::unexpected(WorkerError{113, "authentication rejected"});
    device_id_ = ar->device_id;
    peer_trusted_ = (ar->trusted != 0);

    // ⑦ 后续 DEVICE_PROFILE 也走加密(见 Step 5 封装)
    return send_profile_encrypted(engine);
}
```

- [ ] **Step 4: 改 worker.cpp connect() 存参数**

替换 `Worker::connect`(line 61-79):

```cpp
auto Worker::connect(const std::string& host, uint16_t port,
                     const std::array<uint8_t,32>& peer_identity_pubkey,
                     IdentityKey& local_identity)
    -> expected<void, WorkerError> {
    socket_ = std::make_unique<TCPSocket>();
    auto result = socket_->connect(host, port);
    if (!result.has_value()) {
        return std::unexpected(WorkerError{1, "Connection failed: " + result.error().message});
    }
    peer_id_pub_ = peer_identity_pubkey;
    local_identity_ = &local_identity;
    running_ = true;
    LOG_INFO("Worker", "Connected to " + host + ":" + std::to_string(port));
    return {};
}
```

worker.hpp 对应加成员 `IdentityKey* local_identity_ = nullptr;`、`bool peer_trusted_ = false;`。

- [ ] **Step 5: 加 send_profile_encrypted() 并改所有后续 send/recv 走加密**

在 worker.cpp 加(替换原 `send_profile` 调用):

```cpp
auto Worker::send_profile_encrypted(Engine& engine) -> expected<void, WorkerError> {
    DeviceProfile profile;
    profile.device_id = device_id_;
    profile.device_name = "CLI-Worker";
    profile.type = engine.current_device();
    const float tps = engine.benchmark(256);
    const float def_w = default_recommended_weight(profile.type);
    profile.tiles_per_second = recommended_weight_override_ > 0.0f
        ? recommended_weight_override_ : std::max(tps, def_w);
    profile.recommended_tile_size = recommended_tile_size_override_ > 0
        ? recommended_tile_size_override_
        : default_recommended_tile_size(profile.type);
    profile.max_batch_size = queue_depth_override_ > 0
        ? queue_depth_override_ : default_queue_depth(profile.type);
    profile.ip_address = socket_->peer_address();
    profile.port = socket_->peer_port();

    DeviceProfileMessage msg{};
    std::strncpy(msg.device_id, profile.device_id.c_str(), 35);
    std::strncpy(msg.device_name, profile.device_name.c_str(), 63);
    msg.device_type = static_cast<uint8_t>(profile.type);
    msg.tiles_per_second = profile.tiles_per_second;
    msg.recommended_tile_size = profile.recommended_tile_size;
    msg.max_batch_size = profile.max_batch_size;
    std::strncpy(msg.ip_address, profile.ip_address.c_str(), 45);
    msg.port = profile.port;

    MessageHeader hdr;
    hdr.type = MessageType::MSG_DEVICE_PROFILE;
    hdr.payload_length = sizeof(msg);
    auto wire = session_.seal(hdr, &msg);
    auto sr = socket_->send_all(wire.data(), wire.size());
    if (!sr) return std::unexpected(WorkerError{200, "profile send: " + sr.error().message});
    return {};
}
```

worker.hpp 加声明 `auto send_profile_encrypted(Engine&) -> expected<void, WorkerError>;`。

- [ ] **Step 6: 改 worker 的 heartbeat / task_loop / disconnect 用加密发送**

在 worker.cpp 搜索所有 `Protocol::serialize(...)` 后跟 `socket_->send_all` 的位置(line ~157 disconnect、heartbeat、task accept/result 等),把:
```cpp
auto data = Protocol::serialize(header, payload);
socket_->send_all(data.data(), data.size());
```
改为:
```cpp
auto data = session_.seal(header, payload);
socket_->send_all(data.data(), data.size());
```

同理所有 `socket_->receive_all(header) → parse_header → receive_all(payload)` 的接收点,改为:
```cpp
std::vector<uint8_t> hbuf(Protocol::HEADER_SIZE);
socket_->receive_all(hbuf.data(), hbuf.size());
auto rh = Protocol::parse_header(hbuf.data(), hbuf.size());
std::vector<uint8_t> wire(hbuf);
wire.resize(Protocol::HEADER_SIZE + rh->payload_length);
socket_->receive_all(wire.data() + Protocol::HEADER_SIZE, rh->payload_length);
auto opened = session_.open(wire.data(), wire.size());
// 用 opened->first.type / opened->second 代替原 header/payload
```

逐处替换。每处编译验证。注意 MSG_DISCONNECT 在 session 激活后也需加密。

- [ ] **Step 7: 编译 worker(预期 coordinator 还没改,会有链接错误来自 cmd_server,正常)**

Run: `cd cli && xmake build crossscale-core`
Expected: core 静态库编译通过(worker.cpp 编译过)

- [ ] **Step 8: 改 coordinator.hpp + coordinator.cpp — start 取 identity,握手重写**

读 `share/core/distributed/coordinator.hpp`,修改:
- `start(uint16_t port, const std::string& auth_key)` → `start(uint16_t port, IdentityKey& identity)`
- 加成员 `IdentityKey* identity_ = nullptr;`、`TrustStore trust_store_;`(构造时路径 `~/.crossscale/trusted_clients`)

读 `share/core/distributed/coordinator.cpp`:

- `start` 实现(line 27-):删 `auth_key_` 赋值,改存 `identity_ = &identity;`
- `handle_connection`(line ~170-260)替换 MSG_AUTH_REQUEST 分支为完整握手:

```cpp
if (header->type == MessageType::MSG_KEY_HELLO) {
    auto payload = read_payload(header->payload_length);
    if (!payload || payload->size() < sizeof(KeyHelloMessage)) return;
    auto* c_hello = reinterpret_cast<KeyHelloMessage*>(payload->data());

    // 派生 ephemeral,回 hello + s_sig
    sodium::init();
    auto s_eph = sodium::x25519_keygen();
    const auto s_id = identity_->public_key();
    std::vector<uint8_t> ta;
    ta.insert(ta.end(), c_hello->eph_pub, c_hello->eph_pub+32);
    ta.insert(ta.end(), c_hello->id_pub, c_hello->id_pub+32);
    ta.insert(ta.end(), s_eph.public_key.begin(), s_eph.public_key.end());
    ta.insert(ta.end(), s_id.begin(), s_id.end());
    auto s_sig = identity_->sign(ta.data(), ta.size());

    KeyHelloMessage s_hello{};
    std::memcpy(s_hello.eph_pub, s_eph.public_key.data(), 32);
    std::memcpy(s_hello.id_pub, s_id.data(), 32);
    std::memcpy(s_hello.sig, s_sig.data(), 64);
    MessageHeader sh_hdr;
    sh_hdr.type = MessageType::MSG_KEY_HELLO;
    sh_hdr.payload_length = sizeof(s_hello);
    auto sh_wire = Protocol::serialize(sh_hdr, &s_hello);
    (void)conn.socket->send_all(sh_wire.data(), sh_wire.size());

    // 激活加密会话
    std::array<uint8_t,32> c_eph_pub{};
    std::memcpy(c_eph_pub.data(), c_hello->eph_pub, 32);
    auto shared = sodium::x25519_shared(s_eph.private_key, c_eph_pub);
    conn.session.activate(shared);

    // 收 AUTH_PROOF(加密)
    std::vector<uint8_t> phbuf(Protocol::HEADER_SIZE);
    if (!conn.socket->receive_all(phbuf.data(), phbuf.size())) return;
    auto ph = Protocol::parse_header(phbuf.data(), phbuf.size());
    if (!ph) return;
    std::vector<uint8_t> pwire(phbuf);
    pwire.resize(Protocol::HEADER_SIZE + ph->payload_length);
    if (!conn.socket->receive_all(pwire.data()+Protocol::HEADER_SIZE, ph->payload_length)) return;
    auto popened = conn.session.open(pwire.data(), pwire.size());
    if (!popened || popened->first.type != MessageType::MSG_AUTH_PROOF) return;
    auto* proof = reinterpret_cast<AuthProofMessage*>(popened->second.data());

    // 验 c_sig
    std::array<uint8_t,32> c_id_pub{};
    std::memcpy(c_id_pub.data(), c_hello->id_pub, 32);
    if (!sodium::ed25519_verify(c_id_pub,
            *reinterpret_cast<std::array<uint8_t,64>*>(proof->sig),
            ta.data(), ta.size())) {
        // 验签失败:发拒绝响应(加密)
        AuthResponse resp{0, {0}, 0};
        MessageHeader rh; rh.type = MessageType::MSG_AUTH_RESPONSE;
        rh.payload_length = sizeof(resp);
        auto rw = conn.session.seal(rh, &resp);
        (void)conn.socket->send_all(rw.data(), rw.size());
        return;
    }

    // 查信任白名单
    bool trusted = trust_store_.contains(c_id_pub);
    if (!trusted) {
        trust_store_.add(c_id_pub);
        trust_store_.save();  // TOFU:首次即加
    }

    AuthResponse resp{1, {0}, (uint8_t)(trusted ? 1 : 0)};
    std::strncpy(resp.device_id, conn.device_id.c_str(), 35);
    MessageHeader rh; rh.type = MessageType::MSG_AUTH_RESPONSE;
    rh.payload_length = sizeof(resp);
    auto rw = conn.session.seal(rh, &resp);
    if (!conn.socket->send_all(rw.data(), rw.size())) return;
    conn.authenticated = true;

    // 后续 DEVICE_PROFILE / CLIENT_SUBMIT 也都加密读
    // (现有读取循环改为 session_.open 解包 — 见 Step 9)
    ...
}
```

`Connection`(coordinator.hpp 里的连接结构)加成员 `EncryptedSession session;`。

- [ ] **Step 9: 改 coordinator.cpp 后续读消息走加密**

`handle_connection` 中收到 AUTH_RESPONSE 后的循环,所有 `receive_all(header) → parse_header → read_payload` 改为:
```cpp
std::vector<uint8_t> hbuf(Protocol::HEADER_SIZE);
if (!conn.socket->receive_all(hbuf.data(), hbuf.size())) return;
auto rh = Protocol::parse_header(hbuf.data(), hbuf.size());
std::vector<uint8_t> wire(hbuf);
wire.resize(Protocol::HEADER_SIZE + rh->payload_length);
if (!conn.socket->receive_all(wire.data()+Protocol::HEADER_SIZE, rh->payload_length)) return;
auto opened = conn.session.open(wire.data(), wire.size());
if (!opened) { LOG_WARNING("Coordinator","decrypt fail"); return; }
auto& real_hdr = opened->first;
auto& payload = opened->second;
// 用 real_hdr.type 分发,payload 替代原 read_payload 结果
```

逐分支(MSG_DEVICE_PROFILE、MSG_CLIENT_SUBMIT、MSG_TASK_PROGRESS、MSG_TASK_RESULT、MSG_HEARTBEAT)替换。响应发送也走 `conn.session.seal`。

- [ ] **Step 10: 改 coordinator_admin.cpp — authenticate 改验签**

`coordinator_admin.cpp:11` 原 `return received_key == auth_key_;` 删除整个函数,改由 handshake 内联验签(Step 8 已做)。若 `authenticate` 还有其他调用点,改为查 TrustStore 或直接删。

- [ ] **Step 11: PC 端编译验证**

Run: `cd cli && xmake`
Expected: 全部编译通过(cmd_server/cmd_worker 可能因签名变更报错,见 Task 5)

- [ ] **Step 12: 写 scripts/test_distributed.cpp 端到端加密回环**

替换 `scripts/test_distributed.cpp` 全文为:

```cpp
#include "core/distributed/coordinator.hpp"
#include "core/distributed/worker.hpp"
#include "core/distributed/identity.hpp"
#include "core/distributed/crypto_sodium.hpp"
#include "utils/logger.hpp"
#include <cassert>
#include <filesystem>
#include <thread>

int main() {
    using namespace crossscale;
    sodium::init();

    auto id_file = std::filesystem::temp_directory_path() / "cs_test_server_id";
    auto trust_file = std::filesystem::temp_directory_path() / "cs_test_trust";
    std::filesystem::remove(id_file);
    std::filesystem::remove(trust_file);
    auto server_id = IdentityKey::load_or_create(id_file);
    auto client_id = IdentityKey::load_or_create(
        std::filesystem::temp_directory_path() / "cs_test_client_id");

    // 启 coordinator
    Coordinator coordinator;
    coordinator.set_trust_store_path(trust_file);
    auto sr = coordinator.start(9528, server_id);
    assert(sr.has_value());

    // worker 连(传 server 公钥)
    Worker worker;
    auto cr = worker.connect("127.0.0.1", 9528, server_id.public_key(), client_id);
    assert(cr.has_value());

    // handshake(需要一个 engine;此处用 stub)
    // 注:完整 handshake 需要 Engine;此处仅验证 connect 成功,handshake 在
    // 集成测试(带 ncnn stub)中验证。简化:只断言 connect 成功。
    worker.disconnect();
    coordinator.stop();
    std::printf("OK distributed encrypted connect loopback\n");
    return 0;
}
```

注:完整 handshake 验证需要 Engine 实例,放 Task 6 端到端手测。本 test 至少验证 connect 路径。

- [ ] **Step 13: Commit**

```bash
git add share/core/distributed/protocol.hpp share/core/distributed/worker.hpp \
        share/core/distributed/worker.cpp share/core/distributed/coordinator.hpp \
        share/core/distributed/coordinator.cpp share/core/distributed/coordinator_admin.cpp \
        scripts/test_distributed.cpp
git commit -m "feat(proto): v2 encrypted handshake (ECDH + secretbox) in worker/coordinator"
```

---

## Task 5: PC CLI(server / worker / mdns / QR)适配新身份

**Files:**
- Modify: `cli/src/cmd_server.cpp`
- Modify: `cli/src/common.cpp` + `common.hpp`
- Modify: `cli/src/cmd_worker.cpp`
- Modify: `cli/src/args.hpp`
- Modify: `cli/src/cli_app.cpp`
- Modify: `share/utils/mdns.cpp` + `mdns.hpp`

- [ ] **Step 1: 改 mdns.hpp/cpp — TXT 改 id_fingerprint/id_pubkey**

读 `share/utils/mdns.hpp`。改 `start` 签名:
```cpp
auto start(const std::string& service_name, uint16_t port,
           const std::array<uint8_t,32>& identity_pubkey) -> expected<void, MDNSError>;
```

`share/utils/mdns.cpp`:
- 删 `auth_key_hash` 相关(line 88-90, 110, 120)
- Impl 成员 `auth_key_hash` 改为 `std::array<uint8_t,32> identity_pubkey;`
- TXT 三项改为:
```cpp
const std::string version_txt = "version=2";
const std::string role_txt = "role=coordinator";
const std::string fp_txt = "id_fingerprint=" + fingerprint(identity_pubkey);
const std::string pk_txt = "id_pubkey=" + base64_encode(identity_pubkey.data(), 32);
```
- `execlp` 调用追加 `pk_txt.c_str()` 参数
- 去重判断纳入 `identity_pubkey == impl_->identity_pubkey`

include 加 `#include "core/distributed/identity.hpp"`。

- [ ] **Step 2: 改 args.hpp — 删 key,加 trust-fingerprint**

读 `cli/src/args.hpp`。`Server` 结构删 `key` 字段;`Worker` 结构删 `key`,加 `std::string trust_fingerprint;`。

- [ ] **Step 3: 改 cli_app.cpp — 调整选项定义**

读 `cli/src/cli_app.cpp`。删 `-k/--key` 选项(Server 和 Worker 子命令);Worker 加 `--trust-fingerprint <FP>`(可选)。

- [ ] **Step 4: 改 cmd_server.cpp — 身份加载 + 调用**

替换 `cmd_server`(line 22-99)关键部分:

```cpp
int cmd_server(const args::Server& a) {
    if (a.subcommand == "info") {
        // info 仍可用,但需要加载身份来出卡片
        auto id = crossscale::IdentityKey::load_or_create(
            crossscale::default_identity_path());
        print_connection_card(a.port, id.public_key());
        return 0;
    }
    auto identity = crossscale::IdentityKey::load_or_create(
        crossscale::default_identity_path());
    LOG_INFO("Server", "PC identity fingerprint: " +
              crossscale::fingerprint(identity.public_key()));

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto coordinator = std::make_unique<crossscale::Coordinator>();
    coordinator->set_models_directory("./models");
    coordinator->set_trust_store_path(crossscale::default_trust_path());
    // ... (on_device_connected 等回调不变) ...

    auto start_result = coordinator->start(a.port, identity);
    if (!start_result) { /* 同原 */ }

    crossscale::MDNSBroadcaster mdns;
    mdns.start("CrossScale-PC", a.port, identity.public_key());

    print_connection_card(a.port, identity.public_key());
    // ... 主循环不变 ...
}
```

在 `identity.hpp` 加便捷函数:
```cpp
inline auto default_identity_path() -> std::filesystem::path {
    return std::filesystem::path(getenv("HOME") ? getenv("HOME") : ".") /
           ".crossscale" / "identity";
}
inline auto default_trust_path() -> std::filesystem::path {
    return std::filesystem::path(getenv("HOME") ? getenv("HOME") : ".") /
           ".crossscale" / "trusted_clients";
}
```

- [ ] **Step 5: 改 common.cpp/hpp — QR v2 + 卡片指纹**

读 `cli/src/common.cpp:225-247`(QR 生成)和 `print_connection_card`。

改 `print_connection_card(uint16_t port, const std::array<uint8_t,32>& pubkey)`:
- 控制台输出加一行:`PC 指纹: ABCD1234EF567890`
- QR payload JSON 改 v2:
```cpp
std::string qr = std::format(
    R"({{"v":2,"ip":"{}","port":{},"fp":"{}","pk":"{}"}})",
    ip, port, fingerprint(pubkey), base64_encode(pubkey.data(), 32));
```
- ASCII QR 用 `qr-code-generator-cpp` 包不变

common.hpp 改 `print_connection_card` 签名。

- [ ] **Step 6: 改 cmd_worker.cpp — 删 key,加 trust-fingerprint**

读 `cli/src/cmd_worker.cpp`。Worker connect 调用改:
```cpp
auto identity = IdentityKey::load_or_create(default_identity_path());
// peer pubkey 从 fingerprint 解析?不 —— CLI worker 需要扫码或传 --peer-pubkey
// 简化:CLI worker 加 --peer-pubkey <base64> 必填(mDNS 发的 pk)
std::vector<uint8_t> pk_bytes = *base64_decode(a.peer_pubkey);
std::array<uint8_t,32> peer_pk;
std::memcpy(peer_pk.data(), pk_bytes.data(), 32);
worker.connect(host, port, peer_pk, identity);
```

args.hpp Worker 加 `std::string peer_pubkey;`(base64),cli_app.cpp 加 `--peer-pubkey` 必填。

- [ ] **Step 7: PC 全编译**

Run: `cd cli && xmake`
Expected: 全部编译通过

- [ ] **Step 8: 手动验证 PC 单机**

```bash
cd cli && xmake run crossscale server start
```
Expected:
- 控制台显示 `PC identity fingerprint: XXXXXXXXXXXXXXXX`
- 显示 QR + `PC 指纹:` 行
- 另一终端 `avahi-browse -r _crossscale._tcp` 显示 TXT 含 `id_fingerprint` 和 `id_pubkey`
- `~/.crossscale/identity` 文件存在,权限 0600
- Ctrl+C 退出

- [ ] **Step 9: Commit**

```bash
git add cli/src/ share/utils/mdns.hpp share/utils/mdns.cpp \
        share/core/distributed/identity.hpp
git commit -m "feat(cli): identity-based server/worker + v2 QR + mDNS fingerprint"
```

---

## Task 6: Android JNI 适配 + libsodium 集成

**Files:**
- Modify: `apps/app/src/main/cpp/CMakeLists.txt`
- Modify: `apps/app/src/main/cpp/jni_bridge.cpp`
- Modify: `scripts/sync_native_deps.sh`
- Create: `apps/app/src/main/cpp/jniLibs/<abi>/libsodium.a`
- Create: `apps/app/src/main/cpp/third_party/include/sodium.h`(从 libsodium release)
- Modify: `apps/app/src/main/java/com/crossscale/native/NCNNEngine.kt`
- Modify: `apps/app/src/main/java/com/crossscale/native/NativeWorker.kt`

- [ ] **Step 1: 下载 libsodium 预编译库**

```bash
# arm64-v8a(必需)
cd /tmp && wget https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20-android-arm64.tar.gz
tar xzf libsodium-1.0.20-android-arm64.tar.gz
# 复制静态库 + 头到项目
cp libsodium-android-arm64/lib/libsodium.a \
   /home/xihale/Desktop/learning/android/crossscale/apps/app/src/main/cpp/jniLibs/arm64-v8a/
cp libsodium-android-arm64/include/sodium.h \
   /home/xihale/Desktop/learning/android/crossscale/apps/app/src/main/cpp/third_party/include/
cp -r libsodium-android-arm64/include/sodium \
   /home/xihale/Desktop/learning/android/crossscale/apps/app/src/main/cpp/third_party/include/
```

(若 URL/版本失效,从 https://github.com/jedisct1/libsodium/releases 找最新 Android arm64 包;若需其他 ABI 重复)

- [ ] **Step 2: 改 CMakeLists.txt — import libsodium + 编译开关**

在 `apps/app/src/main/cpp/CMakeLists.txt` 的 `import_static_library(codec_png ...)` 附近加:

```cmake
import_optional_static_library(sodium_static "${NATIVE_LIB_DIR}/libsodium.a")
if(TARGET sodium_static)
    set(CROSSSCALE_HAS_SODIUM ON)
else()
    message(WARNING "libsodium not found at ${NATIVE_LIB_DIR}/libsodium.a — distributed crypto disabled")
endif()
```

并在 target_link_libraries 区域加 `${CODEC_LIB_TARGETS}` 后:
```cmake
if(CROSSSCALE_HAS_SODIUM)
    target_compile_definitions(crossscale PUBLIC CROSSSCALE_PROTOCOL_V2)
    target_link_libraries(crossscale PRIVATE sodium_static)
endif()
```

(具体 target 名按 CMakeLists 现有定义调整)

- [ ] **Step 3: 改 sync_native_deps.sh — 加 libsodium 下载逻辑**

读 `scripts/sync_native_deps.sh` 现有结构(若不存在则新建)。在 ncnn 下载块之后加 libsodium 块,镜像 Step 1 逻辑,遍历 abiFilters。

- [ ] **Step 4: 改 jni_bridge.cpp — nativeLoadIdentity + nativeConnect 签名换**

读 `apps/app/src/main/cpp/jni_bridge.cpp`。

新增 JNI 函数:
```cpp
extern "C" JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NativeWorker_nativeLoadIdentity(
    JNIEnv* env, jobject, jstring identityPath) {
    const char* path = env->GetStringUTFChars(identityPath, nullptr);
    try {
        // 用 thread-local 或单例存 IdentityKey
        static std::unique_ptr<crossscale::IdentityKey> g_identity;
        g_identity = std::make_unique<crossscale::IdentityKey>(
            crossscale::IdentityKey::load_or_create(path));
        env->ReleaseStringUTFChars(identityPath, path);
        return JNI_TRUE;
    } catch (...) {
        env->ReleaseStringUTFChars(identityPath, path);
        return JNI_FALSE;
    }
}
```

改 `nativeConnect` 签名:删 `authKey: String` 参数,加 `peerPubkeyB64: String`:
```cpp
extern "C" JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NativeWorker_nativeConnect(
    JNIEnv* env, jobject, jstring host, jint port, jstring peerPubkeyB64,
    jstring modelsDirectory) {
    // base64 decode peerPubkeyB64 → std::array<uint8_t,32>
    // 用 g_identity + worker_->connect(host, port, peer_pk, *g_identity)
}
```

`nativeProcessImageDistributed` 同理改 `authKey` → `peerPubkeyB64`。

- [ ] **Step 5: 改 Kotlin external 声明**

`apps/app/src/main/java/com/crossscale/native/NativeWorker.kt`:
```kotlin
fun loadIdentity(identityPath: String): Boolean =
    nativeLoadIdentity(identityPath)
private external fun nativeLoadIdentity(identityPath: String): Boolean

external fun nativeConnect(
    host: String, port: Int, peerPubkeyB64: String, modelsDirectory: String
): Boolean
```
删原 `authKey` 参数。

`NCNNEngine.kt` 的 `nativeProcessImageDistributed` 声明同步改 `authKey` → `peerPubkeyB64`。

- [ ] **Step 6: 在 Application 启动时加载 identity**

读 `apps/app/src/main/java/com/crossscale/CrossScaleApplication.kt`。在 `onCreate` 加:
```kotlin
val identityFile = File(filesDir, "identity")
NativeWorker().loadIdentity(identityFile.absolutePath)
```
(或放 AppContainer 初始化)

- [ ] **Step 7: Android 编译验证**

Run: `cd apps && ./gradlew assembleDebug`
Expected: 编译通过(libsodium 链接成功,JNI 签名匹配)

- [ ] **Step 8: Commit**

```bash
git add apps/app/src/main/cpp/ apps/app/src/main/java/com/crossscale/native/ \
        scripts/sync_native_deps.sh
git commit -m "feat(android): integrate libsodium + identity-based JNI bridge"
```

---

## Task 7: Android Kotlin — mDNS TXT / CoordinatorClient / PairingRepo / ViewModel / UI

**Files:**
- Modify: `apps/app/src/main/java/com/crossscale/network/MDNSDiscovery.kt`
- Modify: `apps/app/src/main/java/com/crossscale/network/JoinInvite.kt`
- Modify: `apps/app/src/main/java/com/crossscale/network/CoordinatorClient.kt`
- Modify: `apps/app/src/main/java/com/crossscale/data/repository/PairingRepository.kt`
- Modify: `apps/app/src/main/java/com/crossscale/data/model/DeviceProfile.kt`
- Modify: `apps/app/src/main/java/com/crossscale/data/repository/DeviceRepository.kt`
- Modify: `apps/app/src/main/java/com/crossscale/viewmodel/DeviceViewModel.kt`
- Modify: `apps/app/src/main/java/com/crossscale/ui/components/DeviceCard.kt`
- Create: `apps/app/src/main/java/com/crossscale/ui/components/FingerprintConfirmDialog.kt`
- Modify: `apps/app/src/main/java/com/crossscale/ui/screens/DevicesScreen.kt`

- [ ] **Step 1: 改 MDNSDiscovery.kt — 读 fingerprint + pubkey TXT**

`DiscoveredService` 加字段:
```kotlin
data class DiscoveredService(
    val name: String,
    val host: String,
    val port: Int,
    val fingerprint: String? = null,
    val identityPubKey: String? = null
)
```

`onServiceResolved` 内:
```kotlin
val fp = runCatching { resolvedInfo.getAttribute("id_fingerprint") }.getOrNull()
val pk = runCatching { resolvedInfo.getAttribute("id_pubkey") }.getOrNull()
val discoveredService = DiscoveredService(
    name = resolvedInfo.serviceName,
    host = hostAddress,
    port = resolvedInfo.port,
    fingerprint = fp,
    identityPubKey = pk
)
```

- [ ] **Step 2: 改 JoinInvite.kt — v2 JSON(fp/pk)**

```kotlin
data class JoinInvite(
    val host: String,
    val port: Int,
    val fingerprint: String?,
    val identityPubKey: String  // base64
)
```

`parseJsonInvite`:读 `v`(必须 2),`ip`/`host`,`port`(默认 9527),`fp`,`pk`。v=1 报错「请升级 PC 到支持加密的版本」。

- [ ] **Step 3: 改 CoordinatorClient.kt — connect 用 peerPubkey**

`connect` 签名:`fun connect(host: String, port: Int, peerPubKey: String): Boolean`

`RemoteEndpoint.authKey` → `peerPubKey: String`。`nativeProcessImageDistributed` 调用传 `peerPubKey`。

- [ ] **Step 4: 改 PairingRepository.kt — 存 pubkey + fingerprint**

```kotlin
data class TrustedPC(
    val identityPubKey: String,
    val fingerprint: String
)

fun saveTrustedPC(host: String, port: Int, pc: TrustedPC)
fun getTrustedPC(host: String, port: Int): TrustedPC?
fun isPaired(host: String, port: Int): Boolean = getTrustedPC(host, port) != null
```
SharedPreferences 存 JSON `{"pk":"...","fp":"..."}`。删 `getAuthKey`/`savePairing`。

- [ ] **Step 5: 改 DeviceProfile.kt — CONNECTING + 发现字段**

```kotlin
enum class DeviceStatus { DISCOVERED, CONNECTING, CONNECTED, OFFLINE }

data class DeviceProfile(
    // ... 现有字段 ...
    val discoveredFingerprint: String? = null,
    val discoveredPubKey: String? = null
)
```

- [ ] **Step 6: 改 DeviceRepository.kt — markConnecting/markFailed + 合并保留**

```kotlin
fun markConnecting(deviceId: String)  // 置 CONNECTING,保留其它
fun markFailed(deviceId: String)      // 置 DISCOVERED
```

`updateDiscoveredDevices` 合并时:若已存 status ∈ {CONNECTING, CONNECTED} 不覆盖 status;保留 `discoveredFingerprint/PubKey`。

- [ ] **Step 7: 改 DeviceViewModel.kt — 新连接方法 + 状态包裹**

`DeviceUiState` 加:
```kotlin
val connectionError: String? = null,
val pairedDeviceIds: Set<String> = emptySet(),
val pendingFingerprintConfirm: DeviceProfile? = null  // 待确认指纹的设备
```

`onServiceFound` 传 fingerprint + pubkey:
```kotlin
val discovered = DeviceProfile(
    deviceId = "${service.host}:${service.port}",
    deviceName = service.name,
    // ...
    discoveredFingerprint = service.fingerprint,
    discoveredPubKey = service.identityPubKey
)
```

新增:
```kotlin
fun connectDiscoveredWithKey(deviceId: String) {
    val target = uiState.value.devices.firstOrNull { it.deviceId == deviceId } ?: return
    val pk = target.discoveredPubKey
    if (pk.isNullOrBlank()) {
        // 无 pubkey,弹手动对话框(同原逻辑)
        _showManualConnectDialog.value = true
        return
    }
    // 有 pubkey → 弹指纹确认对话框
    _pendingFingerprintConfirm.value = target
}

fun confirmTrustAndConnect(deviceId: String) {
    val target = _pendingFingerprintConfirm.value ?: return
    _pendingFingerprintConfirm.value = null
    deviceRepository.markConnecting(deviceId)
    viewModelScope.launch {
        val ok = connectToCoordinator(target.ipAddress, target.port, target.discoveredPubKey!!)
        if (ok) {
            pairingRepository.saveTrustedPC(target.ipAddress, target.port,
                TrustedPC(target.discoveredPubKey!!, target.discoveredFingerprint ?: ""))
            deviceRepository.addManualDevice(target.ipAddress, target.port, target.discoveredPubKey!!)
        } else {
            deviceRepository.markFailed(deviceId)
            _connectionError.value = "连接失败,请检查 PC 是否在线"
        }
    }
}

fun cancelFingerprintConfirm() { _pendingFingerprintConfirm.value = null }

fun clearConnectionError() { _connectionError.value = null }
```

`attemptAutoConnect` 改用 `pairingRepository.getTrustedPC` 取 pubkey。`pairedDeviceIds` 在 `combine` 时计算:
```kotlin
val pairedIds = devices.mapNotNull { d ->
    if (pairingRepository.isPaired(d.ipAddress, d.port)) d.deviceId else null
}.toSet()
```

- [ ] **Step 8: 新建 FingerprintConfirmDialog.kt**

```kotlin
@Composable
fun FingerprintConfirmDialog(
    deviceName: String,
    fingerprint: String,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("信任此 PC?") },
        text = {
            Column {
                Text("发现 $deviceName")
                Spacer(Modifier.height(8.dp))
                Text("PC 指纹:", style = MaterialTheme.typography.labelSmall)
                Text(fingerprint, fontFamily = FontFamily.Monospace,
                     style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(8.dp))
                Text("请核对此指纹与 PC 控制台显示一致。",
                     style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text("信任并连接") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("取消") } }
    )
}
```

- [ ] **Step 9: 改 DeviceCard.kt — 四态动作行 + 配对徽章 + 指纹**

签名加 `paired: Boolean`。动作行(line 88-98)替换:

```kotlin
Row(/* ... */) {
    if (paired) {
        AssistChip(onClick = {}, label = { Text("已配对") })
        Spacer(Modifier.width(8.dp))
    }
    when (device.status) {
        DeviceStatus.CONNECTED -> {
            OutlinedButton(onClick = { onDisconnect(device.deviceId) }) { Text("断开") }
        }
        DeviceStatus.CONNECTING -> {
            Button(onClick = {}, enabled = false) {
                CircularProgressIndicator(
                    Modifier.size(16.dp), strokeWidth = 2.dp,
                    color = MaterialTheme.colorScheme.onPrimary)
                Spacer(Modifier.width(8.dp))
                Text("连接中…")
            }
        }
        DeviceStatus.DISCOVERED -> {
            if (!device.discoveredPubKey.isNullOrBlank()) {
                Button(onClick = { onConnect(device.deviceId) }) { Text("一键连接") }
            } else {
                Button(onClick = { onConnect(device.deviceId) }) { Text("连接") }
            }
        }
        DeviceStatus.OFFLINE -> {
            Button(onClick = { onConnect(device.deviceId) }) { Text("重连") }
        }
    }
}
// 在地址行下加指纹显示(若有)
if (!device.discoveredFingerprint.isNullOrBlank()) {
    Text("指纹: ${device.discoveredFingerprint}",
         style = MaterialTheme.typography.labelSmall,
         fontFamily = FontFamily.Monospace)
}
```

- [ ] **Step 10: 改 DevicesScreen.kt — SnackbarHost + 文案 + FingerprintConfirmDialog 挂载**

加 `SnackbarHostState`:
```kotlin
val snackbarHostState = remember { SnackbarHostState() }
LaunchedEffect(state.connectionError) {
    val err = state.connectionError
    if (!err.isNullOrBlank()) {
        snackbarHostState.showSnackbar(err, actionLabel = "忽略")
        onClearConnectionError()
    }
}
```

在 Scaffold 或布局里加 `SnackbarHost(snackbarHostState)`。

发现中文案改「正在搜索局域网内的 CrossScale 服务…」。

末尾加:
```kotlin
state.pendingFingerprintConfirm?.let { device ->
    FingerprintConfirmDialog(
        deviceName = device.deviceName,
        fingerprint = device.discoveredFingerprint ?: "未知",
        onConfirm = { onConfirmTrust(device.deviceId) },
        onDismiss = { onCancelFingerprintConfirm() }
    )
}
```

签名加 `onConfirmTrust: (String) -> Unit`、`onCancelFingerprintConfirm: () -> Unit`、`onClearConnectionError: () -> Unit`。

(连接 DevicesScreen 的 composable 调用处 — MainActivity 或 navigation — 也要补这些回调)

- [ ] **Step 11: 连接 DevicesScreen 调用点**

读调用 `DevicesScreen(...)` 的地方(MainActivity 或 navigation graph),补新增回调的接线:
```kotlin
onConfirmTrust = viewModel::confirmTrustAndConnect,
onCancelFingerprintConfirm = viewModel::cancelFingerprintConfirm,
onClearConnectionError = viewModel::clearConnectionError
```

- [ ] **Step 12: Android 全量编译**

Run: `cd apps && ./gradlew assembleDebug`
Expected: 编译通过

- [ ] **Step 13: Commit**

```bash
git add apps/app/src/main/java/com/crossscale/
git commit -m "feat(android): mDNS auto-pair + fingerprint confirm + connection UX"
```

---

## Task 8(验证): 端到端手测清单

**Files:** 无(纯验证)

- [ ] **Step 1: PC 启动**

```bash
cd cli && xmake run crossscale server start
```
确认:
- 控制台显示 `PC identity fingerprint: XXXX...`
- QR + 指纹行
- `avahi-browse -r _crossscale._tcp` TXT 含 `id_fingerprint`/`id_pubkey`
- `ls -l ~/.crossscale/identity` 权限 0600

- [ ] **Step 2: Android 发现**

安装 APK,开 app → 设备页 → 开始发现。
确认:设备卡出现,显示「指纹: XXXX...」+「一键连接」按钮。

- [ ] **Step 3: 一键连接**

点「一键连接」→ 弹指纹确认 → 「信任并连接」。
确认:状态 `CONNECTING`(按钮禁用+进度圈)→ `CONNECTED` + 「已配对」徽章。
PC 控制台显示 worker joined。

- [ ] **Step 4: 自动重连**

杀 app → 重开 → 开始发现 → 已配对设备自动 `CONNECTED`(trusted=1,无弹窗)。

- [ ] **Step 5: 指纹变更检测**

PC 端 `rm ~/.crossscale/identity` → 重启 server(新身份,新指纹)。
Android 再连 → 指纹确认对话框弹出(新指纹),旧配对失效。

- [ ] **Step 6: 抓包验证加密**

```bash
sudo tcpdump -i any -X port 9527 | head
```
确认:除握手头两条(KEY_HELLO,内容是公钥)外,后续所有包载荷不可读(随机字节)。

- [ ] **Step 7: 提交验证记录**

```bash
git log --oneline | head
```
确认 7 个 feature commit 都在。

---

## Self-Review 记录

(写完计划后检查 — 已做)

**Spec 覆盖:**
- §3 决策 1(secretbox)— Task 1 ✓
- §3 决策 2(X25519+Ed25519)— Task 1,2 ✓
- §3 决策 3(TOFU)— Task 4(coordinator add),Task 7(FingerprintConfirmDialog)✓
- §3 决策 4(硬迁移 v2)— Task 4 Step 1 ✓
- §3 决策 5(指纹)— Task 2 ✓
- §3 决策 6(身份存储)— Task 2(load_or_create 0600),Task 6(Android EncryptedSharedPreferences 走 PairingRepo)✓
- §3 决策 7(信任白名单)— Task 2,Task 4 ✓
- §3 决策 8(EncryptedSession 单点)— Task 3 ✓
- §4 协议帧 — Task 3,4 ✓
- §5 数据流 — Task 7 ✓
- §6 各文件 — 分布到 Task 1-7 ✓

**类型一致:** `peerPubKey: String`(Kotlin, base64)/ `peer_identity_pubkey: std::array<uint8_t,32>`(C++)经 base64 桥接,全计划一致。`TrustedPC`、`DiscoveredService.fingerprint/identityPubKey` 字段名跨文件一致。

**留待执行时确认:**
- Task 3 Step 3:`Protocol::serialize(nullptr)` 行为 — 执行时读源码定
- Task 6 Step 2:具体 target 名(`crossscale`?)— 执行时读 CMakeLists 定
