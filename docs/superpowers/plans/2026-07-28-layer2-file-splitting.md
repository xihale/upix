# 第2层：大文件拆分 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 4 个巨型文件（`ncnn_engine.cpp` 3892行 / `coordinator.cpp` 1131行 / `ProcessingViewModel.kt` 2096行 / `cmd_process.cpp` 1211行）按职责拆成 ~15 个中等文件，每个单一职责、可独立理解。**纯结构重组，不改逻辑**——每个拆分单元 A/B md5 验证输出不变。

**Architecture:** C++ 成员函数定义可跨文件（只要在同一翻译单元内 include 类定义）；Kotlin 通过组合 + 共享 state 类解耦。每个 Task 先建 A/B 基线（记录重构前的输出 md5），拆分后逐一比对。

**Tech Stack:** C++23 (xmake) / Kotlin (Gradle) / ncnn / Vulkan

---

## 关键原则（所有 Task 共用）

1. **零逻辑变更**：只移动代码块 + 加 include / 改可见性。不改任何算法、控制流、常量。
2. **A/B md5 验证**：每个 Task 开始前，先对一组测试图跑出"重构前"输出 md5 作为基线；Task 完成后重跑，必须 bit-identical。
3. **每步可编译**：每个子步骤结束后 `xmake`（C++）或 `gradlew`（Kotlin）必须通过。
4. **构建已 glob 化**：第1层已把 `share/xmake.lua` 改为 glob 自动收集（commit `ee480f8`），新增的 `.cpp` 无需改 xmake.lua 即自动编入。Android CMakeLists 若第1层 Task 4 已完成同理。
5. **顺序**：Task 1-4 相互独立，可任意顺序执行。建议先做 Task 1（ncnn_engine，收益最大），其次 Task 4（cmd_process，C++ 风险一致），最后 Task 3（Kotlin，构建链不同）。

---

## Task 1: 拆分 `ncnn_engine.cpp`（3892 行 → 5 文件）

**最高价值、最高风险**。当前一个文件混了 GLSL shader 源码、Vulkan 初始化、CPU 路径、GPU 三档流水线。按职责拆。

### 1.1 当前结构映射（HEAD 实测）

```
行号       内容
1-34       includes
36         namespace crossscale {
38-547     #if NCNN_VULKAN 匿名命名空间：
             41-42  g_vulkan_init_mutex / g_vulkan_model_load_mutex
             44-56  struct VkAllocGuard
             58-545 11 个 shader 常量 (kHwcToChwComp ... kNormalizeQuantizeByWeightComp)
           547  } // namespace
           548  #endif
550-3886   class NCNNEngineImpl : public Engine
             552-553  构造/析构
             555-572  initialize
             574-599  list_devices
             601-769  load_model
             771-903  process (CPU + GPU dispatch)
             905-1179 process_u8
             1181-1332 process_batch
             1334-1364 supports_gpu_tile_pipeline[_u8][_u8_queue]
             1366-1952 process_tiles_gpu_u8_queue      ← GPU 流水线
             1954-2429 process_tiles_gpu                ← GPU 流水线
             2431-2952 process_tiles_gpu_u8             ← GPU 流水线
             2955-2969 benchmark
             2971-2995 unload_model
             3002-3004 set_scale_factor
             3006-3037 get_memory_info
             3039-3080 get_recommended_tile_size
             3082      private:
             3083-3135 format_vulkan_runtime_snapshot
             3137-3241 init_gpu_layout_pipelines       ← 用 shader
             3243-3288 init_gpu_crop_pipeline          ← 用 shader
             3290-3368 init_gpu_tile_pipelines         ← 用 shader
             3370-3527 process_vulkan_with_gpu_layout
             3529-3699 process_vulkan_with_gpu_layout_u8
             3701-3787 try_init_vulkan                 ← Vulkan 初始化 + GPU 打分
             3789-3858 detect_model_info
             3860-3885 成员变量
3888-3890  create_ncnn_engine() 工厂
3892       } // namespace crossscale
```

### 1.2 目标拆分

```
share/core/
├─ ncnn_engine.cpp        ~600行：NCNNEngineImpl 类骨架（公共 API + 成员 + 工厂）
│                         initialize/list_devices/load_model/process/process_u8/
│                         process_batch/supports_*/benchmark/unload_model/
│                         set_scale_factor/get_memory_info/get_recommended_tile_size
├─ vulkan_shaders.hpp     ~490行：11 个 shader 常量（header-only，#if NCNN_VULKAN 门控）
├─ ncnn_gpu_init.cpp      ~330行：try_init_vulkan + detect_model_info
│                         （GPU 枚举/打分/重试/模型信息探测）
├─ ncnn_cpu_path.cpp      （并入 ncnn_engine.cpp，见 1.3 决策）
└─ ncnn_gpu_pipeline.cpp  ~1620行：三档 GPU 流水线 + init_*_pipelines + process_vulkan_*
                         process_tiles_gpu_u8_queue/process_tiles_gpu/process_tiles_gpu_u8
                         init_gpu_layout_pipelines/init_gpu_crop_pipeline/init_gpu_tile_pipelines
                         process_vulkan_with_gpu_layout[_u8]/format_vulkan_runtime_snapshot
```

