# CrossScale - 分布式图像超分辨率系统 规划书

## 1. 项目概述

### 1.1 核心理念
CrossScale 是一个**跨平台分布式图像超分辨率系统**，核心理念是 "Cross"：
- **跨平台**：PC CLI (C++) + Android (Kotlin + NDK)
- **跨设备协作**：局域网内多设备组队超分（拼好卡）
- **跨资源共享**：CPU、GPU、多显卡统一调度

### 1.2 关键特性
1. **通用模型支持**：支持任意 ncnn 训练的超分模型，用户可自定义
2. **智能分布式计算**：基于性能权重的动态任务分配，追求 99%+ 效率
3. **灵活连接方式**：二维码扫描、手动 IP+Port+Key、mDNS 自动发现
4. **自动模型同步**：组机时自动传输缺失模型（公共子集）

### 1.3 技术栈
| 层级 | 技术选型 |
|------|----------|
| 推理引擎 | ncnn (Vulkan GPU 加速) |
| 图像处理 | libwebp, stb_image |
| 构建系统 | xmake |
| 通信协议 | 自定义二进制协议 (TCP) |
| Android UI | Jetpack Compose + Material Design 3 |
| 网络发现 | mDNS/DNS-SD + QR Code |

---

## 2. 系统架构

### 2.1 项目结构
```
crossscale/
├── apps/                       # 多平台应用工作区（Android + Desktop）
│   ├── app/
│   │   ├── src/main/java/     # Kotlin UI 层
│   │   │   └── com/crossscale/
│   │   │       ├── ui/        # Compose UI 组件
│   │   │       ├── viewmodel/ # ViewModel 层
│   │   │       ├── network/   # 网络客户端
│   │   │       └── service/   # 后台服务
│   │   └── src/main/cpp/      # JNI 桥接层
│   │       ├── jni_bridge.cpp # JNI 接口
│   │       └── android_log.h  # Android 日志适配
│   └── build.gradle.kts
│
├── cli/                        # 命令行版本
│   ├── src/
│   │   ├── main.cpp           # 入口
│   │   ├── commands/          # CLI 命令处理
│   │   └── server/            # 分布式服务器
│   └── xmake.lua
│
├── share/                      # 共享 C++ 核心代码
│   ├── core/
│   │   ├── engine.hpp/cpp     # 超分引擎抽象基类
│   │   ├── ncnn_engine.hpp/cpp # ncnn 引擎实现
│   │   ├── tile_processor.hpp/cpp # 分块处理
│   │   ├── model_manager.hpp/cpp  # 模型管理器
│   │   └── distributed/
│   │       ├── scheduler.hpp/cpp   # 任务调度器
│   │       ├── worker.hpp/cpp      # 工作节点
│   │       ├── coordinator.hpp/cpp # 协调器
│   │       └── protocol.hpp/cpp    # 通信协议
│   ├── utils/
│   │   ├── image.hpp/cpp      # 图像工具
│   │   ├── logger.hpp/cpp     # 日志系统
│   │   ├── network.hpp/cpp    # 网络通信
│   │   ├── benchmark.hpp/cpp  # 性能基准测试
│   │   └── crypto.hpp/cpp     # 加密/Key 生成
│   └── xmake.lua
│
├── models/                     # 预置模型目录（示例）
│   └── README.md              # 模型放置说明
│
├── docs/                       # 文档
│   ├── protocol.md            # 通信协议规范
│   └── model_format.md        # 模型格式说明
│
├── apps/
│   ├── app/                    # Android 应用
│   ├── shared-ui/              # KMP 共享 Compose UI
│   │   └── src/commonMain/     # Android / Desktop 共享主题与页面
│   └── desktopApp/             # Compose Multiplatform 桌面端宿主
│       └── src/desktopMain/    # 桌面状态、文件对话框、CLI 桥接
│
├── AGENTS.md                   # 开发指南（已存在）
├── PLAN.md                     # 本规划书
└── xmake.lua                   # 根 xmake 配置
```

### 2.2 分层架构图
```
┌─────────────────────────────────────────────────────────────────┐
│                         Presentation Layer                       │
│  ┌─────────────────────┐        ┌─────────────────────────────┐ │
│  │   Android App       │        │        CLI                  │ │
│  │   (Kotlin/Compose)  │        │        (C++)                │ │
│  └──────────┬──────────┘        └──────────────┬──────────────┘ │
└─────────────┼──────────────────────────────────┼────────────────┘
              │ JNI Bridge                        │ Direct Call
┌─────────────┴──────────────────────────────────┴────────────────┐
│                         Core Layer (share/)                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │ NCNN Engine  │  │ Tile Process │  │   Model Manager      │  │
│  │ (GPU/CPU)    │  │ (分块/拼接)   │  │   (模型加载/同步)    │  │
│  └──────────────┘  └──────────────┘  └──────────────────────┘  │
│  ┌────────────────────────────────────────────────────────────┐│
│  │              Distributed Computing Layer                    ││
│  │  ┌────────────┐  ┌────────────┐  ┌────────────────────┐   ││
│  │  │ Scheduler  │  │  Worker    │  │   Coordinator      │   ││
│  │  │ (任务分配)  │  │ (任务执行)  │  │   (组机协调)       │   ││
│  │  └────────────┘  └────────────┘  └────────────────────┘   ││
│  └────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
              │
┌─────────────┴───────────────────────────────────────────────────┐
│                        Utility Layer                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌───────┐ │
│  │ Image   │  │ Logger  │  │ Network │  │Benchmark│  │Crypto │ │
│  └─────────┘  └─────────┘  └─────────┘  └─────────┘  └───────┘ │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. 核心模块设计

### 3.1 通用模型管理器 (Model Manager)

#### 3.1.1 设计目标
- 支持加载任意 ncnn 格式模型（.param + .bin）
- 运行时查询模型元数据（输入/输出尺寸、缩放倍数）
- 模型目录管理（本地 + 同步）

#### 3.1.2 类设计
```cpp
// share/core/model_manager.hpp

