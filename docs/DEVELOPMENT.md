# Upix 开发规范

面向本仓库的贡献者：环境、构建、测试、发布流程、代码规范与已知坑。
架构层面的模块划分见 [ARCHITECTURE.md](ARCHITECTURE.md)（单一权威来源）；
用户向使用说明见根 [README.md](../README.md)；UI 规范见
[DESIGN_LANGUAGE.md](DESIGN_LANGUAGE.md)（规范性，改 UI 前必读）。

---

## 1. 环境要求

| 工具 | 版本 | 说明 |
|------|------|------|
| xmake | ≥ 3.0 | 双端 C++ 构建唯一入口 |
| C++ 编译器 | GCC 11+ / Clang 13+ | C++23（JNI 桥为 C++17） |
| JDK / Android SDK / NDK | NDK `29.0.14206865` | NDK 版本钉死在 `apps/app/build.gradle.kts` |
| Gradle | 9.7.1（wrapper） | `./gradlew` 自动拉取，镜像已配阿里云 |
| gh CLI | — | 发版（models release）用，`gh auth login` |

C++ 依赖（ncnn、libpng、libjpeg、libwebp、libdeflate、glaze、libsodium、cli11、
libcurl、openssl、libzip、qr-code-generator-cpp，可选 libavif）全部经
`xmake require` 拉取，无需手装。首次构建会下载，请耐心或自备镜像缓存。

---

## 2. 构建速查

### 2.1 PC CLI

```bash
scripts/build_cli.sh release                     # 推荐：总是先重配 linux/x86_64
scripts/build_cli.sh debug                       # 调试构建
scripts/build_cli.sh release --avif-encoder=y --avif-decoder=y
UPIX_AVIF_ENCODER=1 scripts/build_cli.sh release # 环境变量形式
```

产物：`build/linux/x86_64/release/upix-cli`（`upix-perf` 同目录）。

**不要**在做过 Android 构建后直接裸跑 `xmake`——xmake 每工程只有一份全局配置，
Android 配置会残留（这是 `build_cli.sh` 每次显式 `xmake f -p linux -a x86_64` 的
原因）。Android 构建同理会经由 `scripts/build_native.sh` 重配回去，两端交替安全。

### 2.2 Android App

```bash
cd apps
./gradlew assembleDebug                          # 自动先跑 xmakeNative 编 .so
./gradlew assembleRelease                        # 需签名环境变量，见 §5.3
./gradlew -Pupix.avifEncoder=true -Pupix.avifDecoder=true assembleRelease
```

`xmakeNative`（`app/build.gradle.kts`）在每次构建前调用
`scripts/build_native.sh <mode> arm64-v8a --avif-encoder=? --avif-decoder=?`，
产物 `.so` 自动拷入 `app/src/main/jniLibs/arm64-v8a/`。AVIF 开关以 gradle 属性为
唯一事实来源（见 §7 坑 1）。

### 2.3 主机工具

```bash
xmake build dump-spirv    # 重新生成 share/core/spirv_pack.bin（默认不构建）
xmake run dump-spirv -- share/core/spirv_pack.bin
xmake build upix-perf     # 性能探针
```

### 2.4 调试原生代码

release `.so` 被 strip，符号全无。排查 native 崩溃/GPU 垃圾输出时用
`scripts/build_native.sh debug arm64-v8a` 重编 + gdb/ndk-stack；PC 侧对应
`scripts/build_cli.sh debug`。

---

## 3. 测试

### 3.1 C++ 单元测试（host）

每个测试一个独立二进制（自带 CHECK 宏 + abort，无框架），定义在 `cli/xmake.lua`：

```bash
xmake build test_crypto && xmake run test_crypto        # 加密/会话/信任库
xmake build test_protocol && xmake run test_protocol    # 帧协议 round-trip
xmake build test_sr_marker    # XMP 标记四格式
xmake build test_image_avif   # AVIF 编解码（依赖系统/内置 libavif）
xmake build test_gpu_precision
xmake build test_model_downloader
```

改动加密、协议、图像编解码、sr_marker 后必须跑对应测试。改 GPU kernel 后跑
`UPIX_KERNEL_SELFTEST=1` 冒烟（见坑 4）。

### 3.2 Android

```bash
cd apps && ./scripts/android_test.sh          # 单元测试 + assembleDebug (+lint)
./gradlew connectedDebugAndroidTest           # 仪器测试（需设备）
```

仪器测试 3 个（`apps/app/src/androidTest/`）：`GpuProbeDiagTest`（GPU 诊断）、
`LocalNativeBenchmarkTest`（本地引擎基准）、`RemoteDistributedTest`（分布式全链）。

