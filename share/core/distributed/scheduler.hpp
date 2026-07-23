#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "core/engine.hpp"

namespace crossscale {

struct DeviceProfile {
    std::string device_id;
    std::string device_name;
    DeviceType type;
    
    float tiles_per_second = 0.0f;
    int recommended_tile_size = 256;
    size_t max_batch_size = 1;
    float weight = 0.0f;
    
    std::string ip_address;
    uint16_t port = 0;
    std::chrono::steady_clock::time_point last_heartbeat;
    
    bool is_alive() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat);
        return elapsed.count() < 30;
    }
};

struct Task {
    std::string task_id;
    std::string model_name;
    std::vector<uint32_t> tile_indices;
};

struct TaskAssignment {
    std::string device_id;
    std::vector<uint32_t> assigned_tiles;
    std::chrono::milliseconds estimated_time;
};

struct ProcessedTile {
    uint32_t tile_x;
    uint32_t tile_y;
    uint32_t original_width;
    uint32_t original_height;
    uint32_t output_width;
    uint32_t output_height;
    uint8_t channels;
    std::vector<uint8_t> data;
};

class Scheduler {
public:
    void register_device(const DeviceProfile& profile);
    void unregister_device(const std::string& device_id);

    auto assign_tiles(uint32_t total_tiles, const std::string& model_name)
        -> std::vector<TaskAssignment>;

    auto get_all_devices() const -> std::vector<DeviceProfile>;
    auto device_count() const -> size_t;

private:
    void normalize_weights();

    std::unordered_map<std::string, DeviceProfile> devices_;
    mutable std::mutex devices_mutex_;
};

}
