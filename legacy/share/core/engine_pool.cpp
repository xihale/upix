#include "engine_pool.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace upix {

namespace {

// Physical (non-SMT) core count. On SMT/hybrid CPUs, ncnn's default of one
// thread per logical CPU oversubscribes badly when several engines run
// concurrently; the pool divides this count across its engines.
int detect_physical_core_count() {
#if defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::set<std::pair<int, int>> cores;
        std::string line;
        int physical_id = -1;
        while (std::getline(cpuinfo, line)) {
            if (line.starts_with("physical id")) {
                const auto pos = line.find(':');
                if (pos != std::string::npos) {
                    physical_id = std::atoi(line.c_str() + pos + 1);
                }
            } else if (line.starts_with("core id")) {
                const auto pos = line.find(':');
                if (pos != std::string::npos && physical_id >= 0) {
                    cores.emplace(physical_id, std::atoi(line.c_str() + pos + 1));
                }
            }
        }
        if (!cores.empty()) {
            return static_cast<int>(cores.size());
        }
    }
#endif
    const unsigned int logical = std::thread::hardware_concurrency();
    return logical > 0 ? static_cast<int>(logical) : 1;
}

} // namespace

EnginePool::EnginePool(size_t pool_size, const std::string& param_path, const std::string& bin_path, DeviceType device_type) {
    if (pool_size == 0) {
        throw std::invalid_argument("EnginePool size cannot be zero");
    }

    // CPU engines split the machine's physical cores so that
    // pool_size * threads_per_engine ~= physical cores (ncnn guidance: at
    // most 8 threads per net). Without this, every engine spawns one thread
    // per logical CPU and N engines oversubscribe the machine N-fold. The
    // budget is computed for GPU pools too: a GPU pool whose Vulkan init
    // fails runs CPU engines, and those need the same cap.
    const int physical_cores = detect_physical_core_count();
    const int engines = static_cast<int>(pool_size);
    int threads_per_engine = std::clamp((physical_cores + engines - 1) / engines, 1, 8);
    if (const char* env_t = std::getenv("UPIX_CPU_THREADS_PER_ENGINE")) {
        try { threads_per_engine = std::clamp(std::stoi(env_t), 1, 16); } catch (...) {}
    }
    if (device_type == DeviceType::CPU) {
        LOG_INFO("EnginePool", "CPU thread budget: {} physical cores / {} engines = {} inference threads per engine",
                 physical_cores, pool_size, threads_per_engine);
    }

    engines_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        auto engine = create_ncnn_engine();

        auto init_res = engine->initialize(device_type);
        if (!init_res.has_value()) {
            LOG_WARNING("EnginePool", "Failed to initialize engine {} with specified device, falling back to CPU", i);
            init_res = engine->initialize(DeviceType::CPU);
            if (!init_res.has_value()) {
                throw EngineError{1, std::format("Failed to initialize engine {}", i)};
            }
        }

        // Keyed on the device the engine actually came up on, not the requested
        // one: a GPU pool whose Vulkan init failed falls back to CPU, and those
        // engines would otherwise keep ncnn's one-thread-per-logical-CPU
        // default — the N-fold oversubscription this budget exists to prevent.
        if (engine->current_device() == DeviceType::CPU) {
            engine->set_inference_threads(threads_per_engine);
        }

        // Load model
        auto load_res = engine->load_model(param_path, bin_path);
        if (!load_res.has_value()) {
            throw EngineError{2, std::format("Failed to load model in engine {}: {}", i, load_res.error().message)};
        }

        engines_.push_back(std::move(engine));
        available_.push_back(engines_.back().get());
    }

    LOG_INFO("EnginePool", "Created pool with {} engines on {}",
             pool_size, device_type == DeviceType::GPU_VULKAN ? "GPU" : "CPU");
}

EnginePool::~EnginePool() = default;

Engine* EnginePool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    // Wait for a free engine instead of throwing: callers are allowed to have
    // more concurrent tasks than engines (e.g. a worker pool racing acquire).
    available_cv_.wait(lock, [this] { return !available_.empty(); });
    Engine* e = available_.back();
    available_.pop_back();
    return e;
}

void EnginePool::release(Engine* engine) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only pool-owned engines, and only once: an unchecked push lets a
        // caller bug (double release, foreign or null pointer) grow the free
        // list past pool_size, and acquire() then hands the same engine to two
        // workers at once — two threads inside one ncnn::Net. The free list is
        // a vector (a handful of entries) so the duplicate scan is cheap.
        const bool owned = engine != nullptr &&
            std::any_of(engines_.begin(), engines_.end(),
                        [engine](const std::unique_ptr<Engine>& e) { return e.get() == engine; });
        if (!owned) {
            LOG_WARNING("EnginePool", "release() called with an engine that is not in this pool; ignoring");
            return;
        }
        if (std::find(available_.begin(), available_.end(), engine) != available_.end()) {
            LOG_WARNING("EnginePool", "release() called twice for the same engine; ignoring");
            return;
        }
        available_.push_back(engine);
    }
    available_cv_.notify_one();
}

} // namespace upix
