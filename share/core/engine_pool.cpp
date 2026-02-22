#include "engine_pool.hpp"
#include "utils/logger.hpp"
#include <stdexcept>
#include <condition_variable>

namespace crossscale {

EnginePool::EnginePool(size_t pool_size, const std::string& param_path, const std::string& bin_path, DeviceType device_type) {
    if (pool_size == 0) {
        throw std::invalid_argument("EnginePool size cannot be zero");
    }
    
    engines_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        auto engine = create_ncnn_engine();
        
        auto init_res = engine->initialize(device_type);
        if (!init_res.has_value()) {
            LOG_WARNING("EnginePool", "Failed to initialize engine " + std::to_string(i) + " with specified device, falling back to CPU");
            init_res = engine->initialize(DeviceType::CPU);
            if (!init_res.has_value()) {
                throw EngineError{1, "Failed to initialize engine " + std::to_string(i)};
            }
        }
        
        // Load model
        auto load_res = engine->load_model(param_path, bin_path);
        if (!load_res.has_value()) {
            throw EngineError{2, "Failed to load model in engine " + std::to_string(i) + ": " + load_res.error().message};
        }
        
        engines_.push_back(std::move(engine));
        available_.push(engines_.back().get());
    }
    
    LOG_INFO("EnginePool", "Created pool with " + std::to_string(pool_size) + " engines on " +
             std::string(device_type == DeviceType::GPU_VULKAN ? "GPU" : "CPU"));
}

EnginePool::~EnginePool() = default;

Engine* EnginePool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (available_.empty()) {
        // Should not happen if we limit concurrency correctly
        throw std::runtime_error("EnginePool: no available engine");
    }
    Engine* e = available_.front();
    available_.pop();
    return e;
}

void EnginePool::release(Engine* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    available_.push(engine);
}

} // namespace crossscale
