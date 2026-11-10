#pragma once

// extern "C" surface over upix::NCNNEngineImpl — the ONLY FFI boundary of the
// Rust workspace's C++ island. Hand-mirrored in Rust (no bindgen); struct
// layouts below are the contract.
//
// Rules (CONTRACT §7):
//   - No C++ exception may cross this boundary: every entry point wraps its
//     body in catch-all and converts to {code, message}.
//   - cancel_flag is a `const std::atomic<bool>*` (may be null; layout is
//     ABI-compatible with Rust's AtomicBool on x86_64/aarch64 Linux).
//   - progress is a plain function pointer + user context (trampolined into
//     std::function inside the shim; never stored beyond the call).
//   - Strings are written into caller-owned buffers, NUL-terminated, truncated
//     to cap-1 bytes. No ownership crosses the boundary.

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// --- shared structs (layout pinned; mirrored 1:1 in Rust) -------------------

// Mirrors upix::DeviceType { CPU, GPU_VULKAN }.
enum {
    UPIX_DEVICE_CPU = 0,
    UPIX_DEVICE_GPU_VULKAN = 1,
};

typedef struct UpixDevice {
    int32_t dtype;            // UPIX_DEVICE_*
    char name[256];           // NUL-terminated, truncated
    int32_t compute_units;
    uint64_t memory_mb;
    float benchmark_score;
} UpixDevice;

// Mirrors upix::TileRegion (uint32 x, y, width, height, overlap).
typedef struct UpixTileRegion {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t overlap;
} UpixTileRegion;

// Mirrors upix::TileConfig.
typedef struct UpixTileConfig {
    int32_t tile_size;
    int32_t overlap;
    int32_t batch_size;
} UpixTileConfig;

// Mirrors upix::ProcessingProgress (int total, completed; uint32 x, y).
typedef struct UpixProgress {
    int32_t total_tiles;
    int32_t completed_tiles;
    uint32_t current_tile_x;
    uint32_t current_tile_y;
} UpixProgress;

// Mirrors upix::GPUTileTaskU8 (regions vector + std::function flattened into
// pointer array + fn ptr/ctx).
typedef struct UpixGpuTileTask {
    const uint8_t* input_data;
    uint8_t* output_data;
    int32_t input_width;
    int32_t input_height;
    int32_t channels;
    const UpixTileRegion* regions;
    int32_t region_count;
    UpixTileConfig config;
    void (*progress_callback)(const UpixProgress*, void* ctx);
    void* progress_ctx;
} UpixGpuTileTask;

// Status of any fallible entry point. code == 0 means success; otherwise the
// engine's EngineError code (230 = OOM, ...) with its message (NUL-terminated,
// truncated). code 199 = unexpected C++ exception caught at the boundary.
typedef struct UpixEngineStatus {
    int32_t code;
    char message[512];
} UpixEngineStatus;

// Opaque engine instance.
typedef struct UpixEngine UpixEngine;

// --- lifecycle --------------------------------------------------------------

// Injects the precompiled SPIR-V pack (Rust: include_bytes!("csrc/spirv_pack.bin")).
// Must be called before the first Vulkan engine initialize; bytes are NOT
// copied — the caller must keep them alive for the process lifetime.
void upix_engine_set_spirv_pack(const uint8_t* data, size_t size);

UpixEngine* upix_engine_create(void);
void upix_engine_destroy(UpixEngine* engine);

// preferred: UPIX_DEVICE_CPU / UPIX_DEVICE_GPU_VULKAN.
UpixEngineStatus upix_engine_initialize(UpixEngine* engine, int preferred);

// Route engine logs to stderr (1) or stdout (0). The CLI's machine-output modes
// (`--json`, UPIX_PROGRESS=json) reserve stdout for events, but the engine has
// its own logger (utils/logger.hpp) whose sink this Rust-side flag cannot
// reach — without this entry point those lines corrupt the event stream.
// Process-global; call before the first engine LOG_*.
void upix_engine_set_log_stderr(int enable);

// Writes up to cap devices; actual count in *out_len. *out_len is always set
// (also when returning an error).
UpixEngineStatus upix_engine_list_devices(UpixEngine* engine, UpixDevice* out,
                                          int32_t cap, int32_t* out_len);

UpixEngineStatus upix_engine_load_model(UpixEngine* engine, const char* param_path,
                                        const char* bin_path);