### 3.3 端到端

```bash
scripts/test_remote_e2e.sh    # 起 upix host --auto-approve + 跑 RemoteDistributedTest
```

验证 ECDH 握手 + 配对 + SUBMIT/RESPONSE 全链路。改握手/配对逻辑后必跑。

### 3.4 性能基准

`scripts/bench_images.sh` / `benchmark_dir_eff.sh` 等为维护者本机基准脚本；
通用探针用 `upix-perf {list-probes|gpu-efficiency|gpu-concurrency|gpu-reuse}`。
GPU 管线改动要求 md5/PSNR 双端验证输出不变（性能优化不得改像素）。

---

## 4. 代码规范

### 4.1 C++（share/ 与 cli/）

- **C++23**（JNI 桥除外）；风格跟随现有代码：`snake_case` 文件/函数、
  `PascalCase` 类型、每文件头部一段职责注释。
- **share/ 必须平台干净**：同一份源文件编 PC 和 Android（glob 自动收集，见
  `share/xmake.lua`）。禁止未条件化的 POSIX-only 头（AF_UNIX 等已按平台剔除，
  新增平台相关文件要同步改 glob 排除表）。
- **新增 `.cpp` 无需登记**：glob 收集，但**新增平台限定文件**（如仅 host）必须改
  `share/xmake.lua` 的条件 `add_files`。
- 自定义 Vulkan kernel 的 SSBO 一律 `uint` 字（Adreno 规避，见 ARCHITECTURE §2.2），
  禁止标量 f16/u8 SSBO 成员。
- 日志走 `share/utils/logger.hpp`（`UPIX_LOG_LEVEL` 可调），不要 printf。
- 错误处理：可预期失败用返回值/`std::optional`/`expected.hpp`；协议错误必须回传
  per-task 错误而不是断连。

### 4.2 Kotlin（apps/）

- MVVM：UI 只读 ViewModel 的 `StateFlow`；业务进 `data/repository/`；JNI 封装只在
  `nativelib/`。
- UI 一切 token（颜色/间距/形状/动效/字体）必须走 `ui/theme/*`，规范与审查 grep 见
  DESIGN_LANGUAGE.md §11，**改 UI 前先读它**。
- 字符串资源化（`StringProvider`）；日志界面英文枚举保留是策略（见设计语言 amendment）。

### 4.3 JNI 桥（`apps/app/src/main/cpp/jni_bridge.cpp`）

- 单文件、C++17；新 native 能力先在 share/ 实现，桥只做参数转换与生命周期。
- JNI 引用（`FindClass`/`NewStringUTF` 等）注意局部表溢出，循环里用
  `DeleteLocalRef`；已有模式照抄。
- ncnn/引擎 stderr 在 Android 不可见——诊断走 `nativeRedirectStderr` 回传日志。

### 4.4 提交纪律

- 提交信息中文/英文均可，格式 `type(scope): 摘要`（见 git log 惯例：feat/fix/build/
  refactor/ui）。
- 性能/像素相关改动必须附验证手段（测试输出、md5/PSNR 对比）。
- release 相关文件（`apps/app/build.gradle.kts` 版本、根 `xmake.lua` set_version）
  只在发版提交中改动。

---

## 5. 版本与发布

### 5.1 版本真源

| 渠道 | 真源 | 注入 |
|------|------|------|
| CLI | 根 `xmake.lua` `set_version()` | `cli/xmake.lua` `on_load` 拼 `UPIX_VERSION="v<ver><caps>"` → `upix --version` |
| Android | `apps/app/build.gradle.kts` `versionName/versionCode` | 与 CLI 版本独立演进 |
| 模型 | release `manifest.json` `version` 字段 | `scripts/publish_models.py` 维护 |

两条产品线独立发版，tag 前缀分离：`cli-v*`、`android-v*`、`models`（模型滚动
release，不打新 tag）。

### 5.2 发版清单

1. 改版本号（对应真源），提交。
2. 打 tag：`cli-v0.x.y` 或 `android-v0.x.y` 并推送。
3. CLI 附件二进制（如有 CI/手动）：`scripts/build_cli.sh release` 产物上传 release。
4. 模型：`./scripts/publish_models.py`（见 `models/README.md`，`--overwrite` 覆盖，
   默认跳过已存在的）。新增/转换模型走 `models/CONVERTING.md` 的完整规程
   （来源对账 → spandrel 锚定 → pnnx 转换 → 验证门 → 文档同提交）。

### 5.3 Android release 签名