struct ModelInfo {
    std::string name;           // 模型名称
    std::string path;           // 模型路径
    int scale_factor;           // 缩放倍数 (2x, 4x)
    int input_channels;         // 输入通道数 (1=灰度, 3=RGB)
    int tile_size;              // 建议分块大小
    size_t file_size;           // 文件大小 (用于同步决策)
    std::string hash;           // 文件哈希 (用于校验)
};

class ModelManager {
public:
    // 扫描模型目录，返回可用模型列表
    auto scan_models(const std::filesystem::path& dir) 
        -> std::vector<ModelInfo>;
    
    // 加载模型到内存（延迟加载）
    auto load_model(const std::string& name) 
        -> std::expected<std::unique_ptr<NCNNModel>, Error>;
    
    // 从模型文件推断元数据
    auto infer_metadata(const std::filesystem::path& param_path) 
        -> ModelInfo;
    
    // 计算模型哈希
    auto compute_hash(const std::filesystem::path& model_path) 
        -> std::string;
    
    // 验证模型完整性
    auto verify_model(const std::string& name) -> bool;
};
```

#### 3.1.3 模型元数据推断策略
```cpp
// 解析 .param 文件获取模型结构
// 示例 .param 内容:
// 7767517
// Input                    input                    0 1 input
// Convolution              conv1                    1 1 1 input conv1...
// ...
// 通过分析最后一层输出尺寸推断 scale_factor
```

### 3.2 NCNN 引擎抽象层

#### 3.2.1 设计目标
- 统一 CPU/GPU 执行接口
- 支持 Vulkan GPU 加速（可选回退到 CPU）
- 内存池管理（大图处理）

#### 3.2.2 类设计
```cpp
// share/core/ncnn_engine.hpp

enum class DeviceType {
    CPU,
    GPU_VULKAN  // 自动检测 Vulkan 支持
};

struct DeviceCapability {
    DeviceType type;
    std::string name;           // 设备名称 (如 "NVIDIA RTX 3080")
    int compute_units;          // CUDA cores / CPU cores
    size_t memory_mb;           // 显存/内存
    float benchmark_score;      // 基准测试得分
};

class NCNNEngine {
public:
    // 初始化引擎，选择设备
    auto initialize(DeviceType preferred = DeviceType::GPU_VULKAN) 
        -> std::expected<void, Error>;
    
    // 查询可用设备列表
    auto list_devices() -> std::vector<DeviceCapability>;
    
    // 加载模型
    auto load_model(const ModelInfo& model) 
        -> std::expected<void, Error>;
    
    // 处理单块图像
    auto process_tile(
        const ncnn::Mat& input,
        ncnn::Mat& output,
        const std::atomic<bool>& cancel_flag = {}
    ) -> std::expected<void, Error>;
    
    // Benchmark: 测量处理速度
    auto benchmark(int test_tile_size = 256) 
        -> float;  // tiles/second
    
    // 释放模型内存
    void unload_model();

private:
    ncnn::Net net_;
    ncnn::VulkanDevice* vkdev_ = nullptr;
    ncnn::Pipeline* pipeline_ = nullptr;
    DeviceType current_device_;
};
```

### 3.3 分块处理器 (Tile Processor)

#### 3.3.1 设计目标
- 大图分块处理，避免内存溢出
- 边界重叠处理，消除拼接痕迹
- 支持进度回调和取消

#### 3.3.2 类设计
```cpp
// share/core/tile_processor.hpp

struct TileConfig {
    int tile_size = 256;        // 块大小
    int overlap = 16;           // 重叠区域大小
    int batch_size = 1;         // 批处理大小
};

struct ProcessingProgress {
    int total_tiles;
    int completed_tiles;
    int current_tile_x;
    int current_tile_y;
    float percentage() const { 
        return static_cast<float>(completed_tiles) / total_tiles * 100; 
    }
};

class TileProcessor {
public:
    // 计算分块方案
    auto compute_tile_layout(
        int image_width, 
        int image_height,
        const TileConfig& config
    ) -> std::vector<TileRegion>;
    
    // 分块处理图像
    auto process_image(
        const Image& input,
        Image& output,
        NCNNEngine& engine,
        const TileConfig& config,
        std::function<void(const ProcessingProgress&)> progress_callback = {},
        const std::atomic<bool>& cancel_flag = {}
    ) -> std::expected<void, Error>;
    
private:
    // 提取单个块
    auto extract_tile(
        const Image& image, 
        const TileRegion& region
    ) -> ncnn::Mat;
    
    // 融合块到输出图像（带重叠区域混合）
    auto blend_tile(
        const ncnn::Mat& tile,
        Image& output,
        const TileRegion& region,
        int overlap
    ) -> void;
};
```

#### 3.3.3 重叠区域混合算法
```
原图分块示意 (tile_size=8, overlap=2):

  ┌────┬────┬────┐
  │ T1 │ T2 │ T3 │    横向重叠: 2 像素
  ├────┼────┼────┤    纵向重叠: 2 像素
  │ T4 │ T5 │ T6 │
  ├────┼────┼────┤    混合策略: 线性插值
  │ T7 │ T8 │ T9 │    overlap 区域内: alpha = x / overlap_size
  └────┴────┴────┘    output = prev * (1-alpha) + curr * alpha
```

### 3.4 分布式计算层

#### 3.4.1 设备能力初始化 (Init 阶段)

每个端在首次处理图片时执行：

```cpp
// share/core/distributed/benchmark.hpp

struct DeviceProfile {
    std::string device_id;              // 设备唯一标识
    std::string device_name;            // 显示名称
    DeviceType type;                    // CPU / GPU
    
    // 性能指标 (通过 benchmark 测得)
    float tiles_per_second;             // 处理速度
    int recommended_tile_size;          // 建议块大小
    size_t max_batch_size;              // 最大批处理量
    
    // 权重 (用于任务分配)
    float weight;                       // 相对权重 (所有端权重之和为 1.0)
    
    // 网络信息
    std::string ip_address;
    uint16_t port;
    std::chrono::steady_clock::time_point last_heartbeat;
};

class DeviceBenchmark {
public:
    // 执行完整的性能基准测试
    auto run_benchmark(NCNNEngine& engine) 
        -> DeviceProfile;
    
private:
    // 测试不同块大小的处理速度
    auto test_tile_sizes(NCNNEngine& engine) 
        -> std::map<int, float>;
    
