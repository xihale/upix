# Android ↔ PC mDNS 自动配对 + 连接 UX 打磨设计

- **日期**: 2026-08-05
- **状态**: 设计已确认,待实现
- **范围**: 安卓端 + PC mDNS 广播;不动 scheduler / JNI / 协议帧 / serve daemon

## 1. 背景与问题

当前 Android↔PC 联动代码骨架完整(QR/mDNS/手动三种入口、auth 握手、Android 作 client 与 worker 两种角色均通),但 mDNS 自动配对链路存在两处断点,连接 UX 缺少状态反馈:

| # | 问题 | 位置 |
|---|---|---|
| 1 | PC 只在 mDNS TXT 里发 `auth_key_hash`(sha256 前 16 位),只能校验不能认证 | `share/utils/mdns.cpp:120,137` |
| 2 | Android `MDNSDiscovery` 完全不读 TXT,发现只拿到 host:port,首次配对必须扫码或手输 key | `apps/.../network/MDNSDiscovery.kt:51-60` |
| 3 | `DeviceStatus` 只有 `DISCOVERED/CONNECTED/OFFLINE`,连接中无表示,失败只 log 不反馈 | `apps/.../data/model/DeviceProfile.kt:22-26` |
| 4 | `DeviceCard` 二元状态(连/断),不区分已配对/新发现,无进度、无错误提示 | `apps/.../ui/components/DeviceCard.kt:88-98` |

## 2. 目标与非目标

### 目标
- 局域网内 Android 自动发现 PC 并**一键连接**(无需扫码或手输 key)
- 设备卡区分「已配对」「新发现带 key」「新发现无 key」三种,动作行相应变化
- 连接过程可视化(连接中状态),失败给可操作的错误提示
- PC 端 TXT 向后兼容(旧 Android 仍可读 hash,新 Android 读 full key)

### 非目标(本次不做)
- scheduler 重试 / worker 掉线恢复(属「断线重连」方向)
- 链接加密(明文 key 通过明文 TCP 传输已是现状,威胁模型为可信局域网)
- Android 当 coordinator(改动过大,不在本次范围)
- serve daemon 联动(PC 本地 Unix socket,与 Android 无关)
- 改 `ConnectionDialog` / `QRScannerScreen` / `CoordinatorClient` / JNI / 协议帧

## 3. 关键决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 密钥传播 | mDNS TXT 明文发完整 `auth_key`,保留 `auth_key_hash` 兼容 | 明文 key 本就明文 TCP 传输,不降低安全等级;最简单 |
| 首次配对时机 | 用户点「一键连接」成功后才 `savePairing`,**不**静默自动配对 | 避免静默信任陌生广播;与 QR 路径(`connectByQrPayload:123`)一致 |
| 已配对重连 | 维持现状,`attemptAutoConnect` 只对已存 key 静默重连 | 不扩大自动行为范围 |
| 连接中状态 | `DeviceStatus` 加 `CONNECTING` 枚举值 | 比「独立 connectingDeviceIds set」更简单,UI 单一数据源 |
| 发现的 key 存储 | `DeviceProfile.discoveredAuthKey: String?`(临时,UI 用) | 正式配对仍走 `PairingRepository`,职责不混 |
| 错误反馈 | `DeviceUiState.connectionError: String?` + Snackbar | 一次性、展示即清,不持久化 |
| 配对徽章数据源 | ViewModel 构建 `DeviceUiState` 时查 `PairingRepository.getAuthKey` | 不污染 `DeviceProfile` 持久模型 |

## 4. 架构

### 4.1 数据流(改动后)

```
PC mdns.cpp                Android MDNSDiscovery.kt
─────────────              ────────────────────────
TXT: version=1             onServiceResolved
      role=coordinator       host = hostAddress
      auth_key=<full>   ◀──  port = port
      auth_key_hash=<16>     key  = getAttribute("auth_key")  [新]
                              │
                              ▼
                            DeviceViewModel.onServiceFound
                              DeviceProfile(
                                discoveredAuthKey = service.key,  [新]
                                status = DISCOVERED)
                              │
              ┌───────────────┼─────────────────┐
              ▼               ▼                 ▼
        attemptAutoConnect  (等待用户)      connectDiscoveredWithKey [新]
        (仅已配对,静默)     DeviceCard 显示   用 discoveredAuthKey 直连
                           「一键连接」      成功→savePairing+CONNECTED
                                            失败→markFailed+回退手动对话框
```

