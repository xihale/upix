#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include "core/engine.hpp"

namespace crossscale {

class EnginePool {
public:
    EnginePool(size_t pool_size, const std::string& param_path, const std::string& bin_path, DeviceType device_type = DeviceType::GPU_VULKAN);
    ~EnginePool();
    
    Engine* acquire();
    void release(Engine* engine);
    
    // Non-copyable
    EnginePool(const EnginePool&) = delete;
    EnginePool& operator=(const EnginePool&) = delete;
    
 private:
    std::vector<std::unique_ptr<Engine>> engines_;
    std::queue<Engine*> available_;
    std::mutex mutex_;
};

} // namespace crossscale