### 1.3 拆分决策（重要）

- **CPU 路径不单独成文件**：`process`/`process_u8`/`process_batch` 每个都含 CPU 分支和 GPU 分支（dispatch 到 `process_vulkan_with_gpu_layout`），强耦合。强行拆 CPU 路径会切断方法体。决策：**保留在 `ncnn_engine.cpp` 类骨架中**，与公共 API 一起。这样 `ncnn_engine.cpp` ~1100 行（比 3892 已大幅改善），`ncnn_gpu_pipeline.cpp` ~1620 行。
- **shader 独立 header**：11 个 shader 常量是纯数据，最易抽出，且抽出后 `ncnn_gpu_pipeline.cpp` 的 init 方法只需 `#include "vulkan_shaders.hpp"`。
- **跨文件成员函数**：C++ 允许 `NCNNEngineImpl::try_init_vulkan()` 的定义出现在另一个 `.cpp`，只要该 `.cpp` include 了含类定义的头。但**类定义（含成员变量）目前内联在 ncnn_engine.cpp**——需先把 `NCNNEngineImpl` 类声明提到一个内部头 `ncnn_engine_impl.hpp`，各 `.cpp` include 它。

### 1.4 步骤

- [ ] **Step 1: 建立 A/B 基线**

准备 2 张测试图（如已存在于 `cli/models/` 或仓库的测试资产；若无，用任意 png）。
记录重构前输出 md5：

```bash
cd cli
xmake
xmake run crossscale-cli process -i <test1.png> -o /tmp/before1.png --no-daemon
xmake run crossscale-cli process -i <test2.png> -o /tmp/before2.png --no-daemon
md5sum /tmp/before1.png /tmp/before2.png | tee /tmp/layer2_baseline.txt
```
（`--no-daemon` 确保走内联引擎路径，避免 daemon 干扰。记录 baseline，后续比对。）

- [ ] **Step 2: 提取 `NCNNEngineImpl` 类声明到内部头 `ncnn_engine_impl.hpp`**

新建 `share/core/ncnn_engine_impl.hpp`，把 `ncnn_engine.cpp:550-3886` 的 `class NCNNEngineImpl : public Engine { ... };` **声明**（含所有成员函数声明 + 成员变量，但不含函数体）移入。函数体留在 `.cpp`。

