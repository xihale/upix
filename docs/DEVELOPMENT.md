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
| C++ 编译器 | GCC 14+ / Clang 18+ | C++23（JNI 桥为 C++17）；CLI 用 `<print>`，GCC 14 才有 |
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

`xmakeNative`（`app/build.gradle.kts`）挂在 `merge{Debug,Release}JniLibFolders`
上，只在打 APK 时调用 `apps/scripts/build_native.sh`；`testDebugUnitTest` 不编
native。产物 `.so` 拷入 `app/src/main/jniLibs/arm64-v8a/`。AVIF 开关以 gradle
属性为唯一事实来源（见 §7 坑 1）。CI 分组、缓存与本地 `act` 打通见 §9。

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

### 3.1b serve daemon E2E 套件

黑盒打 daemon 全链路（协议健壮性 / 并发 / 生命周期 / 输入输出滥用）。改
`share/core/serve`、`share/utils/unix_socket.cpp`、`cli/src/cmd_process.cpp`
守护路径、pid 护栏、保存管线后**必跑**：

```bash
scripts/run_serve_tests.sh                    # CPU 档（~3 分钟，默认）
scripts/run_serve_tests.sh --device gpu       # GPU 档（~2 分钟）
scripts/run_serve_tests.sh --suite lifecycle  # 单套件
scripts/run_serve_tests.sh --keep-work        # 保留现场调试
```

前提：`scripts/build_cli.sh release` 已产出二进制；模型在
`~/.local/share/upix/models`（`upix-cli model download NomosUni-SPAN-x2
HFA2k-LUDVAE-SPAN-x2`）。依赖 python3 + numpy + Pillow。每条用例锚定的
分支、以及并发 fp16 漂移等**非确定性断言的定标依据**见
`docs/serve-tests-design.md`——不要收紧回位级比较。

ASAN 档：`xmake f -m debug && xmake build -y upix-cli`（debug 自带
ASAN），跑
`scripts/run_serve_tests.sh --binary build/linux/x86_64/debug/upix-cli --device cpu`，
跑完切回 release。

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
- **边界规则（2026-09-15 架构评审立）**：jni_bridge 是 Android 客户端专用胶水，
  **新会话策略（流水深度/重试/取消语义等）一律加在 Kotlin 侧消费既有导出**，
  不进 native；出现第二个 C++ 客户端时先把客户端会话提取到
  `share/core/distributed/`，不许在桥里长出第二份实现。
- JNI 引用（`FindClass`/`NewStringUTF` 等）注意局部表溢出，循环里用
  `DeleteLocalRef`；已有模式照抄。
- ncnn/引擎 stderr 在 Android 不可见——诊断走 `nativeRedirectStderr` 回传日志。

### 4.4 模型 scale 约定（单一实现）

- xN 文件名约定只有一份 C++ 实现：`core/model_graph.cpp` 的 `scale_from_filename`
  （引擎无上采样器兜底与 ModelManager 扫描共用）；图检测 `detect_model_graph`
  是纯 param 分析，不许在别处重写。
- 改约定/解析/图检测后必须跑 `test_model_catalog`（对 `scripts/publish_models.py`
  的 `scale_from_name` 三方钉一致性；本机模型库全量验证图检测与文件名标签一致、
  无模型落入静默 2x 默认）。新模型入库跑不跑得看本机有没有下载，CI 无模型时跳过。

### 4.5 提交纪律

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

两条产品线独立发版，**不要**再打裸 `v*` tag：

| tag | 工作流 | 产物 |
|-----|--------|------|
| `cli-v0.x.y` | `.github/workflows/release-cli.yml` | `upix-cli-{ver}-linux-x86_64.tar.gz` + `upix-cli-{ver}-macos-arm64.tar.gz` |
| `android-v0.x.y` | `.github/workflows/release-android.yml` | 签名 `upix-android-{ver}-arm64-v8a.apk` |
| `models` | 手动 `publish_models.py` | 模型 zip（滚动 release，不打新 tag） |

CLI `0.1.0` 和 Android `0.1.0` 只是数字碰巧相同，互不绑定。

### 5.2 发版清单

1. 改对应真源版本号，提交并推到 `main`。
2. 打 **一条** 产品线的 tag：`cli-v0.x.y` **或** `android-v0.x.y`，推送。
   也可在 Actions 里 `workflow_dispatch` 填 version。
3. 发版工作流编译对应平台 → 冒烟 → 建 GitHub release 并上传附件。
4. 模型：`./scripts/publish_models.py`（见 `models/README.md`）。

### 5.3 Android release 签名

release 构建强制独立 keystore。本地：

```bash
source ~/.config/upix/android-release.env   # 导出 UPIX_RELEASE_*
cd apps && ./gradlew assembleRelease
```

GitHub Actions 走 Environment `release` 的 secrets（`ANDROID_KEYSTORE_BASE64`、
`UPIX_RELEASE_STORE_PASSWORD`、`UPIX_RELEASE_KEY_ALIAS`、
`UPIX_RELEASE_KEY_PASSWORD`）。缺失时构建失败，除非显式
`-Pupix.allowDebugSignedRelease`——不要用 debug key 发 release（MIUI 等会因
签名不一致拒绝覆盖安装，见坑 7）。

keystore 文件在 `~/.config/upix/android-release.jks`，**不进 git**。

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
| `docs/DEVELOPMENT.md`（本文） | 流程/环境/坑清单/CI 分组变化 |
| `.github/workflows/` + `.github/actions/` | CI 分组、路径过滤、缓存键变化 |
| `docs/DESIGN_LANGUAGE.md` | 一切 UI token/组件规范变化（含 amendment 记录） |
| 根 `README.md` | 用户可见行为、CLI 面、构建说明变化 |
| `models/README.md` | 模型目录/发版流程变化 |
| `models/CONVERTING.md` | 模型转换/验收规程变化（新架构、新 pnnx/ncnn 坑） |