    // 测试内存限制
    auto test_memory_limits(NCNNEngine& engine) 
        -> size_t;
};
```

#### 3.4.2 任务调度器 (Scheduler)

```cpp
// share/core/distributed/scheduler.hpp

struct Task {
    std::string task_id;
    std::string model_name;
    std::vector<TileRegion> tiles;      // 分配给该设备的块
    Priority priority;
    std::chrono::steady_clock::time_point deadline;
};

struct TaskAssignment {
    std::string device_id;
    std::vector<TileRegion> assigned_tiles;
    std::chrono::milliseconds estimated_time;
};

class Scheduler {
public:
    // 注册设备 (组机时调用)
    void register_device(const DeviceProfile& profile);
    
    // 移除设备
    void unregister_device(const std::string& device_id);
    
    // 根据权重分配任务 (核心算法)
    auto assign_tiles(
        const std::vector<TileRegion>& all_tiles,
        const std::string& model_name
    ) -> std::vector<TaskAssignment>;
    
    // 动态调整 (基于实时反馈)
    void update_device_performance(
        const std::string& device_id, 
        float actual_tiles_per_second
    );
    
    // 处理设备离线 (重新分配未完成任务)
    auto handle_device_failure(const std::string& device_id) 
        -> std::vector<TileRegion>;  // 返回需要重新分配的块
    
private:
    std::unordered_map<std::string, DeviceProfile> devices_;
    std::mutex devices_mutex_;
    
    // 权重归一化
    void normalize_weights();
};
```

#### 3.4.3 权重计算算法

```cpp
// 基于实时性能动态计算权重
void Scheduler::normalize_weights() {
    float total_performance = 0;
    for (const auto& [id, profile] : devices_) {
        total_performance += profile.tiles_per_second;
    }
    
    for (auto& [id, profile] : devices_) {
        profile.weight = profile.tiles_per_second / total_performance;
    }
}

// 任务分配: 按权重比例分配块数
auto Scheduler::assign_tiles(
    const std::vector<TileRegion>& all_tiles,
    const std::string& model_name
) -> std::vector<TaskAssignment> {
    std::vector<TaskAssignment> assignments;
    int total_tiles = all_tiles.size();
    int assigned_count = 0;
    
    for (const auto& [id, profile] : devices_) {
        // 按权重分配块数
        int tiles_for_device = static_cast<int>(
            std::ceil(total_tiles * profile.weight)
        );
        
        // 确保不超出剩余块数
        tiles_for_device = std::min(
            tiles_for_device, 
            static_cast<int>(all_tiles.size()) - assigned_count
        );
        
        TaskAssignment assignment;
        assignment.device_id = id;
        assignment.assigned_tiles = {
            all_tiles.begin() + assigned_count,
            all_tiles.begin() + assigned_count + tiles_for_device
        };
        
        // 估算完成时间
        assignment.estimated_time = std::chrono::milliseconds(
            static_cast<int>(tiles_for_device / profile.tiles_per_second * 1000)
        );
        
        assignments.push_back(assignment);
        assigned_count += tiles_for_device;
    }
    
    return assignments;
}
```

#### 3.4.4 Worker (工作节点)

```cpp
// share/core/distributed/worker.hpp

class Worker {
public:
    // 连接到协调器
    auto connect(
        const std::string& coordinator_ip,
        uint16_t coordinator_port,
        const std::string& auth_key
    ) -> std::expected<void, Error>;
    
    // 发送设备能力信息
    auto send_profile(const DeviceProfile& profile) -> void;
    
    // 接收任务
    auto receive_task() -> std::expected<Task, Error>;
    
    // 执行任务
    auto execute_task(
        const Task& task,
        NCNNEngine& engine,
        std::function<void(float)> progress_callback = {}
    ) -> std::expected<std::vector<ProcessedTile>, Error>;
    
    // 提交结果
    auto submit_results(const std::vector<ProcessedTile>& results) 
        -> std::expected<void, Error>;
    
    // 心跳
    void start_heartbeat(std::chrono::seconds interval = std::chrono::seconds(5));
    
private:
    std::unique_ptr<TCPSocket> socket_;
    std::string device_id_;
    std::atomic<bool> running_{false};
};
```

#### 3.4.5 Coordinator (协调器)

```cpp
// share/core/distributed/coordinator.hpp

class Coordinator {
public:
    // 启动协调服务
    auto start(uint16_t port, const std::string& auth_key) 
        -> std::expected<void, Error>;
    
    // 停止服务
    void stop();
    
    // 生成连接信息 (用于二维码/手动连接)
    auto generate_connection_info() -> ConnectionInfo;
    
    // 处理新设备连接
    auto handle_new_connection(
        std::unique_ptr<TCPSocket> socket,
        const std::string& auth_key
    ) -> void;
    
    // 分发任务到所有工作节点
    auto distribute_task(
        const Image& input_image,
        const std::string& model_name,
        const TileConfig& config
    ) -> std::expected<Image, Error>;
    
    // 收集结果
    auto collect_results() 
        -> std::expected<std::vector<ProcessedTile>, Error>;
    
    // 设备管理
    auto get_connected_devices() -> std::vector<DeviceProfile>;
    
    // 模型同步 (Prepare 阶段)
    auto sync_models() -> void;  // 确保所有设备有相同模型集

private:
    std::unique_ptr<TCPServer> server_;
    Scheduler scheduler_;
    std::unordered_map<std::string, std::unique_ptr<WorkerConnection>> workers_;
    std::string auth_key_;
    std::atomic<bool> running_{false};
};
```

---

## 4. 通信协议设计

### 4.1 协议概述

基于 TCP 的自定义二进制协议，设计原则：
- 紧凑高效：二进制格式，最小化传输开销
- 可扩展：版本字段支持协议升级
- 可靠：消息确认 + 重传机制

### 4.2 消息帧格式

```
┌──────────────────────────────────────────────────────────────┐
│                        Message Frame                          │
├────────────┬─────────────┬─────────────┬──────────────────────┤
│ Magic (4B) │ Version (1B)│ Type (2B)   │ Payload Length (4B)  │
│ 0x53534358 │    0x01     │   uint16    │      uint32          │
├────────────┴─────────────┴─────────────┴──────────────────────┤
│                        Payload (N bytes)                      │
│                     (message-specific data)                   │
├───────────────────────────────────────────────────────────────┤
│                      Checksum (4B, CRC32)                     │
└───────────────────────────────────────────────────────────────┘