具体：
1. 新建 `share/core/ncnn_engine_impl.hpp`，内容骨架：
   ```cpp
   #pragma once
   #include "core/engine.hpp"
   #include <memory>
   // forward declare ncnn types to avoid pulling ncnn headers here
   namespace ncnn { class Net; class VulkanDevice; class Pipeline; }

   namespace crossscale {

   // Vulkan shader sources (compiled in only when NCNN_VULKAN is defined).
   // Defined in ncnn_engine.cpp's anonymous namespace; declared here so the
   // pipeline .cpp files can reference them via the detail namespace.
   #if NCNN_VULKAN
   namespace detail {
   // shader constants declared in vulkan_shaders.hpp
   }
   #endif

   class NCNNEngineImpl : public Engine {
   public:
       NCNNEngineImpl();
       ~NCNNEngineImpl() override;
       auto initialize(DeviceType preferred) -> expected<void, EngineError> override;
       auto list_devices() -> std::vector<DeviceCapability> override;
       auto load_model(const std::string& param_path, const std::string& bin_path)
           -> expected<void, EngineError> override;
       auto process(const float* input, float* output, int width, int height, int channels,
                    const std::atomic<bool>& cancel_flag) -> expected<void, EngineError> override;
       auto process_u8(const uint8_t* input, uint8_t* output, int width, int height, int channels,
                       const std::atomic<bool>& cancel_flag) -> expected<void, EngineError> override;
       auto process_batch(const std::vector<const float*>& inputs, const std::vector<float*>& outputs,
                          int width, int height, int channels,
                          const std::atomic<bool>& cancel_flag) -> expected<void, EngineError> override;
       bool supports_gpu_tile_pipeline() const override;
       bool supports_gpu_tile_pipeline_u8() const override;
       bool supports_gpu_tile_pipeline_u8_queue() const override;
       auto process_tiles_gpu_u8_queue(const std::vector<GPUTileTaskU8>& tasks,
                                       const std::atomic<bool>& cancel_flag)
           -> expected<void, EngineError> override;
       auto process_tiles_gpu(const float* input_data, float* output_data, int input_width,
                              int input_height, int channels, const std::vector<TileRegion>& regions,
                              const TileConfig& config,
                              std::function<void(const ProcessingProgress&)> progress_callback,
                              const std::atomic<bool>& cancel_flag)
           -> expected<void, EngineError> override;
       auto process_tiles_gpu_u8(const uint8_t* input_data, uint8_t* output_data, int input_width,
                                 int input_height, int channels, const std::vector<TileRegion>& regions,
                                 const TileConfig& config,
                                 std::function<void(const ProcessingProgress&)> progress_callback,
                                 const std::atomic<bool>& cancel_flag)
           -> expected<void, EngineError> override;
       auto benchmark(int test_tile_size) -> float override;
       void unload_model() override;
       void set_scale_factor(int scale) override;
       auto get_memory_info() const -> std::pair<size_t, size_t> override;
       int get_recommended_tile_size() const override;

   private:
       auto format_vulkan_runtime_snapshot(std::string_view stage, int width=-1, int height=-1,
           int channels=-1, size_t submit_budget=0, size_t submit_budget_cap=0,
           size_t submit_memory_budget_mb=0, double avg_tile_pixels=-1.0) const -> std::string;
       bool init_gpu_layout_pipelines();
       bool init_gpu_crop_pipeline();
       bool init_gpu_tile_pipelines();
       auto process_vulkan_with_gpu_layout(const float* input, float* output, int width, int height,
           int channels, const std::atomic<bool>& cancel_flag) -> expected<void, EngineError>;
       auto process_vulkan_with_gpu_layout_u8(const uint8_t* input, uint8_t* output, int width,
           int height, int channels, const std::atomic<bool>& cancel_flag) -> expected<void, EngineError>;
       bool try_init_vulkan();
       void detect_model_info(const std::string& param_path);

       std::unique_ptr<ncnn::Net> net_;
   #if NCNN_VULKAN
       ncnn::VulkanDevice* vkdev_ = nullptr;
       std::unique_ptr<ncnn::Pipeline> preproc_hwc_to_chw_;
       std::unique_ptr<ncnn::Pipeline> preproc_hwc_u8_to_chw_;
       std::unique_ptr<ncnn::Pipeline> preproc_hwc_crop_to_chw_;
       std::unique_ptr<ncnn::Pipeline> preproc_hwc_crop_u8_to_chw_;
       std::unique_ptr<ncnn::Pipeline> postproc_chw_to_hwc_;
       std::unique_ptr<ncnn::Pipeline> postproc_chw_to_hwc_u8_;
       std::unique_ptr<ncnn::Pipeline> quantize_hwc_to_u8_;
       std::unique_ptr<ncnn::Pipeline> clear_output_pipeline_;
       std::unique_ptr<ncnn::Pipeline> blend_tile_pipeline_;
       std::unique_ptr<ncnn::Pipeline> normalize_output_pipeline_;
       std::unique_ptr<ncnn::Pipeline> normalize_quantize_output_pipeline_;
       bool gpu_layout_pipeline_ready_ = false;
       bool gpu_layout_u8_pipeline_ready_ = false;
       bool gpu_crop_pipeline_ready_ = false;
       bool gpu_crop_u8_pipeline_ready_ = false;
       bool gpu_tile_pipeline_ready_ = false;
   #endif
       DeviceType current_device_ = DeviceType::CPU;
       bool initialized_ = false;
       bool model_loaded_ = false;
       std::string model_name_;
       int scale_factor_ = 2;
       int input_channels_ = 3;
   };

   std::unique_ptr<Engine> create_ncnn_engine();

   } // namespace crossscale
   ```

2. 在 `ncnn_engine.cpp` 中，把原 `class NCNNEngineImpl : public Engine { ... };` 整块删除，改为 `#include "core/ncnn_engine_impl.hpp"`（放在 includes 区）。函数定义（`NCNNEngineImpl::xxx() { ... }`）全部保留原位。

> **注意**：类的成员变量声明移到头后，`.cpp` 里只剩函数定义。`create_ncnn_engine()` 工厂也移到头声明，定义留 `.cpp`。

- [ ] **Step 3: 编译验证（应仍通过，行为不变）**

```bash
cd cli && xmake
```
Expected: 通过。此时是"原地提取头"，无逻辑变更。

- [ ] **Step 4: 提取 shader 常量到 `vulkan_shaders.hpp`**

