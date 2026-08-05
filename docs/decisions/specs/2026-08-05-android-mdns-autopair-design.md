# Android ↔ PC 端到端加密 + mDNS 自动配对设计

- **日期**: 2026-08-05
- **状态**: 设计已确认,待实现
- **范围**: PC CLI + 安卓端 + 共享 C++ 协议层;引入端到端加密与身份认证,替换明文 auth_key
- **前置决策**: 全程加密 / libsodium / 文件&Keystore 原生存储 / 硬迁移(不兼容 v1)
- **不动**: serve daemon(AF_UNIX 本地)、scheduler、ncnn engine、UI 主屏结构

## 1. 背景与问题

当前 Android↔PC 联动骨架完整(QR/mDNS/手动三入口、auth 握手、Android 作 client 与 worker 两角色),但:

| # | 问题 | 位置 |
|---|---|---|
| 1 | 明文 auth_key 直接走 TCP(`AuthRequest.auth_key[64]`),无链路加密 | `share/core/distributed/protocol.hpp:46-48`, `worker.cpp:99-109` |
| 2 | PC 只在 mDNS TXT 发 `auth_key_hash`(sha256 前 16 位),只能校验不能认证;Android 完全不读 TXT | `share/utils/mdns.cpp:120,137`, `MDNSDiscovery.kt:51-60` |
| 3 | 首次配对必须扫码或手输 key,mDNS 无法自动配对 | `DeviceViewModel.kt:196-202` |
| 4 | `DeviceStatus` 只有 `DISCOVERED/CONNECTED/OFFLINE`,连接中无表示,失败只 log 不反馈 | `data/model/DeviceProfile.kt:22-26` |
| 5 | `DeviceCard` 二元状态,不区分已配对/新发现,无进度、无错误提示 | `ui/components/DeviceCard.kt:88-98` |

本设计**一并**解决加密(1)与自动配对(2-5)。

## 2. 目标与非目标

### 目标
- 端到端加密:握手用 X25519 ECDH + Ed25519 签名,数据流用 `crypto_secretbox`(XSalsa20-Poly1305)
- 身份认证:PC/Android 各持持久 Ed25519 身份密钥对,TOFU 信任模型
- mDNS 自动发现 + 指纹确认:Android 发现 PC → 用户看一眼指纹确认 → 自动配对,无需扫码
- 加密覆盖全部 TCP 消息(AUTH/PROFILE/TASK/TILES/CLIENT_SUBMIT 等均加密)
- 设备卡区分「已配对」「新发现」三种状态,动作行相应变化,连接中可视化,失败给可操作提示
- 三入口统一改为基于身份公钥:QR / mDNS / 手动

### 非目标(本次不做)
- scheduler 重试 / worker 掉线恢复(「断线重连」方向另行处理)
- Android 当 coordinator
- serve daemon 加密(AF_UNIX 本地,无需求)
- 兼容旧版 v1 协议(硬迁移;见 §3 决策 4)
- PC 跨平台 mDNS 广播(仍仅 Linux/avahi;非 Linux 平台走 QR/手动)

## 3. 关键决策