Magic: "XSCS" (CrossScale)
Version: Protocol version (current: 1)
Type: Message type (see below)
Payload Length: Length of payload in bytes
Checksum: CRC32 of entire frame (excluding checksum itself)
```

### 4.3 消息类型定义

```cpp
// share/core/distributed/protocol.hpp

enum class MessageType : uint16_t {
    // 连接管理
    MSG_HANDSHAKE_REQUEST      = 0x0001,  // 客户端发起连接
    MSG_HANDSHAKE_RESPONSE     = 0x0002,  // 服务端响应
    MSG_AUTH_REQUEST           = 0x0003,  // 认证请求
    MSG_AUTH_RESPONSE          = 0x0004,  // 认证响应
    MSG_DISCONNECT             = 0x0005,  // 断开连接
    MSG_HEARTBEAT              = 0x0006,  // 心跳
    MSG_HEARTBEAT_ACK          = 0x0007,  // 心跳确认
    
    // 设备管理
    MSG_DEVICE_PROFILE         = 0x0100,  // 设备能力信息
    MSG_DEVICE_LIST            = 0x0101,  // 设备列表
    MSG_DEVICE_JOIN            = 0x0102,  // 设备加入通知
    MSG_DEVICE_LEAVE           = 0x0103,  // 设备离开通知
    
    // 模型管理
    MSG_MODEL_LIST             = 0x0200,  // 模型列表
    MSG_MODEL_REQUEST          = 0x0201,  // 请求模型文件
    MSG_MODEL_DATA             = 0x0202,  // 模型数据传输
    MSG_MODEL_SYNC_START       = 0x0203,  // 开始模型同步
    MSG_MODEL_SYNC_COMPLETE    = 0x0204,  // 模型同步完成
    
    // 任务管理
    MSG_TASK_ASSIGNMENT        = 0x0300,  // 任务分配
    MSG_TASK_ACCEPT            = 0x0301,  // 接受任务
    MSG_TASK_REJECT            = 0x0302,  // 拒绝任务
    MSG_TASK_PROGRESS          = 0x0303,  // 任务进度
    MSG_TASK_RESULT            = 0x0304,  // 任务结果
    MSG_TASK_CANCEL            = 0x0305,  // 取消任务
    
    // 图像数据
    MSG_IMAGE_INFO             = 0x0400,  // 图像元信息
    MSG_IMAGE_TILE             = 0x0401,  // 图像块数据
    MSG_IMAGE_COMPLETE         = 0x0402,  // 图像传输完成
    
    // 错误
    MSG_ERROR                  = 0xFFFF,  // 错误消息
};
```

### 4.4 核心消息格式

#### 4.4.1 Handshake
```cpp
struct HandshakeRequest {
    uint32_t protocol_version;    // 协议版本
    char device_name[64];         // 设备名称
    uint8_t device_type;          // 0=CPU, 1=GPU
};

struct HandshakeResponse {
    uint8_t accepted;             // 0=rejected, 1=accepted
    char rejection_reason[128];   // 拒绝原因 (如果 rejected)
    char server_name[64];         // 服务端名称
    uint16_t heartbeat_interval;  // 心跳间隔 (秒)
};
```

#### 4.4.2 Authentication
```cpp
struct AuthRequest {
    char auth_key[64];            // 认证密钥 (用户设置或自动生成)
};

struct AuthResponse {
    uint8_t authenticated;        // 0=failed, 1=success
    char device_id[36];           // 分配的设备 UUID
};
```

#### 4.4.3 Device Profile
```cpp
struct DeviceProfileMessage {
    char device_id[36];           // 设备 UUID
    char device_name[64];         // 显示名称
    uint8_t device_type;          // 0=CPU, 1=GPU
    
    float tiles_per_second;       // 处理速度
    uint32_t recommended_tile_size;
    uint32_t max_batch_size;
    
    // 网络信息
    char ip_address[46];          // IPv6 最长 45 字符
    uint16_t port;
};
```

#### 4.4.4 Task Assignment
```cpp
struct TaskAssignmentMessage {
    char task_id[36];             // 任务 UUID
    char model_name[64];          // 模型名称
    
    uint32_t tile_count;          // 块数量
    // 后续跟随 tile_count 个 TileInfo
    
    struct TileInfo {
        uint32_t x, y;            // 块在原图中的位置
        uint32_t width, height;   // 块尺寸
        uint32_t overlap;         // 重叠区域大小
    };
};
```

#### 4.4.5 Image Tile Transfer
```cpp
struct ImageTileMessage {
    char task_id[36];
    uint32_t tile_index;          // 块索引
    uint32_t tile_x, tile_y;      // 块位置
    uint32_t width, height;       // 块尺寸
    uint8_t channels;             // 通道数
    uint8_t pixel_format;         // 0=RGB, 1=RGBA, 2=灰度
    uint32_t data_length;         // 数据长度
    // 后续跟随 data_length 字节的图像数据
};
```

#### 4.4.6 Model Sync
```cpp
struct ModelListMessage {
    uint32_t model_count;
    // 后续跟随 model_count 个 ModelInfo
    
    struct ModelInfo {
        char name[64];
        uint32_t param_size;      // .param 文件大小
        uint32_t bin_size;        // .bin 文件大小
        char hash[64];            // SHA256 哈希
    };
};