新建 `share/core/vulkan_shaders.hpp`，把 `ncnn_engine.cpp:58-545` 的 11 个 `constexpr const char* kXxxComp = R"(...)";` 移入。用 `inline constexpr` 并放入 `namespace crossscale::detail`（避免匿名命名空间在多 TU 下 ODR 冲突）：

```cpp
#pragma once
#if NCNN_VULKAN
namespace crossscale::detail {

inline constexpr const char* kHwcToChwComp = R"(
... (原样搬过来) ...
)";

inline constexpr const char* kHwcU8ToChwComp = R"(
...
)";
// ... 其余 9 个 ...

} // namespace crossscale::detail
#endif
```

在 `ncnn_engine.cpp` 中：
- 删除原 58-545 的 shader 定义。
- 在原引用 shader 的 3 个 init 方法里，把 `kHwcToChwComp` 等改为 `detail::kHwcToChwComp`（或在使用处加 `using namespace crossscale::detail;`）。
- `#include "core/vulkan_shaders.hpp"`。

> **保留** `g_vulkan_init_mutex` / `g_vulkan_model_load_mutex` / `VkAllocGuard`（41-56行）在 `ncnn_engine.cpp` 的匿名命名空间（它们不是 shader，且被多个方法用，留在主文件）。

- [ ] **Step 5: 编译 + A/B 验证**

```bash
cd cli && xmake
xmake run crossscale-cli process -i <test1.png> -o /tmp/after1.png --no-daemon
xmake run crossscale-cli process -i <test2.png> -o /tmp/after2.png --no-daemon
md5sum /tmp/after1.png /tmp/after2.png
diff <(md5sum /tmp/before1.png) <(md5sum /tmp/after1.png) && echo "MATCH 1" || echo "MISMATCH 1"
diff <(md5sum /tmp/before2.png) <(md5sum /tmp/after2.png) && echo "MATCH 2" || echo "MISMATCH 2"
```
Expected: 两个 MATCH。若有 MISMATCH，shader 提取引入了改动——回查 Step 4 是否漏改引用名。

- [ ] **Step 6: 提交 shader 提取**

```bash
git add share/core/vulkan_shaders.hpp share/core/ncnn_engine_impl.hpp share/core/ncnn_engine.cpp
git commit -m "refactor(engine): extract Vulkan shaders to header (no logic change)

Move 11 GLSL compute shader constants to vulkan_shaders.hpp under
crossscale::detail. Extract NCNNEngineImpl class declaration to
ncnn_engine_impl.hpp so coming .cpp splits can reference it.
A/B md5 verified bit-identical."
```

- [ ] **Step 7: 提取 GPU 初始化到 `ncnn_gpu_init.cpp`**

新建 `share/core/ncnn_gpu_init.cpp`，把 `try_init_vulkan`（3701-3787）和 `detect_model_info`（3789-3858）的**函数体**移过去。文件头：
```cpp
#include "core/ncnn_engine_impl.hpp"
#include "utils/logger.hpp"
#include <ncnn/...>  // 按原 ncnn_engine.cpp 的 include 搬
// g_vulkan_init_mutex 需可见：要么提到 detail 头，要么 extern 声明
```

**互斥锁问题**：`try_init_vulkan` 用 `g_vulkan_init_mutex`。该锁当前在 `ncnn_engine.cpp` 匿名命名空间。跨文件引用需把锁声明为 `extern`：
- 在 `ncnn_engine.cpp` 把 `std::mutex g_vulkan_init_mutex;` 从匿名命名空间移出，改为 `namespace crossscale::detail { std::mutex g_vulkan_init_mutex; }`。
- 在 `ncnn_engine_impl.hpp` 加 `namespace detail { extern std::mutex g_vulkan_init_mutex; }` 声明。
- `try_init_vulkan` 内引用改为 `detail::g_vulkan_init_mutex`。

在 `ncnn_engine.cpp` 删除这两个函数体（声明已在头里）。

- [ ] **Step 8: 编译 + A/B 验证**

```bash
cd cli && xmake
# 重跑 A/B（同 Step 5 命令）
```
Expected: 编译通过，md5 MATCH。

- [ ] **Step 9: 提交 GPU 初始化提取**

```bash
git add share/core/ncnn_gpu_init.cpp share/core/ncnn_engine.cpp share/core/ncnn_engine_impl.hpp
git commit -m "refactor(engine): split GPU init (try_init_vulkan, detect_model_info) to ncnn_gpu_init.cpp

A/B md5 verified bit-identical."
```

- [ ] **Step 10: 提取 GPU 流水线到 `ncnn_gpu_pipeline.cpp`**