| # | 决策点 | 选择 | 理由 |
|---|---|---|---|
| 1 | 对称加密原语 | `crypto_secretbox`(XSalsa20-Poly1305),**非** AES-GCM | libsodium 默认原语,零硬件依赖;`crypto_aead_aes256gcm` 在无 AES-NI/ARMv8 Crypto 的 CPU 上不可用 |
| 2 | 密钥协商 | X25519 ECDH(ephemeral) + Ed25519 签名(身份) | libsodium 原生支持,握手两轮、相互签名(Noise 风格) |
| 3 | 信任模型 | TOFU + 指纹确认 | 首次连接用户看一眼 PC 指纹确认,之后自动;PC 身份变更检测指纹不符 → 重弹确认(防 MITM) |
| 4 | v1 兼容 | **硬迁移**,删除 `AuthRequest.auth_key`,协议升 v2 | 局域网自部署、非公开分发;双协议并存代码复杂度不值 |
| 5 | 指纹格式 | sha256(身份公钥) 前 8 字节 hex,如 `ABCD1234EF567890` | 16 字符足够防碰撞,人眼可读,易在 UI/QR/控制台对照 |
| 6 | 身份密钥存储 | PC:`~/.crossscale/identity`(0600,Ed25519 seed 32B);Android:`EncryptedSharedPreferences`(已用于 PairingRepository) | 文件/Keystore 原生,无需硬件背书;Android 走既有 MasterKey 路径 |
| 7 | 信任白名单 | PC:`~/.crossscale/trusted_clients`(每行一个 base64 客户端身份公钥);Android:`PairingRepository` 存 PC 身份公钥(替代 auth_key) | 双向 TOFU:PC 记得哪些 Android 连过,Android 记得哪些 PC 信任过 |
| 8 | 加密包装位置 | `Protocol::serialize/parse` 层新增 `EncryptedSession` 包装;所有 socket send/recv 经它 | 单点接入,worker/coordinator 的读写代码不感知加密细节 |
| 9 | 首次配对时机 | 用户在 Android 指纹确认对话框点「信任并连接」后才写信任白名单 | 不静默信任陌生广播 |
| 10 | 已配对重连 | 维持现状自动重连逻辑,改用身份公钥识别 | 不扩大自动行为范围 |

## 4. 协议

### 4.1 MessageType 改动(`share/core/distributed/protocol.hpp`)

```cpp
// 新增
MSG_KEY_HELLO     = 0x0007,  // C→S / S→C:交换 ephemeral + identity 公钥
MSG_AUTH_PROOF    = 0x0008,  // C→S:Ed25519 签名(用会话密钥加密)

// 复用现有码点,但 payload 改变(加 trusted 字段,见 §6.2)
MSG_AUTH_RESPONSE = 0x0004,  // S→C:{ authenticated, device_id, trusted }(加密)
                             //   trusted=0 首次(需 Android 端 TOFU),trusted=1 已在白名单
MSG_DISCONNECT    = 0x0005,  // payload 不变(加密)
MSG_HEARTBEAT     = 0x0006,  // payload 不变(加密)
```

`MSG_AUTH_REQUEST = 0x0003` **删除**(v2 不再有明文 key)。

### 4.2 帧格式:v2 加密帧

握手完成后,所有 `Protocol::serialize` 产生的明文帧被 `EncryptedSession` 包装:

```
[ MessageHeader(15B, type=原类型, payload_length=密文长度) ]
[ nonce(24B) ]
[ ciphertext(原 payload + 16B Poly1305 MAC) ]
```

- header 本身明文(对端需读 type 决定是否解密);header.checksum 仍存**明文 payload 的 CRC32**(便于错误诊断)
- nonce 由 `crypto_secretbox` 要求的 24B 随机生成;通信双方各维护计数器防重放(本次先不做严格防重放,nonce 随机已够;留 TODO)
- 握手阶段的 `MSG_KEY_HELLO` 不加密(公钥本就公开);`MSG_AUTH_PROOF`/`MSG_AUTH_RESPONSE` 起进入加密

### 4.3 握手时序(3 报文相互签名)

```
C (Android/CLI worker)                     S (PC Coordinator)
─────────────────────                      ────────────────────
生成 c_eph = X25519 keypair                 生成 s_eph = X25519 keypair
c_id = 持久 Ed25519 身份                     s_id = 持久 Ed25519 身份

MSG_KEY_HELLO {                            MSG_KEY_HELLO {
  c_eph_pub[32],                             s_eph_pub[32],
  c_id_pub[32]                               s_id_pub[32],
}                                            s_sig[64] = Ed25519_sign(s_id_priv,
                                              SHA256(c_eph||c_id||s_eph||s_id))
                                          }
                    ◀────────────────
校验 s_sig 用 s_id_pub
派生 session_key = X25519(c_eph, s_eph)
c_sig = Ed25519_sign(c_id_priv, SHA256(transcript))

MSG_AUTH_PROOF {                           ─▶  解密校验 c_sig 用 c_id_pub
  c_sig[64]                                    查 c_id_pub 是否在 trusted_clients
} (secretbox 加密)                              ├ 在 → trusted=1
                                                └ 不在 → trusted=0(首次)
                                              派生同样的 session_key(c_eph, s_eph)

                     MSG_AUTH_RESPONSE {
                       authenticated,         校验 c_sig
                       device_id[36],         ├ 过 → authenticated=1
                       trusted                └ 不过 → 0,断开
                     } (secretbox 加密)

握手完成,后续所有帧 secretbox 加密
```

