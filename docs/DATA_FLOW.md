# CrossScale 数据流与调用链

> 本文档描述系统关键操作的数据流路径、组件间调用关系和线程模型。

---

## 1. 单机图像超分 (PC CLI 单文件)

### 调用链

```
cmd_process.cpp / process_batch.cpp (process 命令)
  │
  ├─ ModelManager::scan_models()          // 扫描 models/ 目录
  │     └─ 解析 .param 文件推断 scale_factor / channels
  │
  ├─ create_ncnn_engine() → Engine::initialize(DeviceType::GPU_VULKAN)
  │     ├─ try_init_vulkan()  // GPU 优先
  │     │    ├─ ncnn::create_gpu_instance()
  │     │    ├─ 枚举 GPU 打分选最优（discrete > integrated > others）
  │     │    └─ 创建 ncnn::Net + VkCompute
  │     └─ 失败 → 回退 CPU 初始化
  │
  ├─ Engine::load_model(param_path, bin_path)
  │     ├─ net->load_param()
  │     └─ net->load_model()
  │
  ├─ [auto-tile] TileSizeOptimizer::find_optimal_tile_size()
  │     ├─ 对 {64,128,192,256,320,384,448,512} 遍历
  │     ├─ 每个尺寸: engine.process() benchmark 实测 tiles/sec
  │     ├─ 淘汰超出 80% 可用内存的尺寸
  │     └─ 在最小耗时 ×1.05 容忍窗口内选最大 tile
  │
  ├─ Image::load(input_path)              // 加载原图
  │     └─ libpng/libjpeg/libwebp → 解码到 uint8[] HWC
  │
  ├─ normalize_channels_for_model()       // 通道数转换
  │
  └─ TileProcessor::process_image_u8()
        ├─ [GPU 快速路径] Engine::process_tiles_gpu_u8_queue()
        │     ├─ 整图 u8 上传 GPU
        │     ├─ GPU 上 crop + HWC→CHW shader
        │     ├─ ncnn 推理
        │     ├─ CHW→HWC shader + 量化回 u8
        │     └─ 结果下载回 CPU
        │
        └─ [通用路径] 三段流水线
              ├─ Load 线程: extract_tile_u8() → 切 tile + u8→float
              │     └─ 推入 prepared_queue (容量 2)
              ├─ 主线程: Engine::process_batch()
              │     ├─ GPU: clone→extract 逐个串行提交
              │     └─ CPU: std::async 并行提交
              └─ Save 线程: blend_tile()
                    ├─ 重叠区二次衰减权重 (w*w)
                    ├─ 归一化消除接缝
                    └─ 推入 output_queue → Image::save()
```

### 数据流

```
┌──────────┐   ┌──────────────┐   ┌────────────┐   ┌──────────┐
│ Input    │ → │ CPU: Extract │ → │ GPU: ncnn  │ → │ CPU:     │ → Output
│ Image    │   │ tiles + u8→f │   │ inference  │   │ Blend    │   Image
│ .png/.jpg│   │              │   │            │   │ f→u8     │
└──────────┘   └──────────────┘   └────────────┘   └──────────┘
      ↑                ↑                ↑                ↑
   Load线程       Prepare线程      Process主线程     Save线程
  (有界队列)      (有界队列)        (串行化)         (有界队列)
```

---

## 2. 目录批量处理 (PC CLI --jobs / --gpu)

### CPU 多线程模式 (`--jobs N`)

```
cmd_process.cpp / process_batch.cpp
  │
  ├─ EnginePool(N, param, bin, CPU)    // 创建 N 个独立 CPU Engine
  │     └─ acquire() / release()
  │
  └─ std::async × N 个任务
        └─ 每个任务:
             pool.acquire() → process_one() → pool.release()
```

### GPU 流水线模式 (`--gpu` 目录)

```
cmd_process.cpp / process_batch.cpp
  │
  ├─ 单 Engine（GPU Vulkan 不支持并发提交）
  ├─ Prepare 线程池（~2 线程）
  │     └─ Image::load + normalize_channels → push prepared_queue
  ├─ Process 线程（1 线程）
  │     ├─ 从 prepared_queue 取批量（自适应队列深度 1~8）
  │     ├─ [GPU queue] Engine::process_tiles_gpu_u8_queue()
  │     └─ push processed_queue
  └─ Save 线程池（~2 线程）
        └─ Image::save → 写文件
```