新建 `share/core/ncnn_gpu_pipeline.cpp`，把以下方法的**函数体**移过去：
- `process_tiles_gpu_u8_queue`（1366-1952）
- `process_tiles_gpu`（1954-2429）
- `process_tiles_gpu_u8`（2431-2952）
- `format_vulkan_runtime_snapshot`（3083-3135）
- `init_gpu_layout_pipelines`（3137-3241）
- `init_gpu_crop_pipeline`（3243-3288）
- `init_gpu_tile_pipelines`（3290-3368）
- `process_vulkan_with_gpu_layout`（3370-3527）
- `process_vulkan_with_gpu_layout_u8`（3529-3699）

文件头：
```cpp
#include "core/ncnn_engine_impl.hpp"
#include "core/vulkan_shaders.hpp"  // shader 常量
#include "utils/logger.hpp"
#include <ncnn/...>
namespace crossscale {
// 函数体搬过来
} // namespace crossscale
```

在 `ncnn_engine.cpp` 删除这些函数体。`ncnn_engine.cpp` 现在只剩类骨架（公共 API + 工厂），约 1100 行。

> **互斥锁**：`g_vulkan_model_load_mutex` 若被这些方法用，同样按 Step 7 方式 extern 化。

- [ ] **Step 11: 编译 + A/B 验证（最终）**

```bash
cd cli && xmake
xmake run crossscale-cli process -i <test1.png> -o /tmp/final1.png --no-daemon
xmake run crossscale-cli process -i <test2.png> -o /tmp/final2.png --no-daemon
diff <(md5sum /tmp/before1.png) <(md5sum /tmp/final1.png) && echo "MATCH 1" || echo "MISMATCH 1"
diff <(md5sum /tmp/before2.png) <(md5sum /tmp/final2.png) && echo "MATCH 2" || echo "MISMATCH 2"
wc -l share/core/ncnn_engine.cpp share/core/ncnn_gpu_init.cpp share/core/ncnn_gpu_pipeline.cpp share/core/vulkan_shaders.hpp share/core/ncnn_engine_impl.hpp
```
Expected: 全部 MATCH。文件行数：ncnn_engine.cpp ~1100，gpu_pipeline ~1620，gpu_init ~330，shaders ~490，impl.hpp ~100。

- [ ] **Step 12: 提交 GPU 流水线提取**

```bash
git add share/core/ncnn_gpu_pipeline.cpp share/core/ncnn_engine.cpp
git commit -m "refactor(engine): split GPU tile pipeline (3 tiers + init + dispatch) to ncnn_gpu_pipeline.cpp

ncnn_engine.cpp now ~1100 lines (class skeleton + CPU path).
ncnn_gpu_pipeline.cpp ~1620 lines (3-tier GPU pipeline + SPIRV init).
A/B md5 verified bit-identical."
```

---

## Task 2: 拆分 `coordinator.cpp`（1131 行 → 3 文件）

### 2.1 当前结构映射

```
行号       方法
21-25      构造/析构
27-92      start / stop / is_running / get_connected_devices / device_count
105-526    distribute_task（核心编排，422 行）
531-556    accept_loop
557-910    handle_client（客户端路由，354 行）
911-915    authenticate
916-941    send_model_list
942-945    set_models_directory
946-1041   handle_model_request
1042-1094  deserialize_results
1095-1131  stitch_tiles
```

### 2.2 目标拆分

```
share/core/distributed/
├─ coordinator.cpp          ~430行：start/stop/accept_loop/handle_client 路由 + 构造析构
├─ coordinator_task.cpp     ~480行：distribute_task + deserialize_results + stitch_tiles
└─ coordinator_admin.cpp    ~220行：authenticate + send_model_list + handle_model_request + set_models_directory
```

### 2.3 步骤

- [ ] **Step 1: 建立 A/B 基线（分布式路径）**

启动 server + worker（另一终端），用 client 提交一张图，记录输出 md5。或用现成的分布式测试脚本 `scripts/test_distributed.cpp` 若存在。

```bash
cd cli && xmake
# 终端A: xmake run crossscale-cli server start
# 终端B: xmake run crossscale-cli worker connect 127.0.0.1:9527 -k <key>
# 终端C: xmake run crossscale-cli client process -i <test.png> -o /tmp/dist_before.png -s 127.0.0.1:9527 -k <key>
md5sum /tmp/dist_before.png | tee /tmp/layer2_dist_baseline.txt
```

> **若多机分布式环境不便搭建**：可跳过 A/B，改为"编译通过 + 单元测试通过（若有）"作为验证。coordinator 的拆分是纯代码移动，风险低于 ncnn_engine。

