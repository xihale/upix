#include "scheduler.hpp"
#include <algorithm>
#include <cmath>

namespace crossscale {

void Scheduler::register_device(const DeviceProfile& profile) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    devices_[profile.device_id] = profile;
    normalize_weights();
}

void Scheduler::unregister_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    devices_.erase(device_id);
    normalize_weights();
}

void Scheduler::update_device_performance(const std::string& device_id, 
                                           float actual_tiles_per_second) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    auto it = devices_.find(device_id);
    if (it != devices_.end()) {
        it->second.tiles_per_second = actual_tiles_per_second;
        normalize_weights();
    }
}

auto Scheduler::assign_tiles(uint32_t total_tiles, const std::string& model_name)
    -> std::vector<TaskAssignment> {
    
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    if (devices_.empty()) {
        return {};
    }
    
    std::vector<TaskAssignment> assignments;
    assignments.reserve(devices_.size());
    
    uint32_t assigned_count = 0;
    
    for (const auto& [id, profile] : devices_) {
        uint32_t tiles_for_device = static_cast<uint32_t>(
            std::ceil(total_tiles * profile.weight));
        
        tiles_for_device = std::min(tiles_for_device, total_tiles - assigned_count);
        
        if (tiles_for_device == 0) continue;
        
        TaskAssignment assignment;
        assignment.device_id = id;
        
        for (uint32_t i = 0; i < tiles_for_device; ++i) {
            assignment.assigned_tiles.push_back(assigned_count + i);
        }
        
        if (profile.tiles_per_second > 0) {
            assignment.estimated_time = std::chrono::milliseconds(
                static_cast<int>(tiles_for_device / profile.tiles_per_second * 1000));
        }
        
        assignments.push_back(assignment);
        assigned_count += tiles_for_device;
        
        if (assigned_count >= total_tiles) break;
    }
    
    return assignments;
}

auto Scheduler::handle_device_failure(const std::string& device_id)
    -> std::vector<uint32_t> {
    
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    std::vector<uint32_t> orphaned_tiles;
    
    devices_.erase(device_id);
    normalize_weights();
    
    return orphaned_tiles;
}

auto Scheduler::get_device(const std::string& device_id) const 
    -> std::optional<DeviceProfile> {
    
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    auto it = devices_.find(device_id);
    if (it != devices_.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto Scheduler::get_all_devices() const -> std::vector<DeviceProfile> {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    std::vector<DeviceProfile> result;
    result.reserve(devices_.size());
    
    for (const auto& [id, profile] : devices_) {
        result.push_back(profile);
    }
    
    return result;
}

auto Scheduler::device_count() const -> size_t {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return devices_.size();
}

void Scheduler::normalize_weights() {
    float total_performance = 0.0f;
    
    for (const auto& [id, profile] : devices_) {
        total_performance += profile.tiles_per_second;
    }
    
    if (total_performance > 0) {
        for (auto& [id, profile] : devices_) {
            profile.weight = profile.tiles_per_second / total_performance;
        }
    } else {
        float equal_weight = 1.0f / devices_.size();
        for (auto& [id, profile] : devices_) {
            profile.weight = equal_weight;
        }
    }
}

} // namespace crossscale