void upix_engine_unload_model(UpixEngine* engine);

// --- inference --------------------------------------------------------------

UpixEngineStatus upix_engine_process(UpixEngine* engine, const float* input, float* output,
                                     int32_t width, int32_t height, int32_t channels,
                                     const std::atomic<bool>* cancel_flag);

UpixEngineStatus upix_engine_process_u8(UpixEngine* engine, const uint8_t* input, uint8_t* output,
                                        int32_t width, int32_t height, int32_t channels,
                                        const std::atomic<bool>* cancel_flag);

// Pointer arrays of length `count`; each element is width*height*channels floats.
UpixEngineStatus upix_engine_process_batch(UpixEngine* engine, const float* const* inputs,
                                           float* const* outputs, int32_t count, int32_t width,
                                           int32_t height, int32_t channels,
                                           const std::atomic<bool>* cancel_flag);

UpixEngineStatus upix_engine_process_tiles_gpu(UpixEngine* engine, const float* input_data,
                                               float* output_data, int32_t input_width,
                                               int32_t input_height, int32_t channels,
                                               const UpixTileRegion* regions, int32_t region_count,
                                               UpixTileConfig config,
                                               void (*progress_cb)(const UpixProgress*, void* ctx),
                                               void* progress_ctx,
                                               const std::atomic<bool>* cancel_flag);

UpixEngineStatus upix_engine_process_tiles_gpu_u8(UpixEngine* engine, const uint8_t* input_data,
                                                  uint8_t* output_data, int32_t input_width,
                                                  int32_t input_height, int32_t channels,
                                                  const UpixTileRegion* regions,
                                                  int32_t region_count, UpixTileConfig config,
                                                  void (*progress_cb)(const UpixProgress*,
                                                                      void* ctx),
                                                  void* progress_ctx,
                                                  const std::atomic<bool>* cancel_flag);

// Task array; per-task regions/config/progress as in UpixGpuTileTask.
UpixEngineStatus upix_engine_process_tiles_gpu_u8_queue(UpixEngine* engine,
                                                        const UpixGpuTileTask* tasks,
                                                        int32_t task_count,
                                                        const std::atomic<bool>* cancel_flag);

// --- diagnostics ------------------------------------------------------------

// Writes a NUL-terminated report into buf (truncated to cap-1). Returns a
// plain status; buf is always NUL-terminated when cap > 0.
UpixEngineStatus upix_engine_debug_blob_scan(UpixEngine* engine, const uint8_t* input,
                                             int32_t width, int32_t height, int32_t channels,
                                             char* buf, size_t cap);

// tiles/sec; 0.0 on failure (C++ semantics).
float upix_engine_benchmark(UpixEngine* engine, int32_t test_tile_size);

// --- getters / setters (all exception-safe; failures yield zeros/false/"" ) --

int32_t upix_engine_current_device(UpixEngine* engine);   // UPIX_DEVICE_*
int32_t upix_engine_is_initialized(UpixEngine* engine);   // 0/1
void upix_engine_model_name(UpixEngine* engine, char* buf, size_t cap);
int32_t upix_engine_get_scale_factor(UpixEngine* engine);
int32_t upix_engine_scale_factor_detected(UpixEngine* engine); // 0/1
int32_t upix_engine_model_input_channels(UpixEngine* engine);
void upix_engine_set_scale_factor(UpixEngine* engine, int32_t scale);
void upix_engine_set_inference_threads(UpixEngine* engine, int32_t threads);
int32_t upix_engine_get_recommended_tile_size(UpixEngine* engine);
uint64_t upix_engine_get_device_memory_budget_mb(UpixEngine* engine);
// (total_mb, available_mb); zeros on failure.
void upix_engine_get_memory_info(UpixEngine* engine, uint64_t* total_mb, uint64_t* available_mb);
void upix_engine_hardware_id(UpixEngine* engine, char* buf, size_t cap);

int32_t upix_engine_supports_gpu_tile_pipeline(UpixEngine* engine);        // 0/1
int32_t upix_engine_supports_gpu_tile_pipeline_u8(UpixEngine* engine);     // 0/1
int32_t upix_engine_supports_gpu_tile_pipeline_u8_queue(UpixEngine* engine); // 0/1

#ifdef __cplusplus
} // extern "C"
#endif