- `authenticated=1, trusted=0`:认证通过(签名有效)但 PC 之前没见过这个 Android → Android 端弹「PC 指纹 ABCD1234,信任?」(PC 指纹从 mDNS TXT 或 QR 取,与 s_id_pub 校验一致才信任)
- Android 用户确认信任 → 写 `PairingRepository` 存 s_id_pub → 重连时 PC 已把 c_id_pub 加入白名单 → trusted=1

## 5. 架构

### 5.1 模块划分(C++ 侧新增)

```
share/
├── core/
│   └── distributed/
│       ├── crypto_session.{hpp,cpp}   ← 新:EncryptedSession(secretbox 包装/解包)
│       ├── identity.{hpp,cpp}         ← 新:IdentityKey(Ed25519 load/generate/sign/verify)
│       └── protocol.{hpp,cpp}         ← 改:MSG_KEY_HELLO/AUTH_PROOF,删 MSG_AUTH_REQUEST
└── utils/
    ├── crypto.cpp                     ← 改:删 sha256_hex 之外的 auth_key 用途;新增 fingerprint()
    └── crypto_sodium.{hpp,cpp}        ← 新:libsodium 初始化 + 薄封装(X25519/Ed25519/secretbox)
```

`crypto_session` 是加密的**唯一接入点**。`Worker::handshake` / `Coordinator` 的连接处理改为:

```cpp
// 发送:明文帧 → serialize → EncryptedSession::seal → socket->send_all
// 接收:socket->receive_all(header) → read nonce → receive ciphertext → EncryptedSession::open → parse
```

握手前(发/收 MSG_KEY_HELLO)不加密;握手完成调 `session.activate(session_key)` 后才 seal/open。

### 5.2 数据流(改动后)

```
PC 启动(server start)
  ├─ 加载或生成 ~/.crossscale/identity (Ed25519)
  ├─ 启 Coordinator(身份密钥注入,不再有 auth_key)
  ├─ mDNS TXT: version=2, role=coordinator,
  │            id_fingerprint=<sha256(s_id_pub)[0:8] hex>,
  │            id_pubkey=<base64(s_id_pub)>           ← 新(供 Android 比对)
  └─ QR/connection card: 含 fp + pk

Android 发现
  ├─ MDNSDiscovery 读 TXT → DiscoveredService(fingerprint, identityPubKey)
  ├─ DeviceViewModel:onServiceFound
  │    DeviceProfile(discoveredFingerprint, discoveredPubKey, status=DISCOVERED)
  │    ┌─ 已在 PairingRepository(存过 s_id_pub)→ attemptAutoConnect(静默)
  │    └─ 未配对 → DeviceCard 显示「发现,指纹 ABCD1234,一键连接」
  └─ 用户点「一键连接」
       ├─ 弹「PC 指纹 ABCD1234,信任并连接?」(指纹取 mDNS TXT 与 QR 一致才显示绿勾)
       ├─ 确认 → NativeWorker.connect(host, port, identityPubKey)
       │         (JNI 内部:加载 Android 身份 → ECDH 握手 → 加密 worker session)
       ├─ 成功 → PairingRepository.saveTrustedPC(host, port, s_id_pub) + CONNECTED
       └─ 失败 → markFailed + connectionError + 回退手动对话框
```

### 5.3 DeviceStatus 状态机

```
DISCOVERED ──connect──▶ CONNECTING ──成功──▶ CONNECTED
    ▲                       │
    │                       失败
    └───────────────────────┘

CONNECTED ──disconnect──▶ OFFLINE ──rediscover──▶ DISCOVERED
```

加 `CONNECTING` 枚举值。

## 6. 详细改动

### 6.1 新增 C++ 模块