struct ModelDataMessage {
    char model_name[64];
    uint8_t file_type;            // 0=param, 1=bin
    uint64_t file_offset;         // 文件偏移 (支持断点续传)
    uint32_t data_length;         // 本次传输长度
    // 后续跟随数据
};
```

### 4.5 连接流程

```
┌──────────┐                    ┌──────────┐
│  Worker  │                    │Coordinator│
└────┬─────┘                    └────┬─────┘
     │                               │
     │──── MSG_HANDSHAKE_REQUEST ────│
     │                               │
     │──── MSG_HANDSHAKE_RESPONSE ───│
     │                               │
     │──── MSG_AUTH_REQUEST ─────────│
     │                               │
     │──── MSG_AUTH_RESPONSE ────────│
     │    (device_id assigned)       │
     │                               │
     │──── MSG_DEVICE_PROFILE ───────│
     │                               │
     │──── MSG_MODEL_LIST ───────────│
     │                               │
     │  (模型同步，如需要)            │
     │<─── MSG_MODEL_REQUEST ────────│
     │──── MSG_MODEL_DATA ──────────>│
     │                               │
     │──── MSG_HEARTBEAT ───────────>│
     │<─── MSG_HEARTBEAT_ACK ────────│
     │                               │
     │  (等待任务)                    │
     │<─── MSG_TASK_ASSIGNMENT ──────│
     │──── MSG_TASK_ACCEPT ─────────>│
     │                               │
     │<─── MSG_IMAGE_TILE ───────────│
     │    (接收多个块数据)            │
     │                               │
     │──── MSG_TASK_RESULT ─────────>│
     │                               │
```

---

## 5. 连接方式实现

### 5.1 三种连接方式

| 方式 | 场景 | 流程 |
|------|------|------|
| **二维码扫描** | Android 加入 PC 组机 | PC 生成 QR → Android 扫描 → 自动连接 |
| **手动连接** | 无摄像头设备 | 显示 IP:Port:Key → 手动输入 → 连接 |
| **mDNS 发现** | 局域网自动发现 | 服务广播 → 客户端浏览 → 选择连接 |

### 5.2 连接信息编码

```cpp
// 二维码内容格式 (JSON)
// {
//   "v": 1,                           // 版本
//   "ip": "192.168.1.100",            // IP 地址
//   "port": 9527,                     // 端口
//   "key": "abc123xyz",               // 认证密钥
//   "name": "CrossScale-PC",          // 服务名
//   "model": "realesrgan-x4"          // 当前模型
// }

struct ConnectionInfo {
    std::string ip;
    uint16_t port;
    std::string auth_key;
    std::string server_name;
    std::string current_model;
    
    // 编码为 JSON 字符串 (用于二维码)
    auto to_json() const -> std::string;
    
    // 从 JSON 解析
    static auto from_json(const std::string& json) 
        -> std::expected<ConnectionInfo, Error>;
    
    // 编码为简短字符串 (用于手动输入)
    // 格式: "IP:PORT#KEY" 如 "192.168.1.100:9527#abc123"
    auto to_short_string() const -> std::string;
    
    // 从简短字符串解析
    static auto from_short_string(const std::string& str) 
        -> std::expected<ConnectionInfo, Error>;
};
```

### 5.3 二维码实现

#### 5.3.1 CLI 端生成二维码
```cpp
// cli/src/qr_generator.hpp

class QRGenerator {
public:
    // 生成二维码图像 (返回 ASCII 艺术，用于终端显示)
    static auto generate_ascii(const std::string& data, int size = 25) 
        -> std::string;
    
    // 生成二维码图像文件 (PNG)
    static auto generate_image(
        const std::string& data, 
        const std::filesystem::path& output_path,
        int size = 256
    ) -> std::expected<void, Error>;
};
```

#### 5.3.2 Android 端扫描二维码
```kotlin
// apps/app/src/main/java/com/crossscale/ui/scanner/QRScannerScreen.kt

@Composable
fun QRScannerScreen(
    onConnectionInfoReceived: (ConnectionInfo) -> Unit
) {
    // 使用 CameraX + ML Kit 或 ZXing
    val cameraProviderFuture = ProcessCameraProvider.getInstance(context)
    
    // 扫描回调
    fun onQRCodeScanned(content: String) {
        val info = ConnectionInfo.fromJson(content)
        onConnectionInfoReceived(info)
    }
}
```

### 5.4 mDNS 服务发现

#### 5.4.1 服务端广播 (CLI)
```cpp
// cli/src/mdns_service.hpp

class MDNSBroadcaster {
public:
    // 开始广播服务
    auto start(
        const std::string& service_name,
        uint16_t port,
        const std::string& auth_key
    ) -> std::expected<void, Error>;
    
    // 停止广播
    void stop();
    
private:
    // 服务类型: _crossscale._tcp.local.
    static constexpr const char* SERVICE_TYPE = "_crossscale._tcp.local.";
    
    // TXT 记录包含: auth_key_hash, device_name, model_name
};
```

#### 5.4.2 客户端发现 (Android)
```kotlin
// apps/app/src/main/java/com/crossscale/network/MDNSDiscovery.kt

class MDNSDiscovery(private val context: Context) {
    private val nsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    
    // 开始发现服务
    fun startDiscovery(onServiceFound: (DiscoveredService) -> Unit) {
        val discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                // 解析服务信息
                nsdManager.resolveService(serviceInfo, object : NsdManager.ResolveListener {
                    override fun onServiceResolved(resolvedInfo: NsdServiceInfo) {
                        val service = DiscoveredService(
                            name = resolvedInfo.serviceName,
                            host = resolvedInfo.host.hostAddress,
                            port = resolvedInfo.port
                            // auth_key 从 TXT 记录获取
                        )
                        onServiceFound(service)
                    }
                })
            }
        }
        
        nsdManager.discoverServices("_crossscale._tcp.", NsdManager.PROTOCOL_DNS_SD, discoveryListener)
    }
}
```

---

## 6. Android 应用设计

### 6.1 页面结构

```
┌─────────────────────────────────────────┐
│  CrossScale                              │
├─────────────────────────────────────────┤
│                                         │
│  ┌─────┐ [主页] [设备] [模型] [日志]    │
│  │ FAB │                                │
│  └─────┘                                │
│                                         │
│  ┌─────────────────────────────────┐   │
│  │                                 │   │
│  │       Page Content              │   │
│  │                                 │   │
│  │                                 │   │
│  └─────────────────────────────────┘   │
│                                         │
└─────────────────────────────────────────┘
```

### 6.2 页面功能

#### 6.2.1 主页 (Home)
- 当前模式显示：单机 / 分布式
- 快速操作：选择图片 → 开始超分
- 当前模型：显示 + 切换
- 设备状态：已连接设备列表 (分布式模式)

#### 6.2.2 设备页 (Devices)
- 设备发现：二维码扫描 / 手动输入 / mDNS 列表
- 当前组机状态
- 设备能力详情 (点击展开)
- 断开连接

#### 6.2.3 模型页 (Models)
- 已下载模型列表
- 模型详情：名称、缩放倍数、大小
- 导入模型：从文件选择
- 删除模型

#### 6.2.4 日志页 (Logs)
- 实时日志显示
- 日志级别过滤：DEBUG / INFO / WARNING / ERROR
- 颜色编码
- 搜索/过滤
- 导出日志

### 6.3 核心组件设计

```kotlin
// apps/app/src/main/java/com/crossscale/

