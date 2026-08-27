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
#include <deque>
#include <optional>
#include <string>
#include <memory>
#include <utility>
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

// Per-connection state for an authenticated phone client. The phone runs a
// persistent-session submit flow: handshake → CONNECT_REQUEST/RESPONSE →
// (CLIENT_SUBMIT → CLIENT_RESPONSE)* → close. One connection serves a whole
// batch; the handshake/pairing happens once per session, not per image
// (legacy one-shot clients that close after the first response also work —
// the session loop simply sees EOF).
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
    // `cancel_flag` (optional) is wired straight into TileProcessor: setting
    // it aborts inference at the next tile boundary (returns error "canceled"
    // semantics are the caller's business — the flag is only read here).
    auto process_locally(
        const std::vector<uint8_t>& image_data,
        int width,
        int height,
        int channels,
        const std::string& model_name,
        const ProcessingConfig& config,
        const std::string& output_format,
        int output_quality,
        const EncoderOptions& encoder_opts = {},
        std::function<void(float)> progress_sink = nullptr,
        const std::atomic<bool>* cancel_flag = nullptr
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

    // Shared state between the connection handler thread (frame reader) and
    // the per-task worker thread it spawns:
    //   cancel           — set by MSG_TASK_CANCEL, a dead connection (the
    //                      handler's polled read fails, or the worker's
    //                      response send fails) or coordinator stop(); the
    //                      worker aborts inference at the next tile boundary.
    //   connection_lost  — the transport failed mid-task (diagnostics only).
    //   done             — the worker finished its terminal response send.
    //   progress_pct/eta — latest inference progress written by the worker;
    //                      transmitted by the handler, one frame per poll tick.
    // Progress frames used to be sent directly by the worker. That convoyed
    // against the handler's polled reads on StreamSocket's single io_mutex:
    // measured ~10 s per progress send — a 1900x slowdown of the whole
    // upscale (UPIX_PIPELINE_TIMER total vs submit split proved it). The
    // worker now only publishes into these atomics; the handler thread owns
    // every socket write while a task is in flight.
    struct ClientTaskRun {
        std::atomic<bool> cancel{false};
        std::atomic<bool> connection_lost{false};
        std::atomic<bool> done{false};
        std::atomic<int> progress_pct{-1};      // -1 = nothing new to send
        std::atomic<uint32_t> progress_eta{0};
    };

    // A CLIENT_SUBMIT captured off the session stream while a task was still
    // running (submit-ahead). For a chunked submit the struct frame arrives
    // first and its MSG_CLIENT_DATA continuations trickle in during later
    // poll ticks — staged_data parks those bytes so they are not read again
    // from the socket; reassembly finishes eagerly once the current task
    // joins. The whole capture is then processed like any fresh submit.
    struct CapturedSubmit {
        MessageHeader header{};
        std::vector<uint8_t> payload;      // full SUBMIT frame payload (struct [+ inline input])
        std::vector<uint8_t> staged_data;  // DATA continuations consumed mid-task
    };

    // Declared chunked length for a captured submit (0 = legacy single-frame
    // / invalid declaration). DATA continuations are matched against this.
    static uint32_t declared_chunk_len(const CapturedSubmit& cap);

    // Handle one CLIENT_SUBMIT frame: decode input, upscale, encode, send
    // CLIENT_RESPONSE (+CLIENT_DATA continuations). The heavy work runs on a
    // per-task worker thread so this (handler) thread stays free to read
    // frames — that is what makes MSG_TASK_CANCEL reachable while inference
    // is in flight, and also where submit-ahead CLIENT_SUBMITs (+DATA) from
    // the phone get staged into `early_next`. This function blocks until the
    // worker finished (joining it) and returns:
    //   false                  — transport failed / malformed submit; drop.
    //   true + early_next      — 0..N submits captured mid-task (bounded by
    //                              kMaxAheadCaptures; the phone's window is
    //                              2, so >1 is already abnormal yet legal to
    //                              queue rather than drop). The session loop
    //                              drains them before reading fresh frames.
    //   true, empty queue      — loop back to a normal blocking read.
    // Per-task failures are reported inside the response without killing the
    // connection.
    static constexpr size_t kMaxAheadCaptures = 4;
    bool handle_client_submit(WorkerConnection& conn, std::stop_token& st,
                              CapturedSubmit incoming,
                              std::deque<CapturedSubmit>& early_next);

    // Worker body of one CLIENT_SUBMIT (runs on its own std::thread):
    // decode → process_locally (cancel-aware) → terminal response. All
    // mid-task sends (progress, response, DATA continuations) happen here, on
    // this thread, so they never race the handler's reads.
    void run_client_task(WorkerConnection& conn, ClientTaskSubmit task,
                         std::vector<uint8_t> compressed_input,
                         ClientTaskRun& run);

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

    // Polled variant of recv_encrypted for the in-task frame loop: waits at
    // most `poll_ms` for the FIRST byte of a frame, then (once bytes are in)
    // finishes the frame with the full session timeout so a poll tick can
    // never abandon a partially received frame while the peer is alive.
    //   Frame   — out holds the decrypted frame
    //   Timeout — nothing arrived within poll_ms (no bytes consumed)
    //   Error   — transport/protocol failure (drop the connection)
    enum class FramePoll { Frame, Timeout, Error };
    FramePoll recv_encrypted_poll(WorkerConnection& conn, int poll_ms,
                                  std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>& out);

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