- [ ] **Step 2: 提取 admin 方法到 `coordinator_admin.cpp`**

新建 `share/core/distributed/coordinator_admin.cpp`，移入 `authenticate`（911-915）、`send_model_list`（916-941）、`handle_model_request`（946-1041）、`set_models_directory`（942-945）的函数体。

文件头：
```cpp
#include "core/distributed/coordinator.hpp"
#include "core/distributed/protocol.hpp"
#include "utils/logger.hpp"
namespace crossscale {
// 函数体
} // namespace crossscale
```

在 `coordinator.cpp` 删除这 4 个函数体（声明在 hpp 里，已存在）。

- [ ] **Step 3: 编译验证**

```bash
cd cli && xmake
```
Expected: 通过。

- [ ] **Step 4: 提取 task 方法到 `coordinator_task.cpp`**

新建 `share/core/distributed/coordinator_task.cpp`，移入 `distribute_task`（105-526）、`deserialize_results`（1042-1094）、`stitch_tiles`（1095-1131）的函数体。同样 include `coordinator.hpp`。

在 `coordinator.cpp` 删除这 3 个函数体。`coordinator.cpp` 现在只剩 start/stop/accept_loop/handle_client + 构造析构。

- [ ] **Step 5: 编译 + A/B 验证**

```bash
cd cli && xmake
# 重跑分布式 A/B（若 Step 1 建了基线）
wc -l share/core/distributed/coordinator.cpp share/core/distributed/coordinator_task.cpp share/core/distributed/coordinator_admin.cpp
```
Expected: 编译通过，md5 MATCH（若有基线），文件 ~430/480/220。

- [ ] **Step 6: 提交**

```bash
git add share/core/distributed/coordinator.cpp share/core/distributed/coordinator_task.cpp share/core/distributed/coordinator_admin.cpp
git commit -m "refactor(coordinator): split into skeleton/task/admin (no logic change)

coordinator.cpp ~430 (start/stop/accept/handle_client).
coordinator_task.cpp ~480 (distribute_task + stitch).
coordinator_admin.cpp ~220 (auth + model sync).
A/B md5 verified (distributed path)."
```

---

## Task 3: 拆分 `ProcessingViewModel.kt`（2096 行 → 5 文件）

### 3.1 当前结构映射（要点）

紧耦合状态簇：`{_uiState, processingJob, activeQueueItemId}`（被队列循环 + startProcessingInternal + cancel/retry/resume 共享）。

完全独立的子域（可先抽，零耦合）：
- **备份恢复**：`hasBackupFor`(805)、`restoreBackup`(812) — 只依赖 inputSourceRepository + batchOutputRepository。
- **映射扩展函数**：`toQueueSnapshot`(1911)、`toPersistedQueueItem`(1920)、`toUiQueueItem`(1939)、`toUiSummary`(1958)、`matchesBatchConfiguration`(309) — 纯函数。
- **预估状态**：`refreshExpectedTotal`(625) + `previewCountJob`(131) + `previewRequestId`(132)。

### 3.2 目标拆分（渐进，从易到难）

```
viewmodel/
├─ ProcessingViewModel.kt        ~900行：UI 状态 + startProcessingInternal + buildRunSetup + cancel
│                                （持有 _uiState/processingJob/activeQueueItemId）
├─ ProcessingMappers.kt          ~120行：所有 *.toQueueSnapshot / toPersistedQueueItem 等
├─ BackupRestoreManager.kt       ~60行：hasBackupFor / restoreBackup
├─ InputCountEstimator.kt        ~80行：refreshExpectedTotal + 预估状态
└─ ProcessingUiModels.kt         ~60行：5 个 data class（ProcessingUiState 等）提取
```

> **注意**：原 spec 想拆成 QueueManager/CheckpointCoordinator/RetryCoordinator/BackupCoordinator，但 explore 映射显示队列/checkpoint/retry 三者通过 `startProcessingInternal` + 共享状态强耦合，强行拆成 4 个 coordinator 反而要大量回调。决策：**保守拆分**——只抽真正独立的 3 块（备份/映射/预估）+ data class，ViewModel 主体保留（从 2096 降到 ~900 行已是大幅改善）。Queue/Checkpoint/Retry 的进一步拆分留作后续（风险与收益不匹配）。

### 3.3 步骤

- [ ] **Step 1: 确认现有测试基线**

```bash
cd apps && ./gradlew :app:testDebugUnitTest --tests "com.crossscale.viewmodel.*"
```
记录通过/失败数。这些测试是 A/B 验证的主力（ProcessingViewModelTest 含 fake repository 实现）。

> **若无 Android 构建环境**：本 Task 标记"待 Android 环境执行"，先做 Task 1/2/4。