历史文档（serve daemon 设计、ADR、性能报告）已删除，需要时从 git 历史找回；
不要重新往 `docs/` 堆一次性报告。

---

## 9. CI

`ci-*` **默认关掉**（无 push/PR），全部可在 Actions 里 `workflow_dispatch`。
日常自动跑的只有发版：`cli-v*` / `android-v*` tag，同样也能手动触发。

**编译和测试是不同 job**：编译 job 只出 artifact，测试 job 只下载再跑。

平台分开，互不混编：

| 产品 | 平台 |
|------|------|
| CLI | `linux-x86_64`、`macos-arm64` |
| Android | `android-arm64-v8a` |

### 9.1 分组

| 工作流 | 编译 job | 测试 job | 自动触发 |
|--------|----------|----------|----------|
| `ci-static.yml` | — | UI token grep | 否（手动） |
| `ci-host.yml` | linux-x86_64 CLI + tests | 直跑 `test_*` + serve E2E（CPU） | 否（手动） |
| `ci-android-unit.yml` | Kotlin unit-test classes | `testDebugUnitTest` | 否（手动） |
| `ci-android-apk.yml` | android-arm64-v8a debug APK | `unzip -l` 含 `.so` | 否（手动） |
| `release-cli.yml` | linux-x86_64 + macos-arm64 | 各平台 `--version` | tag `cli-v*` |
| `release-android.yml` | android-arm64-v8a 签名 APK | `jarsigner -verify` | tag `android-v*` |

### 9.2 缓存

| 层 | 键 | 作用 |
|----|----|------|
| `~/.xmake/packages` | `xmake-{host\|android}-...-${{ hashFiles(xmake 工程) }}` | ncnn 等预编译包；host / android **分键**（plat 不同，混存会导致后写的组永远存不进去） |
| `share/core/spirv_pack.bin` | 源 shader / dump-spirv / ncnn overlay 的 hash | 命中则跳过 dump-spirv、glslang 变体 ncnn、lavapipe |
| `~/.ccache` | 按组 + ref | 工程源码增量；xmake 默认 `--ccache=y`（runner 装了 ccache） |
| Gradle | `setup-java` `cache: gradle` | 依赖与 wrapper |

`spirv_pack.bin` 仍 gitignore（`*.bin`）。CI 用 cache 恢复；未命中才走
`scripts/gen_spirv_pack.sh`（隔离 `XMAKE_CONFIGDIR`，需要 Vulkan ICD）。

GitHub-hosted runner 跳过 Aliyun Maven（`apps/settings.gradle.kts`：
`GITHUB_ACTIONS && !ACT`）。本地和 `act` 仍走阿里云。

### 9.3 本地用 act 按组打通

先校验 YAML，再按组跑。**不要**一上来 `act push` 全仓库——会拉大型
runner 镜像并冷编译两份 ncnn。

```bash
act -l                                          # 列出组
act -n --pull=false -W .github/workflows/ci-static.yml
act -n --pull=false -W .github/workflows/ci-host.yml
act -n --pull=false -W .github/workflows/ci-android-unit.yml
act -n --pull=false -W .github/workflows/ci-android-apk.yml   # 会 git clone upload-artifact
act -n --pull=false -W .github/workflows/release-cli.yml
act -n --pull=false -W .github/workflows/release-android.yml

# 真跑：从最便宜的组开始。不要 `act push` 一次全开。
act --pull=false -W .github/workflows/ci-static.yml
scripts/check_ui_tokens.sh                      # 等价，不用容器

# host / android 真跑很重（要 catthehacker 镜像）。等价的无容器路径：
# 编译
scripts/gen_spirv_pack.sh
scripts/build_cli.sh release
scripts/build_host_tests.sh
scripts/compile_android_unit.sh
# 测试（不再编译）
scripts/run_host_tests.sh
scripts/run_android_unit.sh
cd apps && ./gradlew assembleDebug              # apk 编译组
```

`ci-android-apk` 在 act 下还要 Android SDK + 钉死的 NDK，镜像体积最大，
放到最后打通。

### 9.4 为什么不把构建器换成 zig build

**现在不要换。** CI 翻车来自工作流设计（厨房水槽 job、单测绑 preBuild、
pack 每次现编、无路径过滤、`main`/`master` 不一致、Aliyun 对美国 runner），
不是 xmake 作为构建器的能力上限。

换成 zig build 解决不了这些问题，还会新开一摊：

- ncnn 是 CMake 工程（SIMD kernel、Vulkan、glslang），`packages/n/ncnn`
  里有一大块 overlay。Zig 要么继续包一层 CMake（变成 Zig+CMake+Gradle），
  要么重写 ncnn 的构建。
- C++23（CLI `<print>` 目前绑 GCC 14）要跟 zig 自带 Clang 版本对齐。
- glaze / cli11 / libavif+aom+dav1d / libwebp / libsodium / libcurl /
  openssl / libzip 已经在 xmake-repo。Zig package manager 要逐个包。
- Android 壳仍然是 Gradle + AGP + Compose，Zig 替不掉。
- xmake 的真实痛点（全局一份配置、option 粘连、钩子不累积）已经用脚本每次
  显式 `xmake f`、隔离 `XMAKE_CONFIGDIR`、CI 分 plat 缓存接住。

若以后只想要更好的交叉编译器，可以试验 `xmake f --cxx="zig c++"` 当
toolchain，而不是换构建系统。评估标准：能否在不碰 ncnn overlay 的前提下
让 `scripts/build_native.sh` 更短，而不是“先重写工程再修 CI”。