// UI 层
ui/
├── theme/
│   ├── Theme.kt              // MD3 主题
│   ├── Color.kt              // 颜色定义
│   └── Type.kt               // 字体定义
├── components/
│   ├── LogViewer.kt          // 日志显示组件
│   ├── DeviceCard.kt         // 设备卡片
│   ├── ModelCard.kt          // 模型卡片
│   ├── ConnectionDialog.kt   // 连接对话框
│   └── ProcessingProgress.kt // 处理进度
├── screens/
│   ├── HomeScreen.kt
│   ├── DevicesScreen.kt
│   ├── ModelsScreen.kt
│   └── LogsScreen.kt
└── navigation/
    └── Navigation.kt         // 导航配置

// ViewModel 层
viewmodel/
├── MainViewModel.kt          // 主 ViewModel
├── ProcessingViewModel.kt    // 处理任务
└── DeviceViewModel.kt        // 设备管理

// 数据层
data/
├── model/
│   ├── DeviceProfile.kt
│   ├── ModelInfo.kt
│   └── ProcessingTask.kt
├── repository/
│   ├── ModelRepository.kt
│   └── DeviceRepository.kt
└── network/
    ├── CoordinatorClient.kt  // 协调器客户端
    └── MessageHandler.kt     // 消息处理
```

### 6.4 JNI 接口设计

```cpp
// apps/app/src/main/cpp/jni_bridge.cpp

extern "C" {

// 初始化引擎
JNIEXPORT jlong JNICALL
Java_com_crossscale_native_NCNNEngine_nativeInit(
    JNIEnv* env, 
    jobject thiz, 
    jint device_type  // 0=CPU, 1=GPU
);

// 加载模型
JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NCNNEngine_nativeLoadModel(
    JNIEnv* env, 
    jobject thiz, 
    jlong engine_ptr,
    jstring model_path
);

// 处理图像
JNIEXPORT jbyteArray JNICALL
Java_com_crossscale_native_NCNNEngine_nativeProcessImage(
    JNIEnv* env, 
    jobject thiz,
    jlong engine_ptr,
    jbyteArray input_image,
    jint width,
    jint height,
    jint channels
);

// 运行 benchmark
JNIEXPORT jfloat JNICALL
Java_com_crossscale_native_NCNNEngine_nativeBenchmark(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
);

// 获取设备列表
JNIEXPORT jobjectArray JNICALL
Java_com_crossscale_native_NCNNEngine_nativeGetDevices(
    JNIEnv* env,
    jclass clazz
);

// 释放引擎
JNIEXPORT void JNICALL
Java_com_crossscale_native_NCNNEngine_nativeRelease(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
);

}  // extern "C"
```

---

## 7. CLI 设计

### 7.1 命令结构

```bash
# 单机模式
crossscale process -i input.png -o output.png -m realesrgan-x4

# 启动协调服务器
crossscale server start -p 9527 -k mysecretkey

# 作为工作节点连接
crossscale worker connect 192.168.1.100:9527 -k mysecretkey

# 列出设备
crossscale device list

# 模型管理
crossscale model list
crossscale model import /path/to/model.param

# 运行 benchmark
crossscale benchmark

# 显示连接信息 (含二维码)
crossscale server info
```

### 7.2 核心组件

```cpp
// cli/src/

main.cpp                  // 入口
commands/
├── command.hpp           // 命令基类
├── process_command.cpp   // 处理命令
├── server_command.cpp    // 服务器命令
├── worker_command.cpp    // 工作节点命令
├── model_command.cpp     // 模型管理命令
└── device_command.cpp    // 设备管理命令

server/
├── coordinator_impl.cpp  // 协调器实现
└── qr_display.cpp        // 二维码显示

utils/
├── console_ui.cpp        // 终端 UI
└── config.cpp            // 配置管理
```

---

## 8. 日志系统设计

### 8.1 日志级别

```cpp
// share/utils/logger.hpp

enum class LogLevel {
    DEBUG    = 0,
    INFO     = 1,
    WARNING  = 2,
    ERROR    = 3,
    CRITICAL = 4
};
```

### 8.2 日志格式

```
[2024-01-15 10:30:45.123] [Engine] [INFO] Loading model: realesrgan-x4.param
[2024-01-15 10:30:45.456] [Engine] [INFO] Model loaded successfully (23.4 MB)
[2024-01-15 10:30:46.789] [Worker] [DEBUG] Processing tile 12/100
[2024-01-15 10:30:47.012] [Scheduler] [WARNING] Device "android-01" is slow, reassigning tasks
[2024-01-15 10:30:48.345] [Network] [ERROR] Connection lost: device "pc-02"
```

### 8.3 颜色支持

```cpp
// ANSI 颜色代码
namespace console {
    constexpr const char* RESET   = "\033[0m";
    constexpr const char* RED     = "\033[31m";     // ERROR
    constexpr const char* GREEN   = "\033[32m";     // INFO
    constexpr const char* YELLOW  = "\033[33m";     // WARNING
    constexpr const char* BLUE    = "\033[34m";     // DEBUG
    constexpr const char* MAGENTA = "\033[35m";     // CRITICAL
    constexpr const char* CYAN    = "\033[36m";     // Tag
    constexpr const char* WHITE   = "\033[37m";     // Message
}
```

### 8.4 日志接口

```cpp
// share/utils/logger.hpp

