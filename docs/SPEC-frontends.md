# SPEC: 前端一律平等(First-class frontends)

> 状态:生效中(2026-11-07)。本文约束所有前端(CLI / desktop / Android)与 `share/`
> 的关系;与代码冲突时以代码为准,并请回来修文档。背景:2026-10~11 的 Rust/GPUI
> 重写探索(分支 `rust-gpui-desktop`)虽被搁置,但其架构结论对本仓库成立,本文是
> 那次探索的经验在 C++ 侧的落地规格。

## 0. 教训(来自探索)

1. **同一套业务逻辑写两遍,必然漂移**。模型家族分组表在 `desktop/qml/Grouping.js`
   一份、Kotlin `ModelScenes.kt` 一份(注释自认"literal port")、探索分支又镜像了一
   份——三份各自演化。领域知识必须只有一份。
2. **GUI 若靠解析另一个前端的 stdout 工作,它就是二等公民**:功能受限于文本协议、
   需要版本门禁(`kMinCliVersion`)、测试要靠假 CLI 脚本。Android 没有这个问题
   (JNI 直链 `libupix-native.so`),desktop 有。
3. **进程边界要用结构化协议,不用文本**:share 的 serve daemon 协议
   (`ServeRequest/Response/Progress`,CRC32 校验帧)是正确的边界形态;UPIX_PROGRESS
   JSON 行是 CLI 的展示层,不是接口。
4. **跨切面不变式必须由核心执行**:SR 标记的幂等跳过语义只存在于 CLI 调用方时,
   任何新调用方(桌面委派、未来前端)都会漏掉它。
5. **重写的价值在于逼出这些不变式,而不是替换语言**。

## 1. 不变量

- **S1 单一共享核**:引擎、模型管理、批处理管线、编码、SR 标记、配对协议、
  下载器全部在 `share/`;前端只做 UI、输入收集与设置持久化。禁止前端复制
  share 已有的领域知识(表、命名规则、格式)。
- **S2 前端一等公民**:CLI、desktop、Android 功能面对等。desktop 的主流水线
  (单文件 job)必须走 share 的进程内接口或 daemon 协议;文本回退路径
  允许存在,但必须显式标注 `TRANSITIONAL`(见 §3 阶段表)。
- **S3 领域知识单一来源**:场景表 `model_metadata.hpp`、模型家族分组
  `model_families.hpp`、输出命名 `resolve_output_path`、指纹格式
  (SHA-256 前 16 字节大写)、SR 标记语义 `sr_marker` 各只有一份,在 share。
- **S4 不变式由核心执行**:SR 标记跳过语义由 daemon 执行(不止 CLI 调用方);
  任何协议调用方都能获得一致的幂等行为。
- **S5 版本门禁只放在真正的跨二进制边界**:手机↔PC 配对协议。同机的
  desktop↔daemon 因同仓同版发布,不加版本位,按 §4 的容忍规则演进。
- **S6 测试随架构**:share 层纯函数可 headless 测试;desktop 测试打真组件
  (`tst_compare_drag` 模式);回退路径由假 CLI 脚本覆盖(`tst_desk_jobs`)。

## 2. 现状审计(desktop 的 QProcess 触点)

| 触点 | 现状 | 目标阶段 |
|---|---|---|
| `job`(单文件) | daemon 协议委派(AI_UNIX 平台、GPU、固定 tile、非 auto-tile) | **已完成**(阶段1) |
| `job`(目录) | share `run_batch` 进程内直调(真核,含 SR 跳过) | **已完成**(阶段2,2026-11-07) |
| `models`/`remote`/`import` | share `ModelManager` 直调 | **已完成**(阶段2) |
| `download`/`dstatus` | share `download_model` + 进度回调直调 | **已完成**(阶段2) |
| `daemon`(start/stop/status) | 自执行 `--serve-daemon` 模式 + pid/控制端直调 | **已完成**(阶段2) |
| `host`(配对协调) | share `Coordinator` 进程内直调(worker 线程) | **已完成**(阶段2) |
| `version`(门禁) | 已删除(同机同版,S5) | **已删除**(阶段2) |
| 家族分组表 | `model_families.hpp` 单一来源 | **已完成**(阶段0) |
| SR 标记执行点 | daemon 强制执行 | **已完成**(阶段0) |
| Windows 桥 | QProcess + CLI(上表触点在该平台保留) | TRANSITIONAL |

## 3. 阶段与验收

- **阶段0(本次)**:S3/S4 落地——`model_families.hpp` 单一来源,desktop 消费;
  serve 协议加 `force`/`skipped`,daemon 执行 marker 跳过。
  验收:`tst_desk_jobs` 全绿(含新分组 golden 与 daemon 路径测试);xmake 构建 CLI 通过。
- **阶段1(已落地,2026-11-06)**:desktop 单文件 job 走 daemon 协议(AI_UNIX 平台、GPU、固定 tile、
  非 auto-tile 时);目录/Windows/daemon 不在线时回退 QProcess(TRANSITIONAL,
  由假 CLI 测试覆盖)。
  验收:进度/取消/跳过/出错四条路径都有测试;实机跑通一次委派 upscale。
- **阶段2(已落地,Linux 2026-11-07)**:模型管理、daemon 监管、host、目录批全部
  直调 share 类;Linux/桌面构建不再依赖兄弟 `upix-cli`;
  `findCli`/`kMinCliVersion`/版本门禁已删除(同机同版,S5)。
  daemon 监管 = desktop 以 `--serve-daemon` 模式自执行(fork+setsid+execv,
  源为 `/proc/self/exe`),pid/stop/控制端均直调 share;
  host = `Coordinator` 在 worker 线程内运行,`build_connection_card` 产 v2 卡。
  验收:desktop 不含 CLI 仍全功能(单文件委派、目录直批、模型列表/下载、
  daemon 启停、host 卡片,全部实测);测试套全绿(队列/快照/取消/失败/
  目录批真核跑通)。
  **Windows 例外**:AF_UNIX 不可用,Windows 桌面保留阶段0 桥(QProcess+CLI,
  `Desk::spawnCliJob` 等以 `Q_OS_WIN` 隔离),测试相应分支/SKIP,待 Windows
  的进程内方案单独决策(命名管道或同一套 share 类的 WinSock AF_UNIX)。
- **Windows 说明**:AF_UNIX 不可用,阶段1 委派路径编译隔离,Windows 桌面按
  阶段0 形态(QProcess)运行,直到阶段2 决定 Windows 的进程内方案。

## 4. 同机 daemon 协议演进规则

desktop 与 daemon 同仓发布,但用户可能跑着旧 daemon、装了新桌面。规则:

1. 新字段一律**追加在结构体尾部**,旧字段前缀偏移不变;
2. daemon 读请求时容忍 `payload_length >= sizeof(ServeRequest)`(旧结构尺寸),
   缺失的尾部字段按零处理;客户端读响应容忍 `payload < sizeof(ServeResponse)`;
3. 任何破坏性重排 = 新消息类型,不走本规则。

## 5. 待办清单(不在本次范围)

- [ ] Android `ModelScenes.kt` 改为经 JNI 取 `model_metadata`(删 Kotlin 副本)。
- [ ] Windows 桥的进程内方案(命名管道 / WinSock AF_UNIX + 同一套 share 类)。
- [x] 阶段2 全部触点(§2 表,Linux;2026-11-07)。
- [ ] CLI 的 UPIX_PROGRESS JSON 发射点迁移到 share 的进度事件结构,
      machine schema 获得唯一权威定义。