#### `share/utils/crypto_sodium.{hpp,cpp}`
- `SodiumInit()` — 进程级 `sodium_init()`,幂等
- `X25519Keypair` / `Ed25519Keypair` 结构 + `generate_*()` / `load_*()` / `save_*()`
- `x25519_shared_secret(my_priv, peer_pub) -> [32]`
- `ed25519_sign(priv, msg) -> [64]` / `ed25519_verify(pub, sig, msg) -> bool`
- `secretbox_seal(key, plaintext) -> {nonce[24], ciphertext}` / `secretbox_open(key, nonce, ciphertext) -> optional<plaintext>`

#### `share/core/distributed/identity.{hpp,cpp}`
- `class IdentityKey` — 持久 Ed25519 身份
  - `load_or_create(path)` — PC 路径 `~/.crossscale/identity`;Android 通过 JNI 传 app 内部存储路径
  - `public_key() -> [32]`, `sign(msg) -> [64]`, `verify(pub, sig, msg) -> bool`
- `fingerprint(pubkey) -> string` — sha256 前 8 字节 hex 大写
- `class TrustStore` — PC 信任白名单
  - `load(path)`, `add(pubkey)`, `contains(pubkey) -> bool`, `save()`
  - PC 路径 `~/.crossscale/trusted_clients`

#### `share/core/distributed/crypto_session.{hpp,cpp}`
- `class EncryptedSession`
  - `bool active()`, `void activate(const SessionKey&)`
  - `seal(const MessageHeader&, const void* plaintext_payload) -> vector<uint8_t>` — 产出完整 wire bytes(header 改写 payload_length + 追加 nonce + 密文)
  - `open(const uint8_t* wire, size_t) -> optional<pair<MessageHeader, vector<uint8_t>>>` — 校验 MAC,返回明文 header + 明文 payload

### 6.2 改 `share/core/distributed/protocol.hpp`

- 删 `MSG_AUTH_REQUEST = 0x0003`
- 加 `MSG_KEY_HELLO = 0x0007`,`MSG_AUTH_PROOF = 0x0008`
- 删 `struct AuthRequest`(明文 key);`struct AuthResponse` 加 `uint8_t trusted` 字段(改 layout,反正 v2 不兼容 v1)
- 加 `struct KeyHelloMessage { uint8_t eph_pub[32]; uint8_t id_pub[32]; uint8_t sig[64]; }`(sig 在 S→C 时填,C→S 留空)
- 加 `struct AuthProofMessage { uint8_t sig[64]; }`
- `MessageHeader.version` 升 2

### 6.3 改 `share/core/distributed/worker.cpp`

- `connect()` 签名:`(host, port, peer_identity_pubkey)` 替代 `(host, port, auth_key)`;内部加载本地 `IdentityKey`
- `handshake()` 重写为 §4.3 时序:发 KEY_HELLO → 收 KEY_HELLO 验签 → 派生 session_key → 发 AUTH_PROOF(加密)→ 收 AUTH_RESPONSE(加密)
- 握手后所有 send/recv 经 `EncryptedSession::seal/open`
- `MSG_HEARTBEAT` / `MSG_DEVICE_PROFILE` 发送路径改走加密

### 6.4 改 `share/core/distributed/coordinator.cpp` + `coordinator_admin.cpp`

- `Coordinator::start(port, identity)` — 参数从 `auth_key` 改 `IdentityKey&`
- 连接处理(`handle_connection` line ~170-260)重写:收 KEY_HELLO → 发 KEY_HELLO 验签 → 派生 session_key → 收 AUTH_PROOF → 验签 → 查 TrustStore → 发 AUTH_RESPONSE
- `authenticate(conn, pubkey)` 替代 `authenticate(conn, auth_key)` — 验签而非比字符串
- 后续 client submit / worker task 全程加密

### 6.5 改 `share/utils/mdns.cpp`

- `MDNSBroadcaster::start(service_name, port, identity_pubkey, fingerprint)` 替代 `(name, port, auth_key)`
- TXT:`version=2`,`role=coordinator`,`id_fingerprint=<8B hex>`,`id_pubkey=<base64 32B>`
- 删 `auth_key_hash` TXT

### 6.6 改 `cli/src/cmd_server.cpp` + `common.cpp`