### GPU 多 Engine 模式 (`--jobs N --gpu`)

```
cmd_process.cpp / process_batch.cpp
  │
  ├─ N 个独立 GPU Engine（每个有自己的 ncnn::Net + VkCompute）
  ├─ 每个 Engine 串行处理自己的图像列表
  └─ ❗ 注意: N 个 GPU Engine 共享同一 Vulkan 队列
        ncnn 内部通过队列互斥串行化提交，实际 GPU 利用率下降
        推荐 : "--gpu" 单引擎 + 三级流水线，而非多引擎
```

---

## 3. 分布式集群超分

### 启动与握手

```
┌──────────┐                    ┌──────────┐                    ┌──────────┐
│ CLI      │                    │Coordinator│                    │ Worker   │
│ server   │                    │ (服务端)   │                    │ (工作端)   │
│ start    │                    │           │                    │          │
└────┬─────┘                    └────┬──────┘                    └────┬─────┘
     │  Coordinator::start(port)     │                              │
     │ ─────────────────────────────►│                              │
     │                               │  TCPServer::listen(9527)     │
     │                               │  accept_loop() 等待连接      │
     │                               │                              │
     │   print_connection_card()     │                              │
     │   (IP + Port + Key + QR)      │                              │
     │◄──────────────────────────────│                              │
     │                               │                              │
     │                               │◄───── TCP connect ──────────│
     │                               │                              │  worker
     │                               │                              │  connect
     │                               │  MSG_AUTH_REQUEST            │  host:port
     │                               │◄─────────────────────────────│  -k key
     │                               │                              │
     │                               │  MSG_AUTH_RESPONSE           │
     │                               │  (device_id)                 │
     │                               │ ─────────────────────────────►│
     │                               │                              │
     │                               │  MSG_DEVICE_PROFILE          │  engine.
     │                               │  (tps, type, tile_size)     │  benchmark(256)
     │                               │◄─────────────────────────────│
     │                               │                              │
     │                               │  Scheduler::register_device()│
     │                               │  → normalize_weights()       │
     │                               │                              │
     │                               │◄── MSG_HEARTBEAT (每 5s) ───│
```

### 任务执行 (客户端提交)

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│ CLI      │     │Coordinator│     │ Scheduler│     │ Worker1  │
│ client   │     │ (服务端)   │     │          │     │ (权重0.6)│
│ process  │     │           │     │          │     │          │
└────┬─────┘     └────┬──────┘     └────┬─────┘     └────┬─────┘
     │                │                │                │
     │ MSG_CLIENT_    │                │                │
     │ SUBMIT(整图)   │                │                │
     │───────────────►│                │                │
     │                │ 计算 tile 数    │                │
     │                │                │                │
     │                │ assign_tiles() │                │
     │                │───────────────►│                │
     │                │                │ weight[0]=0.6  │
     │                │                │ weight[1]=0.4  │
     │                │                │ tiles[0..59] → W1 │
     │                │                │ tiles[60..99] → W2 │
     │                │◄───────────────│                │
     │                │                │                │
     │                │ MSG_TASK_      │                │
     │                │ ASSIGNMENT +   │                │
     │                │ TILES_BATCH    │                │
     │                │────────────────────────────────►│
     │                │                │                │
     │                │                │     [可选] MSG_MODEL_REQUEST
     │                │                │◄───────────────│
     │                │                │     MSG_MODEL_DATA
     │                │                │───────────────►│
     │                │                │                │
     │                │                │     Worker::execute_task()
     │                │                │     → engine.process(逐 tile)
     │                │                │     → MSG_TASK_PROGRESS(循环)
     │                │◄───────────────│────────────────│
     │                │                │                │
     │                │ 等 待 收 齐 所 有  tile         │
     │                │                │                │
     │                │ collect_results() → stitch_tiles() │
     │                │                │                │
     │ MSG_CLIENT_    │                │                │
     │ RESPONSE(结果)  │                │                │
     │◄───────────────│                │                │
     │                │                │                │
     │ Image::save()  │                │                │
```

### 多波调度机制

Coordinator 的 `distribute_task()` 使用**多波循环**处理 tile 分配：

```
wave = 0
pending = all_tiles