class Logger {
public:
    static Logger& instance();
    
    void set_level(LogLevel level);
    void set_output_file(const std::filesystem::path& path);
    
    void log(LogLevel level, std::string_view tag, std::string_view message);
    
    // 便捷方法
    void debug(std::string_view tag, std::string_view message);
    void info(std::string_view tag, std::string_view message);
    void warning(std::string_view tag, std::string_view message);
    void error(std::string_view tag, std::string_view message);
    void critical(std::string_view tag, std::string_view message);
    
    // Android 回调
    using AndroidLogCallback = void(*)(LogLevel, const char*, const char*);
    void set_android_callback(AndroidLogCallback callback);

private:
    Logger() = default;
    LogLevel min_level_ = LogLevel::INFO;
    std::mutex mutex_;
    std::ofstream file_;
    AndroidLogCallback android_callback_ = nullptr;
};

// 便捷宏
#define LOG_DEBUG(tag, msg)   Logger::instance().debug(tag, msg)
#define LOG_INFO(tag, msg)    Logger::instance().info(tag, msg)
#define LOG_WARNING(tag, msg) Logger::instance().warning(tag, msg)
#define LOG_ERROR(tag, msg)   Logger::instance().error(tag, msg)
#define LOG_CRITICAL(tag, msg) Logger::instance().critical(tag, msg)
```

---

## 9. xmake 构建配置

### 9.1 根 xmake.lua

```lua
-- xmake.lua (根目录)
set_project("crossscale")
set_version("0.1.0")
set_languages("c++23")

-- 通用设置
add_rules("mode.debug", "mode.release")

if is_mode("release") then
    set_optimize("fastest")
    set_strip("all")
end

-- 依赖
add_requires("ncnn", {configs = {vulkan = true}})
add_requires("libwebp")
add_requires("stb")

-- 包含子项目
includes("share", "cli")
```

### 9.2 share/xmake.lua

```lua
-- share/xmake.lua
target("crossscale-core")
    set_kind("static")
    
    add_packages("ncnn", "libwebp", "stb")
    
    add_includedirs(".", {public = true})
    
    add_files(
        "core/engine.cpp",
        "core/ncnn_engine.cpp",
        "core/tile_processor.cpp",
        "core/model_manager.cpp",
        "core/distributed/scheduler.cpp",
        "core/distributed/worker.cpp",
        "core/distributed/coordinator.cpp",
        "core/distributed/protocol.cpp",
        "utils/image.cpp",
        "utils/logger.cpp",
        "utils/network.cpp",
        "utils/benchmark.cpp",
        "utils/crypto.cpp"
    )
    
    -- 平台特定
    if is_plat("android") then
        add_files("utils/android_utils.cpp")
    end
```

### 9.3 cli/xmake.lua

```lua
-- cli/xmake.lua
target("crossscale-cli")
    set_kind("binary")
    
    add_deps("crossscale-core")
    add_packages("ncnn", "libwebp", "stb")
    
    -- CLI 特定依赖
    add_requires("cxxopts")  -- 命令行参数解析
    add_requires("qrcodegen")  -- 二维码生成
    
    add_files(
        "src/main.cpp",
        "src/commands/*.cpp",
        "src/server/*.cpp",
        "src/utils/*.cpp"
    )
    
    -- 安装
    after_install(function (target)
        local bindir = path.join(target:installdir(), "bin")
        os.cp("$(projectdir)/models", bindir)
    end)
```

### 9.4 Android 构建配置

Android 使用 Gradle + CMake/xmake 混合构建：

```kotlin
// apps/app/build.gradle.kts
android {
    ndkVersion = "25.2.9519653"
    
    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_TOOLCHAIN=clang"
                )
            }
        }
    }
    
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}
```

```cmake
# apps/app/src/main/cpp/CMakeLists.txt
cmake_minimum_required(VERSION 3.18.1)

project("crossscale")

# ncnn 预编译库
set(NCNN_DIR ${CMAKE_SOURCE_DIR}/libs/ncnn)
include_directories(${NCNN_DIR}/include)
add_library(ncnn STATIC IMPORTED)
set_target_properties(ncnn PROPERTIES
    IMPORTED_LOCATION ${NCNN_DIR}/lib/${ANDROID_ABI}/libncnn.a
)

# Vulkan
find_library(vulkan vulkan)

# 共享核心代码
add_library(crossscale-native SHARED
    jni_bridge.cpp
    ${CMAKE_SOURCE_DIR}/../../../share/core/engine.cpp
    ${CMAKE_SOURCE_DIR}/../../../share/core/ncnn_engine.cpp
    # ... 其他源文件
)