release 构建强制独立 keystore，环境变量 `UPIX_RELEASE_*`（store 文件、密码、alias）；
缺失时构建失败，除非显式 `-Pupix.allowDebugSignedRelease`——不要用 debug key
发 release（MIUI 等会因签名不一致拒绝覆盖安装，见坑 7）。

---

## 6. 运行时环境变量（调试旋钮）

| 变量 | 作用 |
|------|------|
| `UPIX_MODELS_DIR` | 模型目录覆盖（默认 `$XDG_DATA_HOME/upix/models`） |
| `UPIX_LOG_LEVEL` | 日志级别 |
| `UPIX_PROGRESS` | 目录模式进度输出 |
| `UPIX_KERNEL_SELFTEST=1` | 引擎初始化时跑 GPU kernel round-trip 自检 |
| `UPIX_PIPELINE_TIMER` | GPU 流水线分段时间日志 |
| `UPIX_GPU_DISABLE_FP16` / `UPIX_GPU_DISABLE_INT8` / `UPIX_GPU_ENABLE_FP16_ARITH` | 覆盖精度 profile |
| `UPIX_GPU_FORCE_BUDGET_MB` | 覆盖 VRAM/heap 预算（默认按设备估算，如 8GB 4060 ≈ 6.1GB） |
| `UPIX_GPU_INIT_RETRIES` / `UPIX_GPU_INIT_DELAY_MS` | GPU 初始化重试 |
| `UPIX_CPU_THREADS_PER_ENGINE` | 引擎池每实例线程数 |

---

## 7. 已知坑（踩过的，别再踩）

1. **xmake option 跨 `xmake f` 粘连**：`--avif-encoder` 等选项会残留在全局配置里。
   所有脚本和 Gradle 都**显式传 `y/n`**，新加选项时照做；手动 `xmake f` 后构建结果
   可疑先 `xmake f --menu` 或重配检查。
2. **xmake 单全局配置**：host/android 配置互相覆盖。host 侧工具要在 android 配置
   激活时构建，用隔离 `XMAKE_CONFIGDIR`（参考 `share/xmake.lua` before_build 对
   dump-spirv 的处理）。`build_cli.sh` / `build_native.sh` 每次全量重配，交替使用安全。
3. **`spirv_pack.bin` 的 `.incbin` 依赖不被追踪**：重新生成 pack 后必须 touch
   `spirv_pack.S` 否则静默发旧 shader。`share/xmake.lua` 的 before_build 钩子已自动
   处理（pack 较新即 touch）；手动绕过构建系统操作 pack 文件时要记得这点。
4. **改自定义 GPU kernel 后**：跑 `UPIX_KERNEL_SELFTEST=1` 自检 + 一张真实图双端
   （PC + Android 真机）输出比对。Adreno 的标量 SSBO bug 是**静默垃圾输出**不报错，
   见 ARCHITECTURE §2.2 的规避政策。
5. **多个 `before_build` 注册只有第一个生效**：xmake 不累积钩子，往 target 加钩子
   必须并进现有函数（`share/xmake.lua` 已有注释）。
6. **探针/测试缓冲按 scale² 预算**：写新的 GPU 探针时输出缓冲要按放大倍数平方预留，
   否则大倍率下越界。
7. **Android 覆盖安装签名不一致直接装不上**（MIUI 尤甚，且安装框可能被自动点掉）：
   卸载再装；release 永远用 release key。
8. **协议能力位是双侧协商的**：AVIF 输入/输出走远程时必须检查 CONNECT_RESPONSE 的
   `avif_decode`/`avif_encode` 位（app 已门控）；PC 侧编解码失败回 per-task 错误，
   不许断连。
9. **输出图都带 SR XMP 标记**（`urn:upix:sr:1`），回喂默认跳过。新写的批处理/测试
   若需重复处理同一张图，要么 `-f`，要么用独立输出目录。

---

## 8. 文档维护

| 文档 | 何时更新 |
|------|----------|
| `docs/ARCHITECTURE.md` | 模块增删、调用链变化、协议变化——它是权威，过期即 bug |
| `docs/DEVELOPMENT.md`（本文） | 流程/环境/坑清单变化 |
| `docs/DESIGN_LANGUAGE.md` | 一切 UI token/组件规范变化（含 amendment 记录） |
| 根 `README.md` | 用户可见行为、CLI 面、构建说明变化 |
| `models/README.md` | 模型目录/发版流程变化 |
| `models/CONVERTING.md` | 模型转换/验收规程变化（新架构、新 pnnx/ncnn 坑） |

历史文档（serve daemon 设计、ADR、性能报告）已删除，需要时从 git 历史找回；
不要重新往 `docs/` 堆一次性报告。
