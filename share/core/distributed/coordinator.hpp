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
#include <condition_variable>
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
    // used for lookup of identities explicitly approved by the user.
    std::array<uint8_t, 32> peer_id_pub{};
    bool peer_trusted = false;
};

// A pending pairing request from a phone not yet present in the approved
// TrustStore. The PC's main loop pops these, shows an approval prompt, and resolves them.
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
    // transcript; the TrustStore contains only explicitly approved phones.
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
    // if one is waiting (FIFO), or nullopt. Resolve returned requests with
    // resolve_approval(id, approved). Disconnect or stop cancels the request;
    // resolving a canceled request is harmless. Trusted peers never appear here.
    auto pop_pending_approval() -> std::optional<PendingApproval>;
    void resolve_approval(uint64_t id, bool approved);

private:
    void accept_loop(std::stop_token st);
    // Join and drop connection threads whose handler already returned. Called
    // from accept_loop (its only caller); keeps the vector bounded by live
    // connections instead of by lifetime connection count.
    void reap_finished_clients();
    // Record for one accepted connection; defined below (only the name is
    // needed this early).
    struct ClientThread;

    // Registers a connection handler's live descriptor in its ClientThread
    // record for the whole handler scope and clears it on EVERY exit path
    // (including an exception escaping the session — a hostile all-zero X25519
    // peer key throws from the handshake). stop() only ever shutdown()s a
    // registered descriptor, and the clear runs before the owning TCPSocket is
    // destroyed, so a registration is never left behind pointing at a closed
    // (and possibly recycled) descriptor.
    class SessionWakeRegistration {
    public:
        SessionWakeRegistration(ClientThread& entry, TCPSocket* socket);
        ~SessionWakeRegistration();
        SessionWakeRegistration(const SessionWakeRegistration&) = delete;
        SessionWakeRegistration& operator=(const SessionWakeRegistration&) = delete;

    private:
        ClientThread& entry_;
    };

    // Runs on its own jthread; `st` is checked at phase boundaries so a
    // shutdown request drops the connection instead of serving it. `entry` is
    // the accept-loop record that owns this session: the handler registers its
    // descriptor there (see SessionWakeRegistration) so stop() can wake it out
    // of a blocking socket call.
    void handle_client(std::stop_token st, std::unique_ptr<TCPSocket> socket,
                       ClientThread& entry);

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
        // Submit-order gate for this run's socket writes (progress stage,
        // error replies, final response + DATA chunks). With several tasks in
        // flight a later task can FINISH first — e.g. its predecoded input is
        // corrupt and the decode fails instantly while the previous image is
        // still computing. Responses must still reach the phone in submit
        // order (it reads them FIFO against what it uploaded), so every send
        // blocks until the pump flips send_ready for the oldest unfinished
        // run.
        std::mutex send_order_m;
        std::condition_variable send_order_cv;
        bool send_ready = false;
    };

    // A CLIENT_SUBMIT captured off the session stream while a task was still
    // running (submit-ahead). For a chunked submit the struct frame arrives
    // first and its MSG_CLIENT_DATA continuations trickle in during later
    // poll ticks — staged_data parks those bytes so they are not read again
    // from the socket; reassembly finishes eagerly once the current task
    // joins. The whole capture is then processed like any fresh submit.
    //
    // Submit-ahead prefetch: once the HEAD queued entry's chunks are fully
    // staged while a task is still computing, a helper thread starts decoding
    // it (EXIF → decode → normalize → orientation bake) so the GPU gap
    // between task N and N+1 shrinks by the whole input-prep time. The
    // shared_ptr survives deque spill-swaps; owning the jthread keeps the
    // destructor-join invariant wherever the state dies.
    struct AheadDecode {
        std::mutex mtx;
        std::condition_variable cv;
        bool ready = false;
        bool ok = false;
        std::string error;                 // reply_error text on !ok
        std::vector<uint8_t> pixels;       // normalized u8 image on ok
        int width = 0;
        int height = 0;
        int channels = 0;
        std::unique_ptr<std::jthread> worker;  // joins on destruction
    };

    struct CapturedSubmit {
        MessageHeader header{};
        std::vector<uint8_t> payload;      // full SUBMIT frame payload (struct [+ inline input])
        std::vector<uint8_t> staged_data;  // DATA continuations consumed mid-task
        std::shared_ptr<AheadDecode> ahead;  // set once the head entry is fully staged
    };

    // Declared chunked length for a captured submit (0 = legacy single-frame
    // / invalid declaration). DATA continuations are matched against this.
    static uint32_t declared_chunk_len(const CapturedSubmit& cap);

    // EXIF sniff → decode → channel normalize → orientation bake, the exact
    // input state run_client_task has historically built inline. Shared by
    // the worker's inline path and the submit-ahead prefetch thread so both
    // produce byte-identical inputs. Returns nullopt on success; on failure
    // returns the reply_error text (identical wording to the inline path).
    auto prepare_task_input(
        const ClientTaskSubmit& task,
        const std::vector<uint8_t>& compressed,
        std::vector<uint8_t>& pixels_out,
        int& width_out,
        int& height_out,
        int& channels_out) -> std::optional<std::string>;

    // Spawn the prefetch decoder for a fully-available head entry (its
    // AheadDecode must already be assigned). The jthread is owned by the
    // shared state so it joins deterministically no matter which side —
    // queue teardown or coordinator stop — drops the last reference.
    void start_ahead_decode(CapturedSubmit& cap);

    // Launch conditions for the head-entry prefetch, checked at every point
    // where a queued submit can become fully available: when a legacy/
    // inline SUBMIT frame is armed (complete on arrival) and whenever DATA
    // completions stage a chunked one. Byte count mirrors what the serve
    // path will assemble, so "prefetchable" never disagrees with "serveable".
    void maybe_launch_head_prefetch(std::deque<CapturedSubmit>& q);

    // Session task pump: owns every CLIENT_SUBMIT of a batch once the first
    // one arrives. Heavy work runs on up to kMaxInflightTasks concurrent
    // worker threads while THIS thread keeps polling frames — that is what
    // keeps MSG_TASK_CANCEL reachable, stages submit-ahead traffic, and (the
    // reason it is a multi-worker pump at all) overlaps task N's output
    // encode + result transfer with task N+1's inference. Task N's worker
    // releases engine_mutex_ before encoding, so a queued N+1 worker grabs
    // the GPU the instant inference ends instead of waiting for the encode,
    // the Wi-Fi transfer and the session bookkeeping to unwind too. Exits
    // when everything captured and spawned has been served (control returns
    // to the session loop's normal blocking read — a lull between batches)
    // or by dropping the connection on protocol violations / dead transport.
    // Returns false only when the connection must be dropped.
    //
    // Concurrency contract that keeps this safe:
    //   socket writes  — StreamSocket tx_mutex_ gives whole-frame atomicity,
    //                    and secretbox nonces are random per seal, so workers
    //                    may send their own results concurrently.
    //   GPU            — engine_mutex_ serializes inference; encode runs
    //                    outside the lock precisely so a queued worker can
    //                    take over immediately.
    //   response order — service order equals arrival order (captures are a
    //                    FIFO and a worker is only spawned from the queue
    //                    head), so responses reach the phone in submit order.
    //   progress       — TaskProgress carries no task id; only the OLDEST
    //                    unfinished run may transmit, so the phone never sees
    //                    a newer task's percentages early.
    static constexpr size_t kMaxAheadCaptures = 4;
    static constexpr size_t kMaxInflightTasks = 3;

    // One spawned-but-not-yet-reaped task. Everything except `thread` lives
    // behind a unique_ptr so growing the vector never moves an object the
    // worker references (run atomics are taken by reference inside its body).
    struct LiveTask {
        std::unique_ptr<ClientTaskRun> run{std::make_unique<ClientTaskRun>()};
        std::thread thread;
        LiveTask() = default;
        LiveTask(const LiveTask&) = delete;
        LiveTask& operator=(const LiveTask&) = delete;
    };

    // True when a captured submit has its whole input locally available and
    // can therefore be handed to a worker without touching the socket again.
    bool capture_input_complete(const CapturedSubmit& cap);

    // Copy the compressed input out of a completed capture (chunked staged
    // bytes or legacy inline tail). Never reads the socket: only feed it
    // captures for which capture_input_complete() is true. Returns false with
    // err set for a structurally invalid submit (caller replies a per-task
    // error and keeps the session).
    bool assemble_capture(const CapturedSubmit& cap, ClientTaskSubmit& task_out,
                          std::vector<uint8_t>& input_out, std::string& err);

    bool pump_session_submits(WorkerConnection& conn, std::stop_token& st,
                              CapturedSubmit&& first);

    // Worker body of one CLIENT_SUBMIT (runs on its own std::thread):
    // decode (or await the submit-ahead prefetch) → process_locally
    // (cancel-aware) → terminal response. All mid-task sends (progress,
    // response, DATA continuations) happen here, on this thread, so they
    // never race the handler's reads.
    void run_client_task(WorkerConnection& conn, ClientTaskSubmit task,
                         std::vector<uint8_t> compressed_input,
                         ClientTaskRun& run,
                         std::shared_ptr<AheadDecode> predecoded = nullptr);

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

    // One tracked thread per accepted connection. accept_loop owns the vector
    // (it is the only writer), pushes a record BEFORE starting its thread, and
    // reaps records whose handler already returned: without the reap, a phone
    // that reconnects per image (legacy one-shot clients) would leave one
    // unjoined thread — holding its stack — behind per connection until stop().
    // `done` is written by the connection thread through the stable heap
    // pointer the accept thread waits on. stop() is the only other reader and
    // only after it has joined accept_thread_, which is what freezes the
    // vector for its request_stop/wake/join sweep.
    struct ClientThread {
        std::jthread thread;
        std::atomic<bool> done{false};

        // ── Session wake handle ─────────────────────────────────────────────
        // stop() cannot interrupt a handler parked in a blocking socket call
        // with request_stop() alone: the handler only observes the token
        // between frames, i.e. after its receive timeout (up to
        // SESSION_FRAME_TIMEOUT_MS) expires. So the handler registers its
        // descriptor here for the whole session scope (via
        // SessionWakeRegistration) and stop() shutdown()s whatever is
        // registered. Every access takes `wake_mutex`, and the registration
        // is cleared before the owning socket closes the descriptor — that
        // is what makes shutdown() safe: it can never act on a closed and
        // recycled descriptor number, and never on a destroyed socket.
        std::mutex wake_mutex;
        TCPSocket* session_socket = nullptr;
    };
    std::vector<std::unique_ptr<ClientThread>> client_threads_;

    // Ceiling on simultaneous connections, checked before spawning the handler
    // thread. An unauthenticated peer must not be able to grow the thread/fd
    // count without bound before the handshake gate; the 1:1 design needs one
    // session per phone (plus a reconnect race), so this is far above any
    // legitimate load. The refused socket is closed when its unique_ptr dies.
    static constexpr size_t kMaxConcurrentClients = 8;

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
    // Lazily loaded in start(); empty until a path is set. A missing file
    // leaves every identity untrusted until explicitly approved.
    std::optional<TrustStore> trust_store_;
    std::mutex trust_store_mtx_;

    uint16_t port_ = 0;
    std::atomic<bool> running_{false};

    // Single-flight guard for stop(). The host calls it from its shutdown path
    // and its destructor, and a second caller that entered while the first was
    // mid-teardown (running_ already false, server_ still set) used to
    // re-enter: it then raced the first on the accept-thread join and the
    // client-thread vector. Serializing is also what keeps a second caller
    // from competing for the listener's fd mutex while the accept loop is
    // spinning down.
    std::mutex stop_mutex_;

    std::function<void(float)> on_task_progress_;

    // ── Phone pairing approval state ────────────────────────────────────────
    std::mutex approval_mtx_;
    // Guards registration, resolution, and shutdown; the registry owns every
    // unresolved promise, whether queued or already popped by the UI.
    std::queue<PendingApproval> pending_approvals_;
    std::unordered_map<uint64_t, std::shared_ptr<std::promise<uint8_t>>> approval_futures_;
    uint64_t next_approval_id_ = 1;
};

}
