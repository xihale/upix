#pragma once

#include "protocol.hpp"
#include "crypto_session.hpp"
#include "identity.hpp"
#include "core/engine.hpp"
#include "core/model_manager.hpp"
#include "core/tile_processor.hpp"
#include "core/config.hpp"
#include "utils/expected.hpp"
#include <array>
#include <optional>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <queue>
#include <future>
#include <thread>
#include <filesystem>
#include <unordered_map>

namespace upix {

class TCPServer;
class TCPSocket;

struct CoordinatorError {
    int code;
    std::string message;
};

// Per-connection state for an authenticated phone client. The phone runs the
// one-shot submit flow: handshake → CONNECT_REQUEST/RESPONSE → CLIENT_SUBMIT →
// CLIENT_RESPONSE → close. Each image opens a fresh connection that re-handshakes
// using the persisted peer key (strict MITM); the PC auto-approves trusted peers.
struct WorkerConnection {
    std::unique_ptr<TCPSocket> socket;
    std::string device_id;
    bool authenticated = false;
    // Activated after the ECDH+signature handshake completes. All frames after
    // KEY_HELLO travel through this session.
    EncryptedSession session;
    // The phone's long-term Ed25519 pubkey, learned during the handshake and
    // used for TrustStore lookup (TOFU / known-peer flagging).
    std::array<uint8_t, 32> peer_id_pub{};
    bool peer_trusted = false;
};

// A pending pairing request from a phone (first-time, not yet trusted).
// The PC's main loop pops these, shows an approval prompt, and resolves them.
struct PendingApproval {
    uint64_t id = 0;                       // monotonic id for resolve()
    std::string device_name;               // human-readable, from the request
    std::string fingerprint;               // 16-char hex of the phone's identity
    std::array<uint8_t, 32> identity_pub;  // to add to TrustStore on approval
    // Resolution channel: set by resolve_approval(), read by the blocked handler.
    std::shared_ptr<std::promise<uint8_t>> verdict;  // 0=denied, 1=approved
};

// The PC-side coordinator. In the 1:1 model it accepts a phone connection,
// runs the encrypted handshake + pairing gate, and upscales each submitted
// image on the PC's OWN engine (GPU, CPU fallback). There is no multi-worker
// dispatch — the phone is a submitter, not a compute worker.
class Coordinator {
public:
    Coordinator();
    ~Coordinator();

    // The coordinator's long-term Ed25519 identity signs the ECDH handshake
    // transcript; the TrustStore flags known vs. first-time (TOFU) phones.
    auto start(uint16_t port, IdentityKey& identity)
        -> expected<void, CoordinatorError>;

    void stop();
    bool is_running() const;

    // Upscale a submitted image on the PC's local engine. `image_data` is raw
    // u8 pixels (width*height*channels). Replaces the old multi-worker
    // distribute_task — the PC does all the work itself now.
    auto process_locally(
        const std::vector<uint8_t>& image_data,
        int width,
        int height,
        int channels,
        const std::string& model_name,
        const ProcessingConfig& config,
        const std::string& output_format,
        int output_quality,
        std::function<void(float)> progress_sink = nullptr
    ) -> expected<std::vector<uint8_t>, CoordinatorError>;

    auto& on_task_progress() { return on_task_progress_; }

    void set_models_directory(const std::filesystem::path& dir);

    // Where to load/save the TrustStore of known phone identity pubkeys.
    // Must be called before start() to take effect.
    void set_trust_store_path(const std::filesystem::path& path);

    // ── Phone pairing approval (request/allow flow) ─────────────────────────
    // Called by the PC main loop. Returns a pending first-time pairing request
    // if one is waiting (FIFO), or nullopt. Each returned request must later be
    // resolved by resolve_approval(id, approved) — the requesting phone blocks
    // on that resolution. Already-trusted peers never appear here.
    auto pop_pending_approval() -> std::optional<PendingApproval>;
    void resolve_approval(uint64_t id, bool approved);

private:
    void accept_loop(std::stop_token st);
    // Runs on its own jthread; `st` is checked at phase boundaries so a
    // shutdown request drops the connection instead of serving it.
    void handle_client(std::stop_token st, std::unique_ptr<TCPSocket> socket);

    // Run the server side of the ECDH+signature handshake on conn. On success
    // conn.session is active and conn.peer_* are populated; returns false on
    // any crypto/socket failure (the caller drops the connection).
    bool run_handshake(WorkerConnection& conn);

    // Lazily create + initialize the local engine (GPU → CPU fallback) and load
    // `model_name` if it isn't already loaded. Idempotent. Returns false on
    // init/load failure (engine_mutex_ NOT held — acquires it internally).
    bool ensure_engine_ready(const std::string& model_name);

    // Encrypted-frame helpers. Send/open a single secretbox frame on
    // conn.session. Only valid after run_handshake() succeeds.
    auto send_encrypted(WorkerConnection& conn, MessageType type,
                        const void* payload, size_t len) -> bool;
    auto recv_encrypted(WorkerConnection& conn)
        -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>;

    // ── Shared model library ops (phone↔PC) ─────────────────────────────────
    // Each handles one authenticated role frame and sends the reply. All guard
    // model_manager_ with models_mutex_ (inference reads it via switch_model
    // under engine_mutex_; the two must not interleave on the vector).
    void handle_model_list(WorkerConnection& conn);
    void handle_model_upload(WorkerConnection& conn,
                             const MessageHeader& hdr, const std::vector<uint8_t>& payload);
    void handle_model_download(WorkerConnection& conn,
                               const MessageHeader& hdr, const std::vector<uint8_t>& payload);

    std::unique_ptr<TCPServer> server_;
    std::jthread accept_thread_;

    // One jthread per accepted connection. Only accept_loop pushes and only
    // stop() joins/clears (after accept_thread_ is joined), so no lock is
    // needed. Finished jthreads stay joinable in the vector until stop()
    // reaps them — same retention as the old (dead-code) sweep, but stop()
    // now frees them deterministically.
    std::vector<std::jthread> client_threads_;

    // Local inference engine + tile processor (lazy-init on first submit, then
    // kept warm across requests — mirrors share/core/serve/daemon.cpp).
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<TileProcessor> tile_processor_;
    std::mutex engine_mutex_;
    std::string loaded_model_name_;
    bool engine_initialized_ = false;

    ModelManager model_manager_;
    // Guards model_manager_ scans/mutations against concurrent model-library
    // ops from other phone connections.
    std::mutex models_mutex_;

    IdentityKey* identity_ = nullptr;
    std::filesystem::path trust_store_path_;
    // Lazily loaded in start(); empty until a path is set. A valid trust store
    // means "only listed peers are trusted"; missing file is treated as empty
    // (TOFU for everyone, flagged untrusted).
    std::optional<TrustStore> trust_store_;

    uint16_t port_ = 0;
    std::atomic<bool> running_{false};

    std::function<void(float)> on_task_progress_;

    // ── Phone pairing approval state ────────────────────────────────────────
    std::mutex approval_mtx_;
    std::queue<PendingApproval> pending_approvals_;
    std::unordered_map<uint64_t, std::shared_ptr<std::promise<uint8_t>>> approval_futures_;
    uint64_t next_approval_id_ = 1;
};

}