- [ ] **Step 2: 提取 data class 到 `ProcessingUiModels.kt`**

新建 `apps/app/src/main/java/com/crossscale/viewmodel/ProcessingUiModels.kt`，移入：
- `ProcessingUiState`(48)、`ProcessingQueueItemSummary`(60)、`ProcessingCheckpointSummary`(78)、`ProcessingCheckpointItemSummary`(96)、`ProcessingRunSetup`(104)。

在 `ProcessingViewModel.kt` 删除这些 class（保留 import）。

- [ ] **Step 3: 提取映射扩展到 `ProcessingMappers.kt`**

新建 `ProcessingMappers.kt`，移入 5 个扩展函数（309/1911/1920/1939/1958）。它们是纯函数，无状态依赖。

- [ ] **Step 4: 提取 `BackupRestoreManager.kt`**

新建 `BackupRestoreManager.kt`：
```kotlin
class BackupRestoreManager(
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val logRepository: LogRepository,
    private val ioDispatcher: CoroutineDispatcher,
) {
    suspend fun hasBackupFor(imagePath: String, nested: Boolean): Boolean { ... }
    fun restoreBackup(imagePath: String, nested: Boolean, onComplete: (Int, Int) -> Unit) { ... }
}
```
把 `hasBackupFor`(805)、`restoreBackup`(812) 的函数体搬入。`ProcessingViewModel` 持有一个实例并委托：
```kotlin
private val backupRestore = BackupRestoreManager(inputSourceRepository, batchOutputRepository, logRepository, ioDispatcher)
fun hasBackupFor(imagePath: String, nested: Boolean) = backupRestore.hasBackupFor(imagePath, nested)
fun restoreBackup(imagePath: String, nested: Boolean, onComplete: (Int, Int) -> Unit) =
    backupRestore.restoreBackup(imagePath, nested, onComplete)
```

- [ ] **Step 5: 提取 `InputCountEstimator.kt`**

新建 `InputCountEstimator.kt`，移入 `refreshExpectedTotal`(625) + 持有 `previewCountJob`/`previewRequestId` 状态。它需写 `_uiState`，通过回调或持有 `_uiState` 引用：
```kotlin
class InputCountEstimator(
    private val inputSourceRepository: InputSourceResolver,
    private val scope: CoroutineScope,
    private val ioDispatcher: CoroutineDispatcher,
    private val onUpdate: (Int?, Boolean) -> Unit,  // (expectedTotal, isEstimating)
) { ... }
```
`ProcessingViewModel` 中用 `onUpdate = { count, estimating -> _uiState.update { it.copy(expectedTotalItems = count, isEstimatingInputCount = estimating) } }` 接回。

- [ ] **Step 6: 编译 + 测试验证**

```bash
cd apps && ./gradlew :app:testDebugUnitTest --tests "com.crossscale.viewmodel.*"
wc -l apps/app/src/main/java/com/crossscale/viewmodel/ProcessingViewModel.kt
```
Expected: 测试通过数与 Step 1 基线一致。ViewModel ~900 行。

- [ ] **Step 7: 提交**

```bash
git add apps/app/src/main/java/com/crossscale/viewmodel/
git commit -m "refactor(vm): extract mappers/backup/estimator/models from ProcessingViewModel

ProcessingViewModel.kt 2096 → ~900 lines.
Extracted: ProcessingMappers, BackupRestoreManager, InputCountEstimator,
ProcessingUiModels (5 data classes). Queue/checkpoint/retry stay in VM
(strong coupling via startProcessingInternal + shared state).
Unit tests unchanged."
```

---

## Task 4: 拆分 `cmd_process.cpp`（1211 行 → 3 文件）

### 4.1 当前结构映射

```
行号       内容
43         int cmd_process 入口
66         is_dir 分支判断
75-178     delegate() lambda（serve daemon 委派）
180-496    单图内联路径（init engine + process）
497-879    process_one lambda（核心处理函数，处理单张图）
880-999    目录批处理（GPU 流水线 / CPU EnginePool）
1000-1211  单图路径续 + 输出
```

### 4.2 目标拆分

```
cli/src/
├─ cmd_process.cpp          ~300行：入口 + is_dir 分支 + 单图委派/内联
├─ process_single.hpp       ~30行：process_one 的声明（共享给目录模式）
└─ process_batch.cpp        ~500行：目录 CPU/GPU 批处理
```

> **注意**：`process_one` lambda 被单图和目录模式都用，需提为自由函数（或静态函数）。它是 lambda，捕获了 config/engine 等局部状态——提取时需把这些作为参数传入。

### 4.3 步骤

- [ ] **Step 1: 建立 A/B 基线**