target_link_libraries(crossscale-native
    ncnn
    vulkan
    android
    log
    jnigraphics
)
```

---

## 10. 开发里程碑

### Phase 1: 基础架构 (Week 1-2)
- [ ] 项目骨架搭建
- [ ] xmake 构建配置
- [ ] 日志系统实现
- [ ] 基础图像 I/O

### Phase 2: 核心引擎 (Week 3-4)
- [ ] ncnn 引擎封装
- [ ] 模型管理器
- [ ] 分块处理器
- [ ] Benchmark 功能

### Phase 3: CLI 版本 (Week 5-6)
- [ ] 命令行框架
- [ ] 单机处理功能
- [ ] 服务器模式
- [ ] 二维码生成

### Phase 4: 分布式计算 (Week 7-8)
- [ ] 通信协议实现
- [ ] 任务调度器
- [ ] Worker 实现
- [ ] Coordinator 实现
- [ ] 模型同步

### Phase 5: Android 应用 (Week 9-12)
- [ ] 项目搭建
- [ ] JNI 桥接
- [ ] UI 界面
- [ ] 二维码扫描
- [ ] mDNS 发现

### Phase 6: 集成测试 (Week 13-14)
- [ ] 多设备测试
- [ ] 性能优化
- [ ] Bug 修复

### Phase 7: 桌面端图形界面 (Week 15-20)

> **技术栈调研与选型**见下方 §11。

- [ ] 桌面 GUI 项目骨架搭建与构建系统集成
- [ ] 主窗口框架：拖拽导入、图片预览、模型选择
- [ ] 处理进度与实时预览（对比查看 before/after）
- [ ] 设备管理页：分布式设备列表、连接状态
- [ ] 日志查看器集成
- [ ] 设置页：GPU/CPU 切换、tile 参数、输出格式
- [ ] 跨平台打包（Linux AppImage / Windows installer / macOS dmg）

### Phase 8: 文档与发布 (Week 21-22)
- [ ] 用户文档
- [ ] API 文档
- [ ] 示例模型
- [ ] 发布准备

---

## 11. 桌面端图形界面技术栈调研

### 11.1 候选方案对比

| 方案 | 语言 | UI 共享度 | Android 对齐成本 | 与现有代码集成 | 生态成熟度 | 适合场景 |
|------|------|-----------|------------------|----------------|-----------|---------|
| **KMP + Compose Multiplatform** | Kotlin | ⭐⭐⭐ 主题/页面可共享 | ⭐⭐⭐ 最低 | ⭐⭐ 通过 CLI / JNI / FFI 桥接原生后端 | ⭐⭐⭐ JetBrains 官方 | Android + Desktop 一体化 |
| **Qt 6** | C++ | ❌ 无法复用 Compose UI | ⭐ 需重做桌面设计系统 | ⭐⭐⭐ 原生 C++ | ⭐⭐⭐ 功能全面 | 完整桌面应用 |
| **Tauri 2.0** | Rust + Web | ❌ 无法复用 Compose UI | ⭐⭐ 需重写前端 | ⭐⭐ FFI/Sidecar | ⭐⭐⭐ 增长中 | 现代 Web UI |
| **Electron** | JS/TS | ❌ 无法复用 Compose UI | ⭐⭐ 需重写前端 | ⭐ Node addon / IPC | ⭐⭐⭐ 最成熟 | 快速开发 Web 应用 |
| **GTK 4 / wxWidgets** | C/C++ | ❌ 无法复用 Compose UI | ⭐ 需重做桌面设计 | ⭐⭐⭐ 原生 C++ | ⭐⭐ 稳定但偏传统 | 平台原生工具 |

### 11.2 推荐方案：KMP + Compose Multiplatform

**推荐理由：**

1. **直接继承 Android 设计语言**：桌面端可复用 Android 现有的 Compose 主题、颜色、排版和页面结构，避免再次维护两套视觉系统
2. **技术栈统一**：Android 与 Desktop 同为 Kotlin + Compose，后续页面演进可优先在 `shared-ui` 中一次实现、多端消费
3. **迁移成本最低**：当前 Android 已是 Compose 项目，新增 `shared-ui` 和 `desktopApp` 模块即可扩展桌面端
4. **跨平台原生分发**：Compose Desktop 原生支持 macOS / Windows / Linux 打包，适合后续发布桌面版本
5. **后端可渐进演进**：桌面端现阶段可通过 CLI bridge 复用已有 C++ 能力，后续再升级到 JNI / JNA / Kotlin/Native FFI 直连

**当前架构：**

```
apps/
├── app/                                # Android 应用
├── shared-ui/                          # KMP 共享 Compose UI
│   └── src/commonMain/kotlin/
│       └── com/crossscale/sharedui/
│           ├── theme/                  # 共享主题
│           ├── model/                  # 共享桌面 UI 状态模型
│           └── home/                   # 共享桌面首页/预览/日志布局
└── desktopApp/                         # Compose Desktop 宿主
    └── src/desktopMain/kotlin/
        └── com/crossscale/desktop/
            ├── Main.kt                # 桌面入口
            ├── DesktopApp.kt          # Compose Desktop 宿主装配
            ├── DesktopController.kt   # 桌面状态、模型扫描、CLI bridge
            └── DesktopFileDialogs.kt  # 文件/目录选择
```

**现阶段桌面后端策略：**

- **短期**：Compose Desktop 前端通过 `DesktopController` 调用 `crossscale-cli process`，快速复用已有推理能力
- **中期**：抽出稳定的 JNI/JNA/FFI 接口，桌面端直接调用共享 C++ 核心，减少进程桥接开销
- **长期**：将更多 Android Compose 组件和状态模型上移到 `shared-ui`，实现真正的多端 UI 共享

### 11.3 备选方案

若 KMP 无法满足桌面端特定需求（例如复杂原生窗口系统、深度系统集成），备选路径：

- **Plan B: Qt 6** — 最完整的 C++ 桌面 GUI 方案，适合极重桌面交互和原生系统集成，但与 Android Compose 设计体系割裂
- **Plan C: Tauri 2.0** — 适合偏运营工具 / Web 风格产品，但无法复用 Android Compose UI，需要重新设计前端

### 11.4 技术风险

| 风险 | 缓解 |
|------|------|
| 共享 UI 与平台实现边界不清 | 严格区分 `shared-ui`（纯 Compose）与 `desktopApp`（文件系统/进程/平台能力） |
| CLI bridge 带来额外进程开销 | 作为过渡方案保留，后续切换到原生 FFI 直连 |
| Compose Desktop 打包细节较多 | 优先用官方 `nativeDistributions`，逐平台补齐图标、签名与安装器配置 |
| Android / Desktop 状态模型分化 | 优先共享主题、信息架构和展示组件，再逐步提炼可复用状态层 |

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| ncnn Vulkan 兼容性 | 部分设备 GPU 不可用 | 自动回退到 CPU，提示用户 |
| 网络不稳定 | 任务中断 | 断点续传、任务重新分配 |
| 模型格式多样 | 加载失败 | 完善元数据推断，提供格式转换工具 |
| 内存溢出 | 处理大图失败 | 动态调整块大小，内存监控 |
| Android 权限 | 功能受限 | 最小权限原则，运行时请求 |

---

## 13. 后续扩展

1. **云端模型市场**：用户可下载社区模型
2. **互联网模式**：支持远程设备加入 (需打洞/中继)
3. **视频超分**：支持视频处理
4. **性能监控**：实时性能仪表板
5. **任务队列**：批量处理支持

---

*本文档将随项目进展持续更新。*
