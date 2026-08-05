#pragma once

#include "scheduler.hpp"
#include "protocol.hpp"
#include "crypto_session.hpp"
#include "identity.hpp"
#include "core/model_manager.hpp"
#include "core/config.hpp"
#include "utils/expected.hpp"
#include <array>
#include <optional>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <filesystem>

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
    // Activated after the ECDH+signature handshake completes. All worker
    // frames after KEY_HELLO travel through this session.
    EncryptedSession session;
    // The worker's long-term Ed25519 pubkey, learned during the handshake and
    // used for TrustStore lookup (TOFU / known-peer flagging).
    std::array<uint8_t, 32> peer_id_pub{};
    bool peer_trusted = false;
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

    // Drops the plaintext auth_key. The coordinator's long-term Ed25519
    // identity is used to sign the ECDH handshake transcript, and the
    // TrustStore is consulted to flag known vs. first-time (TOFU) workers.
    auto start(uint16_t port, IdentityKey& identity)
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

    // Where to load/save the TrustStore of known worker identity pubkeys.
    // Must be called before start() to take effect.
    void set_trust_store_path(const std::filesystem::path& path);

private:
    void accept_loop();
    void handle_client(std::unique_ptr<TCPSocket> socket);

    // Run the server side of the ECDH+signature handshake on conn. On success
    // conn.session is active and conn.peer_* are populated; returns false on
    // any crypto/socket failure (the caller drops the connection).
    bool run_handshake(WorkerConnection& conn);

    void send_model_list(WorkerConnection& conn);

    auto handle_model_request(WorkerConnection& conn, const std::vector<uint8_t>& payload)
        -> expected<void, CoordinatorError>;

    // Encrypted-frame helpers — mirror Worker's. Send/open a single secretbox
    // frame on conn.session. No-op-safe: seal throws if session is inactive, so
    // these are only valid after run_handshake() succeeds.
    auto send_encrypted(WorkerConnection& conn, MessageType type,
                        const void* payload, size_t len) -> bool;
    auto recv_encrypted(WorkerConnection& conn)
        -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>;

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

    IdentityKey* identity_ = nullptr;
    std::filesystem::path trust_store_path_;
    // Lazily loaded in start(); empty until a path is set. A valid trust store
    // means "only listed peers are trusted"; missing file is treated as empty
    // (TOFU for everyone, flagged untrusted).
    std::optional<TrustStore> trust_store_;

    std::string server_name_;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> task_cancelled_{false};

    std::function<void(const DeviceProfile&)> on_device_connected_;
    std::function<void(const std::string&)> on_device_disconnected_;
    std::function<void(float)> on_task_progress_;
};

}