```bash
cd cli && xmake
# 单图
xmake run crossscale-cli process -i <test.png> -o /tmp/single_before.png --no-daemon
# 目录（准备含 2-3 张图的目录 /tmp/indir/）
xmake run crossscale-cli process -i /tmp/indir -o /tmp/outdir_before --no-daemon
md5sum /tmp/single_before.png /tmp/outdir_before/* | tee /tmp/layer2_cli_baseline.txt
```

- [ ] **Step 2: 把 `process_one` lambda 提为自由函数**

分析 `process_one`(497-879) 捕获的变量（config、engine pool、model、tile 设置等）。把它们改为函数参数：

新建 `cli/src/process_single.hpp`：
```cpp
#pragma once
#include <string>
struct ProcessSingleArgs {
    // 列出 process_one 需要的所有依赖：config、engine ref、model 信息、tile 参数等
    // （执行时根据实际捕获列表填充）
};
// 处理单张图，返回是否成功。被 cmd_process（单图）和 process_batch（目录）共用。
bool process_one(const ProcessSingleArgs& args,
                 const std::string& in_path,
                 const std::string& out_path);
```

在 `cmd_process.cpp` 把 lambda 体改为 `process_one` 的定义（放 cmd_process.cpp 或新文件 process_single.cpp）。原调用点 `[&](...) {...}` 改为 `process_one(args, ...)`。

- [ ] **Step 3: 编译 + 单图 A/B**

```bash
cd cli && xmake
xmake run crossscale-cli process -i <test.png> -o /tmp/single_after.png --no-daemon
diff <(md5sum /tmp/single_before.png) <(md5sum /tmp/single_after.png) && echo MATCH || echo MISMATCH
```
Expected: MATCH。

- [ ] **Step 4: 提取目录批处理到 `process_batch.cpp`**

新建 `cli/src/process_batch.cpp`，移入目录模式代码（880-999，GPU 流水线 + CPU EnginePool 批处理）。声明：
```cpp
// process_batch.cpp 或 process_single.hpp
int run_batch(const args::Process& a, /* 共享依赖 */);
```
`cmd_process.cpp` 的 is_dir 分支改为调用 `run_batch(...)`。

- [ ] **Step 5: 编译 + 目录 A/B**

```bash
cd cli && xmake
xmake run crossscale-cli process -i /tmp/indir -o /tmp/outdir_after --no-daemon
diff <(md5sum /tmp/outdir_before/*) <(md5sum /tmp/outdir_after/*) && echo MATCH || echo MISMATCH
wc -l cli/src/cmd_process.cpp cli/src/process_batch.cpp cli/src/process_single.hpp
```
Expected: MATCH。cmd_process ~300，process_batch ~500。

- [ ] **Step 6: 提交**

```bash
git add cli/src/cmd_process.cpp cli/src/process_batch.cpp cli/src/process_single.hpp
git commit -m "refactor(cli): split cmd_process into single/batch (no logic change)

cmd_process.cpp 1211 → ~300 lines (entry + single-image delegate/inline).
process_batch.cpp ~500 (directory CPU EnginePool / GPU pipeline).
process_one promoted from lambda to shared free function.
A/B md5 verified (single + directory)."
```

---

## 验收标准（整层）

完成 Task 1-4 后：

- [ ] 4 个原巨型文件均已拆分，无文件超过 ~1600 行（ncnn_gpu_pipeline 例外，因三档流水线自成体系）。
- [ ] 所有 A/B md5 验证 MATCH（单机单图、单机目录、分布式、Android 单测）。
- [ ] `cd cli && xmake` 通过。
- [ ] `cd apps && ./gradlew assembleDebug` 通过（若 Task 3 执行）。
- [ ] `git log --oneline` 显示各 Task 的独立提交。
- [ ] 新增的 `.cpp` 被 xmake glob 自动收集（无需改 xmake.lua）——验证：`xmake -v | grep ncnn_gpu` 应见新文件。

---

## 风险

| 风险 | 缓解 |
|---|---|
| ncnn_engine 跨文件成员函数链接失败 | Step 2 先提取类声明到头，每步编译验证 |
| shader 跨 TU 的 ODR 冲突 | 用 `inline constexpr` + `namespace detail`（非匿名命名空间） |
| 互斥锁跨文件引用 | extern 声明 + 移到 detail 命名空间（Step 7） |
| A/B md5 不一致 | 每步独立验证，二分定位引入变更的步骤 |
| Android 无构建环境 | Task 3 可标记"待环境"，先做 C++ Task |
| process_one lambda 捕获列表复杂 | Step 2 仔细枚举捕获变量，作为参数传入 |