while pending not empty:
    assignments = Scheduler::assign_tiles(pending)
    for each worker:
        send TASK_ASSIGNMENT + TILES_BATCH
    for each worker:
        wait TASK_RESULT (超时 30s)
        if timeout or error:
            failed_tiles ← worker's assigned tiles
            mark worker as dead (Scheduler::unregister)
    pending = failed_tiles
    wave++

stitch all received tiles → return result
```

---

## 4. Android 本地超分

```
用户操作 UI
  │
  ├─ CrossScaleApp (Composable)
  │     └─ 用户选择图片 + 模型 → 点击处理
  │
  ├─ ProcessingViewModel::enqueueBatch()
  │     └─ 加入批处理队列
  │
  ├─ ProcessingViewModel::startBatchQueue()
  │     └─ 循环处理队列中的任务
  │
  ├─ NativeImageProcessingRepository::processImage()
  │     └─ 调用 JNI 方法
  │
  ├─ NCNNEngine.nativeProcessImage()  [JNI bridge]
  │     └─ jni_bridge.cpp → crossscale::create_ncnn_engine()
  │                          → Engine::initialize(CPU)  [Android 只用 CPU]
  │                          → Engine::load_model()
  │                          → TileProcessor::process_image_u8()
  │                          → 返回 byte[] 结果
  │
  └─ Image::save() → 写入 output 目录
```

**Android 限制**：当前 Android JNI 桥仅暴露 CPU 推理路径，Vulkan GPU 推理尚未在 Android 端启用。

---

## 5. Android 加入分布式集群

```
DeviceViewModel
  │
  ├─ [自动发现] MDNSDiscovery::start()
  │     └─ Android NsdManager → 发现 _crossscale._tcp 服务
  │
  ├─ [QR 扫描] QR 扫码 → JoinInviteParser.parseJsonInvite()
  │     └─ 解析 JSON: {"v":1,"ip":"192.168.1.100","port":9527,"key":"xxx"}
  │
  └─ CoordinatorClient::connect()
        └─ NativeWorker.nativeConnect(host, port, key)  [JNI]
              └─ jni_bridge.cpp → crossscale::Worker::connect()
                                    → 认证 → benchmark → 进入任务循环

Worker 任务循环 (AndroidWorkerSession):
  while connected:
    receive_task() → engine.process() → submit_results()
    └─ 心跳线程每 5s 发送 MSG_HEARTBEAT
```

---

## 6. 本地 Serve Daemon (预热引擎)

```
第一次:  crossscale serve start --model Real-ESRGANv3-anime-x2
  → init Engine + load model (~490ms固定开销)
  → 监听 Unix Domain Socket: ~/.cache/crossscale/serve.sock

以后每次: crossscale process -i input.png -o output.png
  → 探测 sock 文件 → 连接 UnixSocket
  → 发送 ServeRequest + 图像二进制
  → Daemon 端:
       mutex.lock() → [如果模型不同则 reload] → TileProcessor::process_image_u8()
       → mutex.unlock() → 返回 ServeResponse + 结果
  → 写输出文件
  → 跳过 Engine init + model load，节省 ~490ms
```

---

## 7. Android 数据层关系

```
AppContainer (手工 DI)
  ├── ModelRepository        → models/ 目录扫描 + asset 拷贝
  ├── DeviceRepository       → 连接设备列表 (StateFlow)
  ├── LogRepository          → 日志收集 (环形缓冲区)
  ├── PairingRepository      → 已配对设备持久化
  ├── InputSourceRepository  → 输入源 URI 管理
  ├── ProcessingQueueRepository    → 队列持久化 (JSON)
  ├── ProcessingCheckpointRepository → 断点续传
  ├── BatchOutputRepository  → 输出路径管理
  └── NativeImageProcessingRepository → JNI 桥封装

ViewModel ──依赖──► Repository ──数据──► StateFlow ──收集──► UI (Compose)
```

---

## 8. 配置加载优先级

```
1. 默认值（ProcessingConfig 成员初始值）
     │
2. JSON 配置文件（--config file.json）
     │  └─ ProcessingConfig::load()
     │
3. CLI 命令行参数（显式指定）
     │  └─ 覆盖配置文件中的同名字段
     │
4. 引擎自动检测（如未显式指定）
     ├─ tile_size → engine->get_recommended_tile_size() [基于 GPU VRAM]
     └─ auto_tile → TileSizeOptimizer::find_optimal_tile_size() [实测]
```

优先级：**CLI 参数 > 配置文件 > 自动检测 > 默认值**