- `cmd_server`:启动时 `IdentityKey::load_or_create("~/.crossscale/identity")`,传给 Coordinator 和 mDNSBroadcaster
- `print_connection_card`:QR payload 改 v2 JSON:`{"v":2,"ip":..,"port":..,"fp":"ABCD1234..","pk":"<base64>"}`
- 控制台卡片显示指纹行(`PC 指纹: ABCD1234EF567890`)
- 删 auth_key 自动生成逻辑(line 30-33)

### 6.7 改 `cli/src/cmd_worker.cpp`

- `worker connect host:port` 不再要 `-k key`;改为读本地 identity 自动 ECDH
- 可选 `--trust-fingerprint <FP>` 命令行参数(CLI worker 也走指纹确认,无交互时用此参数)

### 6.8 改 `cli/xmake.lua` + `apps/app/src/main/cpp/CMakeLists.txt`

- PC xmake:`add_packages(..., "libsodium")`(xrepo 有官方包)
- Android CMake:用**预编译静态库**方案——从 libsodium 官方 release(stallshop/libsodium)下载 `libsodium-x.x.x-android-arm64.tar.gz` 等,解压取 `libsodium.a` 放 `apps/app/src/main/cpp/jniLibs/<abi>/`;`CMakeLists.txt` 加 `find_library(SODIUM libsodium PATHS ${CMAKE_SOURCE_DIR}/jniLibs/${ANDROID_ABI})` + `target_link_libraries(..., ${SODIUM})`;支持 abiFilters 里的所有 ABI(arm64-v8a 必需,armeabi-v7a/x86/x86_64 按需)
- 两端编译定义 `CROSSSCALE_PROTOCOL_V2`

### 6.9 改 `apps/app/src/main/cpp/jni_bridge.cpp`

- `NCNNEngine.nativeProcessImageDistributed` 签名:`authKey: String` → `peerPubkey: String`(base64);内部走加密
- `NativeWorker.nativeConnect(host, port, peerPubkey)` 替代 `authKey`
- 新增 `nativeLoadIdentity(identityPath): Boolean`(启动时加载 Android 身份,或懒加载)

### 6.10 改 Android Kotlin

#### `network/MDNSDiscovery.kt`
- `DiscoveredService` 加 `fingerprint: String?`,`identityPubKey: String?`
- `onServiceResolved`:`getAttribute("id_fingerprint")`,`getAttribute("id_pubkey")`

#### `network/JoinInvite.kt`
- `JoinInvite`:`authKey: String` → `fingerprint: String?`,`identityPubKey: String`
- 解析 v2 JSON(`fp`,`pk`);v1 JSON(`key`)丢弃并报「请用新版 PC」

#### `network/CoordinatorClient.kt`
- `connect(host, port, peerPubkey)` 替代 `authKey`
- `RemoteEndpoint.authKey` → `peerPubKey`

#### `data/repository/PairingRepository.kt`
- `savePairing(host, port, authKey)` → `saveTrustedPC(host, port, identityPubKey, fingerprint)`
- `getAuthKey` → `getTrustedPC(host, port): TrustedPC?`(含 pubkey + fingerprint)
- `isPaired(host, port): Boolean`

#### `data/model/DeviceProfile.kt`
- `enum DeviceStatus` 加 `CONNECTING`
- `DeviceProfile` 加 `discoveredFingerprint: String? = null`,`discoveredPubKey: String? = null`

#### `data/repository/DeviceRepository.kt`
- 加 `markConnecting(deviceId)`,`markFailed(deviceId)`
- `updateDiscoveredDevices` 保留 `CONNECTING/CONNECTED` 不被覆盖,保留 `discoveredFingerprint/PubKey`

#### `viewmodel/DeviceViewModel.kt`
- `onServiceFound` 传 fingerprint + pubkey 进 DeviceProfile
- 新增 `connectDiscoveredWithKey(deviceId)`(用 discoveredPubKey 直连)
- 新增 `confirmTrustAndConnect(deviceId, confirmedFingerprint)` — 用户确认指纹后调用
- 所有连接调用点包裹 `markConnecting/markFailed`,失败设 `connectionError`
- `DeviceUiState` 加 `connectionError: String?`,`pairedDeviceIds: Set<String>`

