#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include "core/engine.hpp"

namespace upix {

class EnginePool {
public:
    EnginePool(size_t pool_size, const std::string& param_path, const std::string& bin_path, DeviceType device_type = DeviceType::GPU_VULKAN);
    ~EnginePool();
    
    // Blocks until an engine is free. Never throws for exhausted pools:
    // callers may run more tasks than engines.
    Engine* acquire();
    // Returns an engine to the pool. Foreign / null pointers and duplicate
    // releases are rejected with a warning (they would hand one engine to two
    // workers at once) instead of corrupting the free list.
    void release(Engine* engine);
    
    // Non-copyable
    EnginePool(const EnginePool&) = delete;
    EnginePool& operator=(const EnginePool&) = delete;
    
 private:
    std::vector<std::unique_ptr<Engine>> engines_;
    // Free list; a vector rather than a queue so release() can scan for
    // duplicates.
    std::vector<Engine*> available_;
    std::mutex mutex_;
    std::condition_variable available_cv_;
};

} // namespace upix
