# serve E2E 测试套件 — 取舍记录

沉淀自 2026-09-02 的 daemon 暴力测试（5 个修复：payload 炸弹 / 字段越界 /
zombie pid 罚时 / socket 清理 / 目录创建）。原则：每条用例必须锚定一个
**具体的校验分支或已修复的 bug**，否则不进套件。

## 从临时套件中删掉的（冗余/显然）

| 删掉 | 原因 |
| --- | --- |
| connect+close、garbage、wrong-magic、wrong-type、short-payload 各自独立 canary | 同属"畸形输入被拒"，合并为一批，只查存活，批末一次 canary |
| bad-checksum | 校验和本地信任模型下故意不验——测的是不存在的约定 |
| slow-header slowloris | 已知设计局限（单连接线程阻塞，无锁持有），本地攻击面无行动项 |
| empty/text/png-header-only/devnull 损坏输入 | 与 truncated-png 走同一 "load failed" 分支，留 3 个代表性损坏即可 |
| fuzz 侧 mid-inference-hangup | 与 lifecycle L5 完全重复，保留带后续验证的 L5 |
| 16 客户端各格式独立 canary | 响应已含 w/h，像素校验统一离线做 |

## 保留的（每条锚定一处代码）

- protocol：超限 payload（F1）、非 NUL 字段（F2）、路径长度不匹配/零长、
  非 UTF8 路径、连接风暴 64、64 路并发存活。
- concurrency：48 请求格式×并发像素位级（编码器确定性）、8 路同路径碰撞
  （原子发布）、模型热切换（blob 纪律）、inline+daemon 同 GPU。
- lifecycle：SIGKILL 恢复、双 daemon 拒绝、stop/idle 清理、中断后无残留、
  junk pidfile 0.1s fallback（F3）、junk 普通文件不被清（F4）。
- io：3 类损坏输入、4 个尺寸边界（1x1 / 1x300 / 300x1 / 奇宽）、嵌套输出
  目录（F5）、只读目录、怪文件名、SR 标记 refeed、batch 坏成员不中断、
  4 输出格式矩阵（avif 按能力自动跳过）。

## 已知非确定性（断言据此定标，勿收紧回位级）

- **C4 跨路径**：daemon 引擎与 CLI 引擎 spawn 时的空闲 VRAM / 线程拓扑不同，
  归约调度随之漂移——实测 GPU 与 CPU 上均为 PSNR 26.8–27.2dB、maxdiff
  43–64 的整体亮度微移，两簇各自位级稳定；仅存在单一引擎时两路径位级
  一致。阈值 PSNR≥24 / maxdiff≤96 用于抓撕裂/黑带/错模型（远低于线下）。
- **tile 划分对输出无影响**：256/384/auto 交叉验证位级一致（4-pass
  disjoint tile batching 的设计目标），但 auto-tile 的取值随环境变，因此
  跨路径对比必须**显式钉 tile** 才有意义。
- **并发 fp16**：两进程同时跑时单侧输出也会漂移；并发阶段只断言稳定性
  与尺寸，位级等价只在串行输出间断言。