### 4.2 状态机(DeviceStatus)

```
                  ┌──────────────────────────────────────┐
                  ▼                                      │
DISCOVERED ──connect──▶ CONNECTING ──成功──▶ CONNECTED   │
    ▲                       │                            │
    │                       失败                         │
    └───────────────────────┘                            │
                  markFailed                             │
                                                         │
CONNECTED ──disconnect──▶ OFFLINE ──rediscover──▶ DISCOVERED
```

## 5. 详细改动

### 5.1 PC: `share/utils/mdns.cpp`

- `Impl` 加字段 `std::string auth_key;`(line 67-76 区域)
- `start()` 签名不变(已收 `auth_key` 参数),内部:
  - 计算 hash 时同时保留入参 `auth_key` 到 `impl_->auth_key`
  - 去重判断(line 96-101)纳入 `auth_key == impl_->auth_key`
  - TXT 列表(line 117-137)在现有三项后加 `auth_key_txt = "auth_key=" + auth_key`,作为 `execlp` 的额外参数

### 5.2 Android: `apps/.../network/MDNSDiscovery.kt`

- `DiscoveredService` data class(line 9-13)加 `val key: String? = null`
- `onServiceResolved`(line 51-60):
  ```kotlin
  val key = runCatching { resolvedInfo.getAttribute("auth_key") }.getOrNull()
  val discoveredService = DiscoveredService(
      name = resolvedInfo.serviceName,
      host = hostAddress,
      port = resolvedInfo.port,
      key = key
  )
  ```
  (`NsdServiceInfo.getAttribute(String): String?` 是 API 21+ 标准方法,无需新依赖)

### 5.3 Android: `apps/.../data/model/DeviceProfile.kt`

- `enum class DeviceStatus`(line 22-26):加 `CONNECTING`
- `data class DeviceProfile`(line 3-15):加 `val discoveredAuthKey: String? = null`

### 5.4 Android: `apps/.../data/repository/DeviceRepository.kt`

- 新增 `fun markConnecting(deviceId: String)`:把该 device 置 `CONNECTING`,保留其它字段
- 新增 `fun markFailed(deviceId: String)`:把该 device 置回 `DISCOVERED`
- `updateDiscoveredDevices`(line 35-89):合并时若已存 device 状态为 `CONNECTING` 或 `CONNECTED`,不被新发现覆盖(保留进度/连接);若已存有 `discoveredAuthKey` 也保留不丢

### 5.5 Android: `apps/.../viewmodel/DeviceViewModel.kt`

- `startDiscovery` 的 `onServiceFound`(line 77-91):构造 `DeviceProfile` 时传 `discoveredAuthKey = service.key`
- 新增 `fun connectDiscoveredWithKey(deviceId: String)`:
  - 查 device 的 `discoveredAuthKey`,空则回退 `connectDevice`
  - `markConnecting` → `connectToCoordinator(ip, port, key)` → 成功:`savePairing` + `addManualDevice`;失败:`markFailed` + 设 `connectionError` + 回退 `_showManualConnectDialog=true`
- `connectDevice`(line 151-203)、`connectByQrPayload`(line 114-130)、`connectManual`(line 132-149)各连接调用点包裹 `markConnecting/markFailed`,失败时设 `connectionError`
- `DeviceUiState`(line 26-32)加 `val connectionError: String? = null`;`combine` 流纳入一个 `_connectionError: MutableStateFlow<String?>`;新增 `fun clearConnectionError()` 清空(Snackbar 展示后调)
- 构建 `devices` 时为每个 device 计算并附 `isPaired`(查 `pairingRepository.getAuthKey(ip,port) != null`);方式:`DeviceUiState` 新增 `val pairedDeviceIds: Set<String>`,不污染 `DeviceProfile` 持久模型

### 5.6 Android: `apps/.../ui/components/DeviceCard.kt`

`DeviceCard` 签名(line 20-25)加 `paired: Boolean`。`onConnect` 回调复用现有的——ViewModel 在 `connectDevice` 内部根据 device 是否有 `discoveredAuthKey` 分流(有则走 `connectDiscoveredWithKey` 路径,无则弹手动对话框),减少 UI 层新回调。

