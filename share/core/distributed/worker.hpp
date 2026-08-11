#pragma once

#include "protocol.hpp"
#include "scheduler.hpp"
#include "crypto_session.hpp"
#include "identity.hpp"
#include "core/model_manager.hpp"
#include "utils/expected.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <filesystem>

namespace crossscale {

class TCPSocket;
class Engine;

struct WorkerError {
    int code;
    std::string message;
};

class Worker {
public:
    Worker();
    ~Worker();

    // Drops the plaintext auth_key. The caller supplies the coordinator's
    // long-term Ed25519 identity pubkey (used to detect MITM during the
    // signature handshake) and this worker's own IdentityKey (signs the
    // transcript).
    auto connect(const std::string& host, uint16_t port,
                 const std::array<uint8_t, 32>& peer_identity_pubkey,
                 IdentityKey& local_identity)
        -> expected<void, WorkerError>;

    auto handshake(Engine& engine) -> expected<void, WorkerError>;

    void disconnect();
    bool is_connected() const;

    auto send_profile(const DeviceProfile& profile) -> expected<void, WorkerError>;
    auto receive_task() -> expected<Task, WorkerError>;

    auto execute_task(
        const Task& task,
        Engine& engine,
        std::function<void(float)> progress_callback = {}
    ) -> expected<std::vector<uint8_t>, WorkerError>;

    auto submit_results(const std::vector<uint8_t>& results)
        -> expected<void, WorkerError>;

    void set_recommended_weight(float weight);
    void set_queue_depth(size_t queue_depth);
    void set_recommended_tile_size(int tile_size);

    void start_heartbeat(std::chrono::seconds interval = std::chrono::seconds(5));
    void stop_heartbeat();

    auto device_id() const -> const std::string& { return device_id_; }
    auto& on_task_received() { return on_task_received_; }
    auto& on_progress() { return on_progress_; }

    void set_models_directory(const std::filesystem::path& dir);
    auto sync_models() -> expected<void, WorkerError>;

private:
    void heartbeat_loop(std::chrono::seconds interval);

    void send_progress_update(const std::string& task_id, float progress);

    auto serialize_processed_tiles(const std::vector<ProcessedTile>& tiles)
        -> std::vector<uint8_t>;

    auto download_model(const std::string& model_name, uint8_t file_type)
        -> expected<void, WorkerError>;

    // Send a single encrypted frame (header.type + payload) over session_.
    // Returns false on socket failure. seal() throws if session is inactive —
    // callers must only use this after handshake() activates the session.
    auto send_encrypted(MessageType type, const void* payload, size_t len)
        -> expected<void, WorkerError>;

    // Receive a single encrypted frame and open it. Returns the plaintext
    // header + payload, or an error if the socket read fails, the header is
    // malformed, or the MAC fails. nullopt-equivalent errors carry code 5/6.
    auto recv_encrypted()
        -> expected<std::pair<MessageHeader, std::vector<uint8_t>>, WorkerError>;

    // 数据流变体:大 payload(tile/result)按 session 的 stream_plaintext 设定
    // 决定是否加密。控制帧仍用 send_encrypted/recv_encrypted。
    auto send_stream(MessageType type, const void* payload, size_t len)
        -> expected<void, WorkerError>;
    auto recv_stream()
        -> expected<std::pair<MessageHeader, std::vector<uint8_t>>, WorkerError>;

    std::unique_ptr<TCPSocket> socket_;
    std::string device_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> heartbeat_running_{false};
    std::thread heartbeat_thread_;

    // Encrypted handshake state.
    EncryptedSession session_;
    IdentityKey* local_identity_ = nullptr;
    std::array<uint8_t, 32> peer_id_pub_{};
    bool peer_trusted_ = false;

    float recommended_weight_override_ = 0.0f;
    size_t queue_depth_override_ = 0;
    int recommended_tile_size_override_ = 0;

    ModelManager model_manager_;

    std::function<void(const Task&)> on_task_received_;
    std::function<void(float)> on_progress_;
};

}