#### `ui/components/DeviceCard.kt`
- 签名加 `paired: Boolean`
- 动作行四态:`CONNECTED`/`CONNECTING`(进度圈)/`DISCOVERED+pubkey`(一键连接)/`DISCOVERED+无pubkey`(手动)
- 顶部加配对徽章 + 指纹显示(`指纹: ABCD1234`)

#### `ui/components/FingerprintConfirmDialog.kt`(新)
- 「PC 指纹 `ABCD1234EF567890`,信任并连接?」+ 确认/取消
- 确认调 `confirmTrustAndConnect`

#### `ui/screens/DevicesScreen.kt`
- 加 `SnackbarHost` 展示 `connectionError`
- 发现中文案「正在搜索局域网内的 CrossScale 服务…」
- 透传 paired / 指纹到 DeviceCard;挂载 FingerprintConfirmDialog

## 7. 测试策略

### 7.1 单元测试(C++ 侧,新增)
- `crypto_sodium`:X25519 双向派生一致(KAT 向量)、Ed25519 sign/verify、secretbox seal/open 往返
- `crypto_session`:seal→open 往返、篡改密文 MAC 失败、nonce 长度
- `identity`:load_or_create 幂等、fingerprint 稳定
- `TrustStore`:add/contains/save/load 往返
- 放 `share/tests/`(若无可加 xmake test target)

### 7.2 编译验证
- PC:`cd cli && xmake`(确认 libsodium 链接)
- Android:`cd apps && ./gradlew assembleDebug`(确认 jniLibs + CMake)

### 7.3 端到端手测
1. PC `xmake run crossscale server start` → 确认控制台显示指纹 + QR v2 + mDNS TXT 含 `id_fingerprint`/`id_pubkey`
2. `avahi-browse -r _crossscale._tcp` → 确认 TXT 字段
3. Android 开 app → 开始发现 → 设备卡出现「指纹 ABCD1234,一键连接」
4. 点一键连接 → FingerprintConfirmDialog → 确认 → CONNECTING → CONNECTED + 「已配对」徽章
5. 杀 PC → Android OFFLINE;重启 PC(同一 identity 文件)→ 自动重连(trusted=1)
6. 删 PC identity 文件重启(模拟换机)→ Android 检测指纹不符 → 重弹确认
7. CLI worker `crossscale worker connect host:port --trust-fingerprint ABCD...` → 连上
8. Wireshark 抓包:除握手头两条公钥外,后续全部不可读

### 7.4 回归
- QR 扫码(v2 payload)、手动连接(输指纹/pubkey)、本地 device 流程不受影响
- serve daemon 命令不受影响

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| libsodium Android jniLibs 集成繁琐 | 优先用 xmake 交叉编译产出 `.a`;失败回退官方预编译包;CMake 加 ABI 检测 |
| Ed25519/X25519 自实现风险 | 不自实现,全部走 libsodium |
| 中间人篡改 mDNS TXT 的 pubkey | Android 端:指纹来自 mDNS TXT,但与 QR 交叉比对时取 QR 为准(扫码是带外);纯 mDNS 首次配对依赖用户肉眼核对指纹(与 SSH TOFU 同模型) |
| v2 不兼容旧版 Android | 局域网自部署,文档说明需同步升级;Protocol version=2 旧版直接拒连并报清晰错误 |
| `EncryptedSession` 单点包装漏改某条 send/recv 路径 | 加编译期检查:`TCPSocket::send_all` 在 `EncryptedSession::active()` 后被直接调用 → 编译警告(用 `[[deprecated]]` 标记裸 send 或 code review 清点) |
| nonce 随机碰撞(理论) | 24B 随机空间,单会话消息量远不到碰撞阈值;留 TODO 后续加计数器防重放 |
| 身份密钥文件权限泄漏 | 创建即 `chmod 0600`;Android 走 app 私有存储 + EncryptedSharedPreferences |

## 9. 不验证的事项

- Android 当 coordinator(非目标)
- scheduler/worker 失败恢复(非目标,「断线重连」方向)
- serve daemon 加密(非目标,本地 AF_UNIX)
- 严格防重放(nonce 计数器)— 留 TODO
- PC 跨平台 mDNS(仍 Linux/avahi)

## 10. 受影响文件清单

