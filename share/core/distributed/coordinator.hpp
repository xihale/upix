#pragma once

#include "scheduler.hpp"
#include "protocol.hpp"
#include "core/model_manager.hpp"
#include "core/config.hpp"
#include "utils/expected.hpp"
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <thread>

namespace crossscale {

class TCPServer;
class TCPSocket;
class Engine;

struct CoordinatorError {
    int code;
    std::string message;
};

struct WorkerConnection {
    std::unique_ptr<TCPSocket> socket;
    DeviceProfile profile;
    std::string device_id;
    bool authenticated = false;
};

struct ActiveTask {
    std::string task_id;
    std::string worker_id;
    std::vector<uint32_t> tile_indices;
    bool completed = false;
    std::chrono::steady_clock::time_point dispatched_at;
    // Tile pixel-data batch, sent only after the worker accepts the task and
    // (if needed) fetches the model. Keeping this deferred avoids a protocol
    // desync: the worker can issue MSG_MODEL_REQUEST between ACCEPT and the
    // batch without colliding with in-flight TILES_BATCH bytes.
    std::vector<uint8_t> pending_batch;
    bool batch_sent = false;
};

class Coordinator {
public:
    Coordinator();
    ~Coordinator();

    auto start(uint16_t port, const std::string& auth_key = "")
        -> expected<void, CoordinatorError>;

    void stop();
    bool is_running() const;

    auto get_connected_devices() -> std::vector<DeviceProfile>;
    auto device_count() const -> size_t;

    auto distribute_task(
        const std::vector<uint8_t>& image_data,
        int width,
        int height,
        int channels,
        const std::string& model_name,
        int scale_factor,
        const ProcessingConfig& config
    ) -> expected<std::vector<uint8_t>, CoordinatorError>;

    void cancel_current_task();

    auto& on_device_connected() { return on_device_connected_; }
    auto& on_device_disconnected() { return on_device_disconnected_; }
    auto& on_task_progress() { return on_task_progress_; }

    void set_models_directory(const std::filesystem::path& dir);

private:
    void accept_loop();
    void handle_client(std::unique_ptr<TCPSocket> socket);
    void process_messages(WorkerConnection& conn);

    bool authenticate(WorkerConnection& conn, const std::string& received_key);
    void send_model_list(WorkerConnection& conn);

    auto handle_model_request(WorkerConnection& conn, const std::vector<uint8_t>& payload)
        -> expected<void, CoordinatorError>;

    auto deserialize_results(const std::vector<uint8_t>& data)
        -> std::vector<ProcessedTile>;

    auto stitch_tiles(const std::vector<ProcessedTile>& tiles,
                      int original_width, int original_height,
                      int channels, int scale_factor)
        -> expected<std::vector<uint8_t>, CoordinatorError>;

    std::unique_ptr<TCPServer> server_;
    Scheduler scheduler_;
    std::unordered_map<std::string, WorkerConnection> workers_;
    std::mutex workers_mutex_;
    std::thread accept_thread_;

    std::vector<std::thread> client_threads_;
    std::mutex client_threads_mutex_;

    ModelManager model_manager_;

    std::string auth_key_;
    std::string server_name_;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> task_cancelled_{false};

    std::function<void(const DeviceProfile&)> on_device_connected_;
    std::function<void(const std::string&)> on_device_disconnected_;
    std::function<void(float)> on_task_progress_;
};

}