动作行(line 88-98)从二元改四态:

| 状态 | UI |
|---|---|
| `CONNECTED` | 「断开」按钮(同今) |
| `CONNECTING` | 禁用按钮 + 小 `CircularProgressIndicator` + 「连接中…」 |
| `DISCOVERED` + 有 `discoveredAuthKey` | 「一键连接」填充按钮(`onConnect` 复用,ViewModel 分流到 `connectDiscoveredWithKey`) |
| `DISCOVERED` + 无 key | 「连接」按钮(弹手动对话框,同今) |

顶部加配对徽章:`paired` 为真时显示「已配对」小 `AssistChip`/文本标签。

### 5.7 Android: `apps/.../ui/screens/DevicesScreen.kt`

- `DevicesScreen` 签名加 `onClearConnectionError: () -> Unit`(line 29-39 区域)
- 包一层 `SnackbarHost`:`state.connectionError != null` 时 `SnackbarHostState.showSnackbar(message, actionLabel="忽略")`,展示后调 `onClearConnectionError`
- 发现中文案(line 65-69)从「发现中…」改为「正在搜索局域网内的 CrossScale 服务…」
- 透传 `paired` 到 `DeviceCard`

## 6. 测试策略

无新增单测框架(项目现以 instrumented test 为主)。验证靠:

- **PC 端单元验证**:`mdns.cpp` 改动小,用现有 `xmake build` 编译 + 手动跑 `cli server start`,用 `avahi-browse -r _crossscale._tcp` 确认 TXT 含 `auth_key=`
- **Android 构建**:`cd apps && ./gradlew assembleDebug` 通过
- **端到端手测**(真机 + PC 同局域网):
  1. PC `xmake run crossscale server start` → 确认 QR/TXT
  2. Android 开 app → 开始发现 → 设备卡出现且带「一键连接」
  3. 点「一键连接」→ 状态 `CONNECTING` → 成功 `CONNECTED` + 出现「已配对」徽章
  4. 杀 PC → Android `OFFLINE`;重启 PC → 再发现 → 自动重连(已配对路径)
  5. 模拟 key 错误(改 PC key 不重启广播)→ Snackbar 显示错误,回退手动对话框
- **回归**:QR 扫码、手动连接、本地 device 流程不受影响

## 7. 风险与缓解

| 风险 | 缓解 |
|---|---|
| `getAttribute` 在个别 OEM ROM 行为不一 | 已用 `runCatching` 兜底,null 时退化为「无 key,弹手动对话框」,与现状一致 |
| TXT 明文 key 被同网段他人嗅探 | 明文 key 本就明文 TCP 传输,不引入新风险;文档已界定可信局域网模型 |
| `CONNECTING` 状态卡死(网络 hang) | 后续「断线重连」方向会加超时;本次连接调用本身沿用 `CoordinatorClient.connect` 的既有 socket 超时 |
| `updateDiscoveredDevices` 保留逻辑破坏既有合并 | 只在状态为 `CONNECTING/CONNECTED` 时跳过覆盖,`DISCOVERED/OFFLINE` 走原逻辑 |

## 8. 不验证的事项

- Android 当 coordinator(非目标)
- scheduler/worker 失败恢复(非目标)
- 链接加密(非目标)
- `DATA_FLOW.md` 调度循环描述过时(独立文档问题,另行处理)

## 9. 受影响文件清单

| 文件 | 改动类型 |
|---|---|
| `share/utils/mdns.cpp` | TXT 加 `auth_key`,Impl 加字段 |
| `apps/.../network/MDNSDiscovery.kt` | 读 TXT,`DiscoveredService.key` |
| `apps/.../data/model/DeviceProfile.kt` | `CONNECTING` 枚举,`discoveredAuthKey` 字段 |
| `apps/.../data/repository/DeviceRepository.kt` | `markConnecting/markFailed`,合并保留逻辑 |
| `apps/.../viewmodel/DeviceViewModel.kt` | `connectDiscoveredWithKey`,状态包裹,`connectionError`,`pairedDeviceIds` |
| `apps/.../ui/components/DeviceCard.kt` | 四态动作行 + 配对徽章 |
| `apps/.../ui/screens/DevicesScreen.kt` | Snackbar + 文案 + paired 透传 |