### 新增源文件(4 组,7 文件)
| 文件 | 内容 |
|---|---|
| `share/utils/crypto_sodium.hpp` + `.cpp` | libsodium 薄封装 |
| `share/core/distributed/identity.hpp` + `.cpp` | IdentityKey + TrustStore + fingerprint |
| `share/core/distributed/crypto_session.hpp` + `.cpp` | EncryptedSession(secretbox 包装) |
| `apps/app/src/main/java/com/crossscale/ui/components/FingerprintConfirmDialog.kt` | 指纹确认对话框 |

### 新增二进制
| 文件 | 说明 |
|---|---|
| `apps/app/src/main/cpp/jniLibs/<abi>/libsodium.a` | 每个 ABI 一份(arm64-v8a 必需,余按 abiFilters) |

### 改 C++ / 构建(11)
| 文件 | 改动 |
|---|---|
| `share/core/distributed/protocol.hpp` | 删 MSG_AUTH_REQUEST/AuthRequest,加 MSG_KEY_HELLO/AUTH_PROOF,AuthResponse 加 trusted,version=2 |
| `share/core/distributed/worker.cpp` | connect 签名换 pubkey,handshake 重写,读写走 EncryptedSession |
| `share/core/distributed/coordinator.cpp` | start 取 identity,handle_connection 重写握手,认证改验签 |
| `share/core/distributed/coordinator_admin.cpp` | authenticate(conn, pubkey) 验签 |
| `share/utils/mdns.cpp` | TXT 改 id_fingerprint/id_pubkey,start 签名换 |
| `share/utils/crypto.cpp` | 删 auth_key 相关;fingerprint() 归入 identity 模块则此处仅留 sha256_hex/uuid |
| `cli/src/cmd_server.cpp` | 身份加载,调用改 identity |
| `cli/src/common.cpp` | QR v2 + connection card 指纹 |
| `cli/src/cmd_worker.cpp` | 删 -k,加 --trust-fingerprint |
| `cli/xmake.lua` | add libsodium |
| `apps/app/src/main/cpp/CMakeLists.txt` | jniLibs libsodium find_library + CROSSSCALE_PROTOCOL_V2 |
| `apps/app/src/main/cpp/jni_bridge.cpp` | nativeProcessImageDistributed/nativeConnect 签名换 pubkey,nativeLoadIdentity |

### 改 Android Kotlin(9)
| 文件 | 改动 |
|---|---|
| `network/MDNSDiscovery.kt` | 读 id_fingerprint/id_pubkey TXT |
| `network/JoinInvite.kt` | v2 JSON(fp/pk),删 authKey |
| `network/CoordinatorClient.kt` | connect 用 peerPubkey |
| `data/repository/PairingRepository.kt` | 存 identityPubKey+fingerprint,删 authKey |
| `data/model/DeviceProfile.kt` | CONNECTING,discoveredFingerprint/PubKey |
| `data/repository/DeviceRepository.kt` | markConnecting/markFailed,合并保留 |
| `viewmodel/DeviceViewModel.kt` | connectDiscoveredWithKey,confirmTrustAndConnect,状态包裹,connectionError,pairedDeviceIds |
| `ui/components/DeviceCard.kt` | 四态动作行 + 配对徽章 + 指纹 |
| `ui/screens/DevicesScreen.kt` | SnackbarHost,文案,paired 透传,FingerprintConfirmDialog 挂载 |

## 11. 实现顺序建议

1. **C++ 加密地基**:`crypto_sodium` → `identity` → `crypto_session` + 单测
2. **协议层**:`protocol.hpp` v2 结构 + `EncryptedSession` 接入 `Protocol::serialize` 路径
3. **PC 握手**:`worker.cpp` + `coordinator.cpp` 重写,本地 CLI worker↔server 自测
4. **mDNS + QR**:`mdns.cpp` TXT + `cmd_server`/`common.cpp` 卡片
5. **Android JNI**:身份加载 + `jni_bridge.cpp` 签名改造 + libsodium 集成
6. **Android Kotlin**:`PairingRepository` → `MDNSDiscovery` → `CoordinatorClient` → `DeviceViewModel` → UI
7. **端到端手测**:按 §7.3 全流程

每步可独立编译验证,降低集成风险。
