#include "coordinator.hpp"
#include "core/tile_types.hpp"
#include "core/tile_optimizer.hpp"
#include "core/engine_setup.hpp"
#include "utils/image.hpp"
#include "utils/image_avif.hpp"
#include "utils/exif.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include "utils/crypto_sodium.hpp"
#include "utils/sr_marker.hpp"
#include <system_error>
#include <thread>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <optional>

namespace upix {

// Raw upscaled pixels are a transient buffer (encoded to jpg/png before
// transport), so they may exceed MAX_NETWORK_PAYLOAD (utils/network.hpp).
// Bound PC memory instead.
static constexpr uint64_t MAX_RAW_OUTPUT_BYTES = 512ULL * 1024 * 1024;

// Idle timeout between frames of a persistent phone session: submit N+1
// only leaves the phone after output N has been written to SAF and the
// checkpoint saved, so the gap must tolerate file IO, not just framing.
static constexpr int SESSION_FRAME_TIMEOUT_MS = 60 * 1000;

// Deadline for the whole pre-session phase of an accepted connection: waiting
// for the first frame, the ECDH handshake, and the pairing request. Accepted
// sockets carry no timeout of their own, so without this a peer that connects
// and never writes pins a handler thread in a blocking recv() forever — the
// accept loop would spawn threads without bound, and stop() (which joins the
// handlers) would never return. Ten seconds is far beyond any legitimate
// exchange on a LAN; the session then installs its own longer timeouts.
static constexpr int HANDSHAKE_TIMEOUT_MS = 10 * 1000;

// While a task runs on its worker thread, the connection handler polls for
// new frames with this receive timeout instead of blocking for the full
// session timeout — a MSG_TASK_CANCEL (or a dead peer, or a stop() request)
// is then noticed within one tick, and the io lock is released between
// polls so the worker's progress/response sends can interleave.
static constexpr int TASK_FRAME_POLL_MS = 200;

// Human-readable byte count for logs: "6.66 MB", "512.0 KB", "723 bytes".
static std::string format_bytes(uint64_t bytes) {
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = 1024 * KB;
    constexpr uint64_t GB = 1024 * MB;
    const double b = static_cast<double>(bytes);
    if (bytes >= GB) {
        return std::format("{:.2f} GB", b / static_cast<double>(GB));
    }
    if (bytes >= MB) {
        return std::format("{:.2f} MB", b / static_cast<double>(MB));
    }
    if (bytes >= KB) {
        return std::format("{:.1f} KB", b / static_cast<double>(KB));
    }
    return std::format("{} bytes", bytes);
}

// Human-readable duration for logs: "340 ms" or "3.21 s".
static std::string format_seconds(double s) {
    if (s < 1.0) {
        return std::format("{:.0f} ms", s * 1000.0);
    }
    return std::format("{:.2f} s", s);
}

// Wire structs carry fixed-width char[] fields that the peer controls and that
// are NOT guaranteed NUL-terminated. Constructing std::string straight from the
// array scans past the field — and, for a trailing field, past the struct
// itself (a stack over-read whose bytes can then be echoed back to the peer).
// Copy with an explicit bound instead; the serve daemon uses the same rule.
static std::string bounded_string(const char* src, size_t capacity) {
    return {src, ::strnlen(src, capacity)};  // POSIX: not in std:: on glibc
}

// Replace `path` with exactly `size` bytes from `data`. Callers stage into a
// temp path and rename, so a short write can never masquerade as a complete
// file. Returns false on open/write/close failure.
static bool write_file(const std::filesystem::path& path, const uint8_t* data,
                       size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
}

Coordinator::Coordinator() = default;

Coordinator::~Coordinator() {
    stop();
}

auto Coordinator::pop_pending_approval() -> std::optional<PendingApproval> {
    std::lock_guard<std::mutex> lock(approval_mtx_);
    while (!pending_approvals_.empty()) {
        PendingApproval req = std::move(pending_approvals_.front());
        pending_approvals_.pop();
        // Disconnected or already-resolved requests must not re-enter the UI.
        if (approval_futures_.contains(req.id)) {
            return req;
        }
    }
    return std::nullopt;
}

void Coordinator::resolve_approval(uint64_t id, bool approved) {
    std::lock_guard<std::mutex> lock(approval_mtx_);
    auto it = approval_futures_.find(id);
    if (it == approval_futures_.end()) {
        return;  // stale, disconnected, or canceled by stop()
    }
    // Fulfill and erase under the same lock as shutdown and registration.
    it->second->set_value(approved && running_ ? 1 : 0);
    approval_futures_.erase(it);
}

auto Coordinator::start(uint16_t port, IdentityKey& identity)
    -> expected<void, CoordinatorError> {

    if (running_) {
        return {};
    }

    identity_ = &identity;
    sodium::init();

    // Load explicitly approved peers. Missing stores leave every identity
    // unapproved until the user accepts a CONNECT_REQUEST.
    if (!trust_store_path_.empty()) {
        std::lock_guard<std::mutex> lock(trust_store_mtx_);
        trust_store_ = TrustStore::load(trust_store_path_);
        LOG_INFO("Coordinator", "TrustStore loaded ({} known peers) from {}",
                 trust_store_->size(), trust_store_path_.string());
    }

    server_ = std::make_unique<TCPServer>();

    auto result = server_->listen(port);
    if (!result.has_value()) {
        return std::unexpected(CoordinatorError{1, std::format("Failed to start server: {}", result.error().message)});
    }

    port_ = port;
    running_ = true;
    // jthread move-assignment stops + joins any previous accept thread.
    accept_thread_ = std::jthread([this](std::stop_token st) { accept_loop(st); });

    LOG_INFO("Coordinator", "Server started on port {}", port_);
    return {};
}

void Coordinator::set_trust_store_path(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(trust_store_mtx_);
    trust_store_path_ = path;
    trust_store_.reset();
}

auto Coordinator::trusted_devices() const -> std::vector<TrustedDeviceInfo> {
    std::lock_guard<std::mutex> lock(trust_store_mtx_);
    if (!trust_store_ && !trust_store_path_.empty()) {
        trust_store_ = TrustStore::load(trust_store_path_);
    }
    if (!trust_store_) return {};
    return trust_store_->trusted_devices();
}

auto Coordinator::remove_trusted_device(const std::string& fingerprint) -> bool {
    std::lock_guard<std::mutex> lock(trust_store_mtx_);
    if (!trust_store_ && !trust_store_path_.empty()) {
        trust_store_ = TrustStore::load(trust_store_path_);
    }
    if (!trust_store_) return false;
    if (!trust_store_->remove_by_fingerprint(fingerprint)) {
        return false;
    }
    std::error_code ec;
    if (!trust_store_->save(ec)) {
        LOG_WARNING("Coordinator", "Failed to save TrustStore after remove: {}",
                    ec.message());
    }
    return true;
}

void Coordinator::clear_trusted_devices() {
    std::lock_guard<std::mutex> lock(trust_store_mtx_);
    if (!trust_store_ && !trust_store_path_.empty()) {
        trust_store_ = TrustStore::load(trust_store_path_);
    }
    if (!trust_store_) return;
    trust_store_->clear();
    std::error_code ec;
    if (!trust_store_->save(ec)) {
        LOG_WARNING("Coordinator", "Failed to save TrustStore after clear: {}",
                    ec.message());
    }
}

void Coordinator::stop() {
    // Serialize concurrent callers (see stop_mutex_): the early-out below is
    // only sound once a caller owns the whole teardown.
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    if (!running_ && !server_) {
        return;
    }

    LOG_INFO("Coordinator", "Stopping coordinator...");
    {
        std::lock_guard<std::mutex> lock(approval_mtx_);
        running_ = false;
        // The registry includes requests still queued and those held by the UI.
        // Closing registration before waking waiters prevents orphan promises.
        for (auto& [id, verdict] : approval_futures_) {
            verdict->set_value(0);
        }
        approval_futures_.clear();
        pending_approvals_ = {};
    }

    // Shutdown ordering matters:
    //   1. Request the accept loop's stop FIRST, before anything touches the
    //      listener. The loop re-checks the token at the top of every
    //      iteration, so the request alone ends it within one 500 ms poll
    //      tick. With the previous order (close the listener, then request)
    //      this thread had to win TCPServer's fd mutex against a loop that
    //      re-acquires it every tick, and a listener closed under that loop
    //      makes accept() return immediately instead of on a poll tick — the
    //      loop then spins without ever sleeping and holds the mutex almost
    //      continuously. On a real host that showed up as one stop() blocked
    //      for 48 s and a second caller (which entered while the first was
    //      still tearing down, so server_ was not null yet) never returning.
    //   2. Then join the accept thread; its loop observes the request token on
    //      every poll-tick wake.
    //   3. Only then destroy the server object: accept_loop still dereferences
    //      it on its final wake (ASAN proved a use-after-free when reset() ran
    //      before the join), and with the join done no thread can be inside
    //      accept() holding its fd mutex, so the close cannot starve anyone.
    //   4. Then stop+join the client handler threads. request_stop() alone is
    //      not enough: a handler parked in recv() on a peer that went silent —
    //      one that connected and never sent its first frame, or an
    //      authenticated one that went quiet between frames — only notices the
    //      token after its receive timeout, up to SESSION_FRAME_TIMEOUT_MS
    //      (the 1-minute session window). shutdown() on the descriptor each
    //      handler registered for exactly this purpose wakes those calls
    //      immediately. Pending pairing waits were already rejected above.
    if (accept_thread_.joinable()) {
        accept_thread_.request_stop();
        accept_thread_.join();
    }

    server_.reset();

    for (auto& t : client_threads_) {
        t->thread.request_stop();
    }
    // Wake after every token is requested: a handler that has not registered
    // its descriptor yet then sees the token on its post-registration check
    // and drops the connection itself, while one already waiting is woken by
    // the shutdown. Either way no handler is left waiting out a timeout.
    for (auto& t : client_threads_) {
        std::lock_guard<std::mutex> wake_lock(t->wake_mutex);
        if (t->session_socket != nullptr) {
            t->session_socket->shutdown();
        }
    }
    for (auto& t : client_threads_) {
        if (t->thread.joinable()) {
            t->thread.join();
        }
    }
    client_threads_.clear();

    LOG_INFO("Coordinator", "Coordinator stopped");
}

bool Coordinator::is_running() const {
    return running_;
}

// Lazily create + initialize the local engine (GPU → CPU fallback) and ensure
// `model_name` is loaded. Idempotent. Mirrors share/core/serve/daemon.cpp.
bool Coordinator::ensure_engine_ready(const std::string& model_name) {
    std::lock_guard<std::mutex> lk(engine_mutex_);

    if (!engine_initialized_) {
        auto init = initialize_engine(SetupDevice::GPU_THEN_CPU);
        if (!init) {
            LOG_ERROR("Coordinator", init.error());
            return false;
        }
        engine_ = std::move(*init);
        tile_processor_ = std::make_unique<TileProcessor>();
        engine_initialized_ = true;
        LOG_INFO("Coordinator", "Local engine ready on " +
                 std::string(engine_->current_device() == DeviceType::GPU_VULKAN ? "GPU" : "CPU"));
    }

    return switch_model(*engine_, model_manager_, loaded_model_name_, model_name, "Coordinator");
}

// Upscale a submitted image on the PC's own engine. image_data is raw u8 pixels.
// Replaces the old multi-worker distribute_task — no tile dispatch, no workers.
auto Coordinator::process_locally(const std::vector<uint8_t>& image_data,
                                  int width, int height, int channels,
                                  const std::string& model_name,
                                  const ProcessingConfig& config,
                                  const std::string& output_format,
                                  int output_quality,
                                  const EncoderOptions& encoder_opts,
                                  std::function<void(float)> progress_sink,
                                  const std::atomic<bool>* cancel_flag)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    const auto canceled = [cancel_flag] {
        return cancel_flag != nullptr && cancel_flag->load(std::memory_order_relaxed);
    };

    if (!ensure_engine_ready(model_name)) {
        return std::unexpected(CoordinatorError{2, "Engine/model not ready: " + model_name});
    }
    if (canceled()) {
        return std::unexpected(CoordinatorError{7, "canceled"});
    }

    // Inference is serialized under engine_mutex_ (Vulkan is not concurrency-safe).
    // The post-inference encode deliberately runs OUTSIDE the lock: it is pure
    // CPU over local buffers and can take seconds on large outputs — holding
    // the lock through it would stall other sessions' ensure_engine_ready().
    int scale = 1;
    int out_w = 0;
    int out_h = 0;
    std::vector<uint8_t> out_bytes;
    double inf_s = 0.0;
    std::string inference_error;
    // Effective model name, snapshotted under engine_mutex_: loaded_model_name_
    // may be reassigned by another session's switch_model() (it is only
    // serialized by that mutex), and it is read again below for the SR marker.
    std::string effective_model;
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        // Re-check after the (possibly long) engine lock wait: another
        // connection's task may have run while this one queued behind it,
        // and the cancel may have landed during that wait.
        if (canceled()) {
            return std::unexpected(CoordinatorError{7, "canceled"});
        }
        scale = engine_->get_scale_factor();
        out_w = width * scale;
        out_h = height * scale;
        const size_t out_bytes_n = static_cast<size_t>(out_w) * out_h * channels;
        if (out_w <= 0 || out_h <= 0 || out_bytes_n > MAX_RAW_OUTPUT_BYTES) {
            return std::unexpected(CoordinatorError{3, "Invalid output dimensions"});
        }

        // tile_size 0 = remote auto: the phone did not pin a size, so the PC
        // picks one with the same TileSizeOptimizer probe the CLI's
        // --auto-tile uses. The per-size benchmarks are cached per
        // (hardware, model, device, tile size) — in memory and persisted to
        // disk (~/.cache/upix/auto_tile_cache.json) — so the one-time
        // measurement cost (~8 sizes x (2 warmup + 10 timed) single-tile
        // forwards) is paid once per hardware+model, not per task or
        // process; the engine recommendation is the fallback if the probe
        // cannot produce a usable size.
        int tile_size = config.tile_size;
        if (tile_size == 0) {
            TileSizeOptimizer optimizer;
            ModelConstraints mc;
            mc.model_name = model_name.empty() ? loaded_model_name_ : model_name;
            mc.min_tile_size = config.min_tile_size;
            mc.max_tile_size = config.max_tile_size;
            tile_size = optimizer.find_optimal_tile_size(
                *engine_, mc, config, width, height);
            if (tile_size <= 0) {
                tile_size = engine_->get_recommended_tile_size();
            }
            LOG_INFO("Coordinator",
                     "Auto-selected tile size {} for remote task (tile_size=0)",
                     tile_size);
        }

        effective_model = model_name.empty() ? loaded_model_name_ : model_name;
        LOG_INFO("Coordinator", "Processing locally: {}x{} model={} tile={}", width, height,
                 effective_model, tile_size);

        out_bytes.resize(out_bytes_n);

        TileConfig tc;
        tc.tile_size = tile_size;
        tc.overlap = config.tile_overlap;
        tc.batch_size = 1;

        // Cancellation is wired to the per-task atomic owned by the
        // connection handler (MSG_TASK_CANCEL / dead peer / stop()).
        // TileProcessor checks it between tiles, so a cancel costs at most
        // one tile of wasted inference instead of a whole image.
        std::atomic<bool> local_cancel{false};
        const std::atomic<bool>& cancel = cancel_flag != nullptr ? *cancel_flag : local_cancel;
        const auto t_inf_start = std::chrono::steady_clock::now();
        auto res = tile_processor_->process_image_u8(
            image_data.data(), out_bytes.data(),
            width, height, channels,
            *engine_, tc,
            [this, &progress_sink](const ProcessingProgress& p) {
                const float frac = p.percentage() / 100.0f;
                if (on_task_progress_) {
                    on_task_progress_(frac);
                }
                if (progress_sink) {
                    progress_sink(frac);
                }
            },
            cancel,
            /*enable_alpha_bicubic=*/true);
        inf_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_inf_start).count();
        if (!res) {
            inference_error = res.error().message;
        }
    }
    if (!inference_error.empty()) {
        return std::unexpected(CoordinatorError{4, inference_error});
    }
    // Canceled between the last tile and the encode: skip the (multi-second,
    // un-interruptible) output encode — nobody is waiting for the bytes.
    if (canceled()) {
        return std::unexpected(CoordinatorError{7, "canceled"});
    }

    // Encode before transport: a camera photo at x2 is hundreds of MB raw but
    // only a few MB as jpg/webp. The phone forwards these bytes to Kotlin as-is.
    //   jpg/webp — lossy; output_quality (default 90) is real compression.
    //   png      — lossless; the zlib level only trades encode CPU for transport
    //              SIZE, never pixel quality. Level 9 (the old default, quality=1)
    //              is a pure pessimization: on a 7000x7200 photo it spent ~111 s of
    //              CPU to save ~9 MB vs level 1 (2.4 s). Level 4 sits at the size/
    //              time knee measured on real photos (good ratio, just before the
    //              level 8-9 explosion) — identical decoded pixels, ~24x faster
    //              than level 9. (quality 61 -> zlib level 4 via png_compression_level_from_quality.)
    int transport_quality = output_quality;
    EncoderOptions transport_opts = encoder_opts;
    if (output_format == "png") {
        transport_quality = 61;  // -> zlib level 4 via png_compression_level_from_quality
        // The transport encode is a bandwidth knob, not the user's output
        // setting: force the derived level so an explicit --png-level cannot
        // re-inflate the transport stream.
        transport_opts.png_level = -1;
    }
    const auto t_enc_start = std::chrono::steady_clock::now();
    Image out_image(out_w, out_h, channels);
    std::memcpy(out_image.data(), out_bytes.data(), out_bytes.size());
    // The SR marker rides with the remote output: the phone writes these
    // bytes verbatim, so remote results are recognized on re-input the same
    // way local ones are.
    const auto sr_xmp = sr_marker::build_xmp(
        effective_model, scale, static_cast<int64_t>(std::time(nullptr)));
    auto encoded = out_image.save_to_memory(output_format, transport_quality, sr_xmp, transport_opts);
    const auto t_enc_end = std::chrono::steady_clock::now();
    if (!encoded) {
        return std::unexpected(CoordinatorError{5, std::format("output encode failed: {}", encoded.error().message)});
    }
    // The result streams back in MSG_CLIENT_DATA chunks, so the only hard
    // ceiling is what fits a uint32 total length. (Historically anything over
    // 100 MB was rejected with "use jpg or webp"; chunking removed that.)
    if (encoded->size() > 0xFFFFFFFFull) {
        return std::unexpected(CoordinatorError{
            6, std::format("Encoded output too large for transport ({})",
                           format_bytes(encoded->size()))});
    }

    const double enc_s = std::chrono::duration<double>(t_enc_end - t_enc_start).count();
    LOG_INFO("Coordinator", "Local upscale done: inference {} + encode {} ({}x{} {} -> {})",
             format_seconds(inf_s), format_seconds(enc_s), out_w, out_h,
             output_format, format_bytes(encoded->size()));

    return *encoded;
}

// ── Session wake registration ───────────────────────────────────────────────
// The handler registers its descriptor for the whole session and clears it on
// every exit path, before `conn` (its owner) closes the descriptor: `wake` is
// declared after `conn` in handle_client, so its destructor runs first.
// stop() shutdown()s whatever is registered while holding the same
// `wake_mutex`, so a handler is either still parked in a blocking call
// (shutdown wakes it) or already deregistered (nothing is touched).
Coordinator::SessionWakeRegistration::SessionWakeRegistration(ClientThread& entry,
                                                             TCPSocket* socket)
    : entry_(entry) {
    std::lock_guard<std::mutex> lk(entry_.wake_mutex);
    entry_.session_socket = socket;
}

Coordinator::SessionWakeRegistration::~SessionWakeRegistration() {
    std::lock_guard<std::mutex> lk(entry_.wake_mutex);
    entry_.session_socket = nullptr;
}

void Coordinator::accept_loop(std::stop_token st) {
    // The stop token is checked on every iteration; TCPServer::accept() polls
    // with a 500 ms timeout so this loop re-runs at least every 500 ms even
    // with no incoming connections.
    while (!st.stop_requested()) {
        reap_finished_clients();
        auto client = server_->accept();
        if (!client.has_value()) {
            // EAGAIN/ETIMEDOUT is the normal poll-timeout wake-up: loop back
            // and re-check the stop token. Only log genuinely unexpected errors.
            const int code = client.error().code;
            if (!st.stop_requested() && code != EAGAIN && code != ETIMEDOUT) {
                LOG_ERROR("Coordinator", "Accept failed: {}", client.error().message);
            }
            continue;
        }

        if (client_threads_.size() >= kMaxConcurrentClients) {
            LOG_WARNING("Coordinator",
                        "Refusing connection: {} clients already connected",
                        client_threads_.size());
            continue;  // the refused socket closes with this unique_ptr
        }

        // Record first, thread second: std::jthread's constructor can throw
        // (EAGAIN), and the thread body writes e->done through this pointer —
        // it must never observe a record that was never owned by the vector.
        auto entry = std::make_unique<ClientThread>();
        ClientThread* e = entry.get();
        client_threads_.push_back(std::move(entry));
        try {
            e->thread = std::jthread(
                [this, e, sock = std::move(client.value())](std::stop_token st) mutable {
                    // A handler must never take the process down: an exception
                    // escaping a thread body calls std::terminate. Hostile
                    // input can throw before authentication (libsodium rejects
                    // an all-zero X25519 peer key), so the whole session runs
                    // inside this guard.
                    try {
                        handle_client(st, std::move(sock), *e);
                    } catch (const std::exception& ex) {
                        LOG_ERROR("Coordinator", "client handler aborted: {}", ex.what());
                    } catch (...) {
                        LOG_ERROR("Coordinator", "client handler aborted (unknown exception)");
                    }
                    e->done.store(true);
                });
        } catch (const std::system_error& ex) {
            LOG_ERROR("Coordinator", "cannot start client thread: {}", ex.what());
            client_threads_.pop_back();
        }
    }
}

void Coordinator::reap_finished_clients() {
    // Runs on the accept thread only, so the vector needs no lock. join() on a
    // finished thread returns immediately and releases its stack; records of
    // still-running sessions are kept for stop().
    for (auto it = client_threads_.begin(); it != client_threads_.end();) {
        if (!(*it)->done.load()) {
            ++it;
            continue;
        }
        if ((*it)->thread.joinable()) {
            (*it)->thread.join();
        }
        it = client_threads_.erase(it);
    }
}

// ── Encrypted-frame helpers (mirror Worker's) ────────────────────────────────
auto Coordinator::send_encrypted(WorkerConnection& conn, MessageType type,
                                 const void* payload, size_t len) -> bool {
    if (!conn.session.active()) return false;
    try {
        MessageHeader header{};
        header.type = type;
        header.payload_length = static_cast<uint32_t>(len);
        auto wire = conn.session.seal(header, payload);
        auto res = conn.socket->send_all(wire.data(), wire.size());
        return res.has_value();
    } catch (const std::exception&) {
        return false;
    }
}

auto Coordinator::recv_encrypted(WorkerConnection& conn)
    -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>> {
    if (!conn.session.active()) return std::nullopt;
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    auto hdr_recv = conn.socket->receive_all(hdr_buf.data(), hdr_buf.size());
    if (!hdr_recv.has_value()) return std::nullopt;
    auto parsed = Protocol::parse_header(hdr_buf);
    if (!parsed) return std::nullopt;
    if (parsed->payload_length > MAX_NETWORK_PAYLOAD) return std::nullopt;
    std::vector<uint8_t> wire(Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        auto pay_recv = conn.socket->receive_all(wire.data() + Protocol::HEADER_SIZE,
                                                 parsed->payload_length);
        if (!pay_recv.has_value()) return std::nullopt;
    }
    return conn.session.open(wire.data(), wire.size());
}

auto Coordinator::recv_encrypted_poll(WorkerConnection& conn, int poll_ms,
                                      std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>& out)
    -> FramePoll {
    out.reset();
    if (!conn.session.active()) return FramePoll::Error;
    // Poll window for the first byte only; once any byte of the header is in,
    // the rest of the frame is read with the full session timeout (a poll
    // tick must never abandon a partially received frame while the peer is
    // merely slow — that would corrupt the stream framing).
    conn.socket->set_recv_timeout(poll_ms);
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    size_t got = 0;
    while (got < Protocol::HEADER_SIZE) {
        // Single-shot receive() per call: the io lock is taken and released
        // around each call, so the task worker's sends interleave cleanly.
        auto chunk = conn.socket->receive(hdr_buf.data() + got,
                                          Protocol::HEADER_SIZE - got);
        if (!chunk.has_value()) {
            const int code = chunk.error().code;
            if (got == 0 && (code == EAGAIN || code == EWOULDBLOCK)) {
                return FramePoll::Timeout;
            }
            return FramePoll::Error;
        }
        got += chunk.value();
        if (got > 0) {
            conn.socket->set_recv_timeout(SESSION_FRAME_TIMEOUT_MS);
        }
    }
    auto parsed = Protocol::parse_header(hdr_buf);
    if (!parsed || parsed->payload_length > MAX_NETWORK_PAYLOAD) {
        return FramePoll::Error;
    }
    std::vector<uint8_t> wire(Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        auto pay_recv = conn.socket->receive_all(wire.data() + Protocol::HEADER_SIZE,
                                                 parsed->payload_length);
        if (!pay_recv.has_value()) return FramePoll::Error;
    }
    auto opened = conn.session.open(wire.data(), wire.size());
    if (!opened) return FramePoll::Error;
    out = std::move(opened);
    return FramePoll::Frame;
}

// Server side of the ECDH+signature handshake. `first_hdr_buf` already holds
// the 14 plaintext bytes of the client's KEY_HELLO header (peeked by the
// caller); `first_parsed` is the parsed header. Reads the client KEY_HELLO
// payload, generates the server eph keypair, signs the transcript, sends the
// server KEY_HELLO, derives & activates the session key, reads the (encrypted)
// AUTH_PROOF, verifies the client signature, consults the TrustStore, and
// sends the (encrypted) AUTH_RESPONSE. Returns true on success.
bool Coordinator::run_handshake(WorkerConnection& conn) {
    if (!identity_) {
        LOG_ERROR("Coordinator", "run_handshake: no identity configured");
        return false;
    }

    // (1) Generate server eph keypair + read client KEY_HELLO.
    auto eph = sodium::x25519_keygen();
    const auto s_eph_pub = eph.public_key;
    const auto s_id_pub = identity_->public_key();

    KeyHelloMessage c_hello{};
    {
        std::vector<uint8_t> payload(sizeof(c_hello));
        auto recv = conn.socket->receive_all(payload.data(), payload.size());
        if (!recv.has_value()) {
            LOG_WARNING("Coordinator", "Handshake aborted: client KEY_HELLO payload missing");
            return false;
        }
        std::memcpy(&c_hello, payload.data(), sizeof(c_hello));
    }

    // (2) Build transcript c_eph||c_id||s_eph||s_id (128 bytes).
    std::array<uint8_t, 128> transcript{};
    std::memcpy(transcript.data() + 0,  c_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 32, c_hello.id_pub,  32);
    std::memcpy(transcript.data() + 64, s_eph_pub.data(), 32);
    std::memcpy(transcript.data() + 96, s_id_pub.data(),  32);

    // (3) Sign transcript, send server KEY_HELLO (plaintext).
    KeyHelloMessage s_hello{};
    std::memcpy(s_hello.eph_pub, s_eph_pub.data(), 32);
    std::memcpy(s_hello.id_pub,  s_id_pub.data(),  32);
    auto s_sig = identity_->sign(transcript.data(), transcript.size());
    std::memcpy(s_hello.sig, s_sig.data(), 64);
    {
        MessageHeader hh{};
        hh.type = MessageType::MSG_KEY_HELLO;
        hh.payload_length = sizeof(s_hello);
        auto wire = Protocol::serialize(hh, &s_hello);
        auto send_res = conn.socket->send_all(wire.data(), wire.size());
        if (!send_res.has_value()) {
            LOG_WARNING("Coordinator", "Handshake aborted: failed to send server KEY_HELLO");
            return false;
        }
    }

    // (4) Derive session key + activate. From here on everything is encrypted.
    std::array<uint8_t, 32> c_eph_pub_arr{};
    std::memcpy(c_eph_pub_arr.data(), c_hello.eph_pub, 32);
    auto session_key = sodium::x25519_shared(eph.private_key, c_eph_pub_arr);
    conn.session.activate(session_key);

    // (5) Receive AUTH_PROOF (encrypted) + verify client signature.
    auto proof_frame = recv_encrypted(conn);
    if (!proof_frame) {
        LOG_WARNING("Coordinator", "Handshake aborted: no AUTH_PROOF / MAC failed");
        return false;
    }
    auto& [proof_hdr, proof_payload] = *proof_frame;
    if (proof_hdr.type != MessageType::MSG_AUTH_PROOF ||
        proof_payload.size() < sizeof(AuthProofMessage)) {
        LOG_WARNING("Coordinator", "Handshake aborted: malformed AUTH_PROOF");
        return false;
    }
    const auto* proof = reinterpret_cast<const AuthProofMessage*>(proof_payload.data());

    std::array<uint8_t, 32> c_id_pub_arr{};
    std::memcpy(c_id_pub_arr.data(), c_hello.id_pub, 32);
    std::array<uint8_t, 64> c_sig_arr{};
    std::memcpy(c_sig_arr.data(), proof->sig, 64);
    if (!sodium::ed25519_verify(c_id_pub_arr, c_sig_arr,
                                transcript.data(), transcript.size())) {
        LOG_WARNING("Coordinator", "Handshake aborted: client signature verification failed");
        // Still reply so the worker doesn't block, but mark unauthenticated.
        AuthResponse resp{};
        resp.authenticated = 0;
        resp.trusted = 0;
        resp.device_id[0] = '\0';
        (void)send_encrypted(conn, MessageType::MSG_AUTH_RESPONSE, &resp, sizeof(resp));
        return false;
    }

    conn.peer_id_pub = c_id_pub_arr;
    // Authentication proves key ownership; only affirmative pairing approval
    // may add this identity to the TrustStore.
    bool was_known = false;
    {
        std::lock_guard<std::mutex> lock(trust_store_mtx_);
        was_known = trust_store_ && trust_store_->contains(c_id_pub_arr);
    }
    conn.peer_trusted = was_known;

    // (6) Send AUTH_RESPONSE (encrypted).
    AuthResponse resp{};
    resp.authenticated = 1;
    std::strncpy(resp.device_id, conn.device_id.c_str(), sizeof(resp.device_id) - 1);
    resp.device_id[sizeof(resp.device_id) - 1] = '\0';
    resp.trusted = was_known ? 1 : 0;  // trust requires prior approval
    if (!send_encrypted(conn, MessageType::MSG_AUTH_RESPONSE, &resp, sizeof(resp))) {
        LOG_WARNING("Coordinator", "Handshake aborted: failed to send AUTH_RESPONSE");
        return false;
    }

    conn.authenticated = true;
    LOG_INFO("Coordinator", "Client handshake ok (device_id=" + conn.device_id +
             ", trusted=" + (was_known ? "1" : "0 (approval required)") + ")");
    return true;
}

// Declared chunked length for a captured submit (0 = legacy single-frame /
// invalid declaration). DATA continuations are matched against this.
uint32_t Coordinator::declared_chunk_len(const CapturedSubmit& cap) {
    if (cap.payload.size() != sizeof(ClientTaskSubmit)) {
        return 0;
    }
    ClientTaskSubmit p{};
    std::memcpy(&p, cap.payload.data(), sizeof(p));
    if (p.input_data_length == 0 || p.input_data_length > MAX_NETWORK_PAYLOAD) {
        return 0;
    }
    return p.input_data_length;
}

// EXIF sniff → decode → channel normalize → orientation bake. Byte-identical
// to what run_client_task has always built inline; factored out so the
// submit-ahead prefetch thread can produce the same input state while the
// GPU is still busy with the previous task.
auto Coordinator::prepare_task_input(
    const ClientTaskSubmit& task,
    const std::vector<uint8_t>& compressed,
    std::vector<uint8_t>& pixels_out,
    int& width_out,
    int& height_out,
    int& channels_out) -> std::optional<std::string> {
    // Sniff EXIF orientation from the encoded bytes before they are freed:
    // portrait JPEGs (orientation 5-8) decode unrotated, and the bake-in
    // below keeps the remote path upright like the phone's local path.
    const int exif_orientation =
        exif::parse_jpeg_exif_orientation(compressed.data(),
                                          compressed.size());
    auto decoded = Image::load_from_memory(task.input_format, compressed);
    if (!decoded) {
        return "input decode failed: " + decoded.error().message;
    }
    // Normalize gray and other model inputs before inference. RGBA stays
    // intact for RGB models: TileProcessor splits off alpha and merges it
    // after inference instead of compositing it away here.
    int expected_channels = 0;
    {
        std::lock_guard<std::mutex> lk(models_mutex_);
        if (auto task_model = model_manager_.get_model(
                bounded_string(task.model_name, sizeof(task.model_name)))) {
            expected_channels = task_model->input_channels;
        }
    }
    if (!upix::normalize_image_channels(*decoded, expected_channels,
                                        "remote task")) {
        return "channel conversion failed";
    }
    width_out = decoded->width();
    height_out = decoded->height();
    channels_out = decoded->channels();
    pixels_out.assign(decoded->data(), decoded->data() + decoded->size());
    if (exif_orientation != 1 &&
        exif::apply_exif_orientation_u8(pixels_out, width_out, height_out,
                                        channels_out, exif_orientation)) {
        LOG_INFO("Coordinator", "applied EXIF orientation {}", exif_orientation);
    }
    return std::nullopt;
}

// Spawn the prefetch decoder for a fully-available head entry. Only ever
// called from the connection handler thread while it owns the queue, so no
// locking around the entry itself — the shared AheadDecode state is the sole
// cross-thread channel.
void Coordinator::start_ahead_decode(CapturedSubmit& cap) {
    cap.ahead = std::make_shared<AheadDecode>();
    const auto state = cap.ahead;
    ClientTaskSubmit task{};
    std::memcpy(&task, cap.payload.data(), sizeof(task));
    // Source snapshot: staged chunks for chunked submits, the payload tail
    // for legacy inline ones. Either way a copy — the entry keeps owning its
    // bytes because reassembly/serving reads them again later.
    std::vector<uint8_t> source;
    if (cap.staged_data.empty() &&
        cap.payload.size() > sizeof(ClientTaskSubmit)) {
        source.assign(cap.payload.begin() + sizeof(ClientTaskSubmit),
                      cap.payload.end());
    } else {
        source = cap.staged_data;
    }
    const auto bytes = std::make_shared<const std::vector<uint8_t>>(
        std::move(source));
    state->worker = std::make_unique<std::jthread>(
        [this, state, task, bytes](std::stop_token) {
            std::vector<uint8_t> pixels;
            int w = 0, h = 0, c = 0;
            const auto err = prepare_task_input(task, *bytes, pixels, w, h, c);
            {
                std::lock_guard<std::mutex> lk(state->mtx);
                if (err) {
                    state->ok = false;
                    state->error = *err;
                } else {
                    state->ok = true;
                    state->pixels = std::move(pixels);
                    state->width = w;
                    state->height = h;
                    state->channels = c;
                }
                state->ready = true;
            }
            state->cv.notify_all();
        });
}

// Prefetch trigger for the current head entry: fires when its full input is
// present on the queue — legacy inline payloads are complete the moment they
// are armed; chunked completes once staged bytes reach the declared length.
void Coordinator::maybe_launch_head_prefetch(
    std::deque<CapturedSubmit>& q) {
    if (q.empty()) {
        return;
    }
    CapturedSubmit& head = q.front();
    if (head.ahead) {
        return;
    }
    // A payload shorter than the submit struct is not a submit at all: the
    // pump answers it with a framing error when served. Read nothing from it —
    // the copy below would read past the buffer.
    if (head.payload.size() < sizeof(ClientTaskSubmit)) {
        return;
    }
    size_t have = 0;
    uint32_t want = 0;
    if (head.payload.size() == sizeof(ClientTaskSubmit)) {
        // Chunked framing: declared length drives staged completion.
        want = declared_chunk_len(head);
        have = head.staged_data.size();
    } else {
        // Legacy single-frame framing: input rides behind the struct.
        ClientTaskSubmit p{};
        std::memcpy(&p, head.payload.data(), sizeof(p));
        want = p.input_data_length;
        have = head.payload.size() - sizeof(ClientTaskSubmit);
    }
    if (want == 0 || have < want ||
        head.payload.size() <= sizeof(ClientTaskSubmit)) {
        return;
    }
    start_ahead_decode(head);
    LOG_INFO("Coordinator",
             "Submit-ahead prefetch decode started for {} ({})",
             format_bytes(want),
             head.payload.size() == sizeof(ClientTaskSubmit) ? "chunked"
                                                             : "inline");
}

bool Coordinator::capture_input_complete(const CapturedSubmit& cap) {
    if (cap.payload.size() == sizeof(ClientTaskSubmit)) {
        // Chunked framing: complete once staged bytes reach the declared len.
        const uint32_t want = declared_chunk_len(cap);
        return want > 0 && cap.staged_data.size() >= want;
    }
    if (cap.payload.size() < sizeof(ClientTaskSubmit)) {
        return false;  // malformed; the pump replies an error when served
    }
    // Legacy single-frame framing: struct + input inline, so whatever the
    // declared length is, all bytes are already here.
    return true;
}

bool Coordinator::assemble_capture(const CapturedSubmit& cap,
                                   ClientTaskSubmit& task_out,
                                   std::vector<uint8_t>& input_out,
                                   std::string& err) {
    std::memcpy(&task_out, cap.payload.data(), sizeof(task_out));
    const uint32_t data_len = task_out.input_data_length;

    if (cap.payload.size() == sizeof(ClientTaskSubmit)) {
        // Chunked: staged_data must hold exactly the declared payload.
        if (data_len == 0 || data_len > MAX_NETWORK_PAYLOAD) {
            err = "invalid chunked submit size";
            return false;
        }
        input_out.resize(data_len);
        std::memcpy(input_out.data(), cap.staged_data.data(), data_len);
        return true;
    }

    // Legacy single-frame: struct + bytes tail. A structurally broken frame
    // here means the peer violated framing — caller drops the connection.
    if (data_len > MAX_NETWORK_PAYLOAD ||
        sizeof(ClientTaskSubmit) + data_len > cap.payload.size()) {
        err.clear();
        return false;
    }
    input_out.assign(cap.payload.begin() + sizeof(ClientTaskSubmit),
                     cap.payload.end());
    return true;
}

// Session task pump (see coordinator.hpp for the full contract).
bool Coordinator::pump_session_submits(
    WorkerConnection& conn, std::stop_token& st,
    CapturedSubmit&& first) {
    std::deque<CapturedSubmit> captures;
    captures.push_back(std::move(first));
    std::vector<std::unique_ptr<LiveTask>> inflight;

    // Every exit path joins workers BEFORE returning — they reference conn.
    // After flipping cancel, every gate-parked worker must be notified, or a
    // run waiting for its send turn would hang forever on join (its predicate
    // changed while it was parked — the classic lost wakeup).
    const auto cancel_all_and_join = [&] {
        for (auto& t : inflight) {
            t->run->cancel.store(true);
            std::lock_guard<std::mutex> lk(t->run->send_order_m);
            t->run->send_order_cv.notify_all();
        }
        for (auto& t : inflight) {
            if (t->thread.joinable()) {
                t->thread.join();
            }
        }
        inflight.clear();
    };

    // Reply helper for structurally invalid captures: per-task error, session
    // stays usable. Transport failures still drop the connection (send false
    // via send failure detection on the next poll).
    const auto reply_task_error = [&](const std::string& message) {
        ClientTaskResponse response{};
        response.success = 0;
        std::strncpy(response.error_message, message.c_str(),
                     sizeof(response.error_message) - 1);
        LOG_ERROR("Coordinator", "Client submit rejected: " + message);
        (void)send_encrypted(conn, MessageType::MSG_CLIENT_RESPONSE,
                             &response, sizeof(response));
    };

    bool transport_ok = true;

    // Open the socket-write gate for the oldest unfinished run. Idempotent;
    // called whenever the front may have changed (initial spawn, any reap).
    const auto grant_front_send = [&] {
        if (inflight.empty()) {
            return;
        }
        auto* r = inflight.front()->run.get();
        {
            std::lock_guard<std::mutex> lk(r->send_order_m);
            r->send_ready = true;
        }
        r->send_order_cv.notify_all();
    };

    for (;;) {
        // Shutdown must not orphan workers spinning on an dead socket: flip
        // cancel every iteration and bail out as soon as a joinable wake-up
        // (any Timeout/Error tick reaches here within TASK_FRAME_POLL_MS).
        if (st.stop_requested()) {
            cancel_all_and_join();
            return false;
        }

        // A head capture shorter than the submit struct can never become
        // servable — it is neither a chunked declaration (payload == struct)
        // nor a legacy inline frame (payload > struct) — so the waiting below
        // would poll for DATA chunks that can never complete it and wedge the
        // session forever. Treat it as the framing violation it is.
        if (!captures.empty() &&
            captures.front().payload.size() < sizeof(ClientTaskSubmit)) {
            LOG_WARNING("Coordinator",
                        "Peer {} sent a CLIENT_SUBMIT shorter than the submit "
                        "struct ({} bytes); dropping",
                        conn.device_id, captures.front().payload.size());
            cancel_all_and_join();
            return false;
        }

        // Spawn new workers from complete head captures while slots last.
        while (!captures.empty() && inflight.size() < kMaxInflightTasks &&
               capture_input_complete(captures.front())) {
            CapturedSubmit cap = std::move(captures.front());
            captures.pop_front();

            ClientTaskSubmit task{};
            std::vector<uint8_t> input;
            std::string err;
            if (!assemble_capture(cap, task, input, err)) {
                if (!err.empty()) {
                    reply_task_error(err);
                    continue;  // malformed sizes: answered, keep serving
                }
                cancel_all_and_join();
                return false;  // broken legacy framing: drop the connection
            }
            LOG_INFO("Coordinator", "Serving staged submit: {} ({})",
                     format_bytes(input.size()),
                     cap.payload.size() == sizeof(ClientTaskSubmit)
                         ? "chunked" : "inline");

            auto live = std::make_unique<LiveTask>();
            auto predecoded = std::move(cap.ahead);
            auto* run_ptr = live->run.get();
            live->thread = std::thread(
                [this, &conn, task = std::move(task), input = std::move(input),
                 run_ptr, p = std::move(predecoded)]() mutable {
                    run_client_task(conn, task, std::move(input), *run_ptr,
                                    std::move(p));
                });
            inflight.push_back(std::move(live));
        }

        // Reap finished tasks and exit once nothing is left to serve.
        for (auto it = inflight.begin(); it != inflight.end();) {
            if ((*it)->run->done.load()) {
                if (it->get()->thread.joinable()) {
                    it->get()->thread.join();
                }
                it = inflight.erase(it);
            } else {
                ++it;
            }
        }
        grant_front_send();
        if (inflight.empty() && captures.empty()) {
            // Batch lull: hand control back to the session loop's blocking
            // read; a fresh submit re-enters the pump within microseconds.
            conn.socket->set_timeout(SESSION_FRAME_TIMEOUT_MS);
            return transport_ok;
        }

        // Progress frames are sent for the OLDEST unfinished run only —
        // TaskProgress has no task id, and the phone keys percent/ETA on the
        // task it is currently awaiting (service order = arrival order).
        if (transport_ok && !inflight.empty()) {
            const int pending_pct =
                inflight.front()->run->progress_pct.exchange(
                    -1, std::memory_order_relaxed);
            if (pending_pct >= 0) {
                const bool perf_timer = std::getenv("UPIX_PIPELINE_TIMER") != nullptr;
                TaskProgress msg{};
                msg.stage = 0;
                msg.percent = static_cast<uint32_t>(pending_pct);
                msg.eta_seconds = inflight.front()->run->progress_eta.load(
                    std::memory_order_relaxed);
                if (!send_encrypted(conn, MessageType::MSG_TASK_PROGRESS,
                                    &msg, sizeof(msg))) {
                    LOG_WARNING("Coordinator",
                                "Progress send failed (peer gone); canceling");
                    cancel_all_and_join();
                    return false;
                }
                if (perf_timer) {
                    LOG_INFO("Coordinator", "[PERF] progress send pct={}",
                             pending_pct);
                }
            }
        }

        std::optional<std::pair<MessageHeader, std::vector<uint8_t>>> frame;
        const auto pf_t0 = std::chrono::steady_clock::now();
        const auto poll = recv_encrypted_poll(conn, TASK_FRAME_POLL_MS, frame);
        if (std::getenv("UPIX_PIPELINE_TIMER") != nullptr &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pf_t0).count() > 500) {
            LOG_INFO("Coordinator",
                     "[PERF] poll tick took {:.1f} ms",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - pf_t0).count());
        }
        if (poll == FramePoll::Timeout) {
            continue;
        }
        if (poll == FramePoll::Error) {
            LOG_WARNING("Coordinator",
                        "Connection lost while task(s) were running; canceling");
            cancel_all_and_join();
            return false;
        }

        if (st.stop_requested()) {
            cancel_all_and_join();
            return false;
        }

        // ── Reassembly gate for an in-flight chunked upload ─────────────────
        // If the HEAD capture is a chunked submit whose bytes are still
        // trickling in, this frame must be its next MSG_CLIENT_DATA
        // continuation. That stays true even when nothing is running: a slow
        // phone may simply still be uploading task N+2 between waves of work.
        // Any other frame means that staging will never finish (version
        // mismatch / sender bug): answer a PER-TASK error and abandon only
        // that capture so the peer can fall back to a legacy single-frame
        // submit on this live session. Never drop the connection here.
        if (!captures.empty()) {
            const uint32_t want = declared_chunk_len(captures.front());
            const size_t& have = captures.front().staged_data.size();
            if (want > 0 && have < want) {
                CapturedSubmit& head = captures.front();
                if (frame->first.type == MessageType::MSG_CLIENT_DATA &&
                    frame->second.size() <= want - have) {
                    head.staged_data.insert(head.staged_data.end(),
                                            frame->second.begin(),
                                            frame->second.end());
                    maybe_launch_head_prefetch(captures);
                    continue;
                }
                if (frame->first.type == MessageType::MSG_CLIENT_DATA) {
                    reply_task_error("chunked submit overflow");
                } else {
                    reply_task_error(
                        "chunked submit interrupted: expected CLIENT_DATA");
                }
                captures.pop_front();
                continue;
            }
        }

        if (frame->first.type == MessageType::MSG_TASK_CANCEL) {
            LOG_INFO("Coordinator", "Task cancel requested by peer " +
                     conn.device_id + " (canceling all in-flight tasks)");
            for (auto& t : inflight) {
                t->run->cancel.store(true);
                std::lock_guard<std::mutex> lk(t->run->send_order_m);
                t->run->send_order_cv.notify_all();
            }
            continue;
        }

        if (frame->first.type == MessageType::MSG_CLIENT_SUBMIT) {
            if (captures.size() >= kMaxAheadCaptures) {
                LOG_WARNING("Coordinator",
                            "Peer {} exceeded the submit-ahead capture bound; "
                            "dropping",
                            conn.device_id);
                cancel_all_and_join();
                return false;
            }
            CapturedSubmit cap;
            cap.header = frame->first;
            cap.payload = std::move(frame->second);
            captures.push_back(std::move(cap));
            // An inline submit is complete the moment it lands — its whole
            // input can start decoding while the GPU keeps working.
            maybe_launch_head_prefetch(captures);
            continue;
        }

        if (frame->first.type == MessageType::MSG_CLIENT_DATA && !captures.empty() &&
            declared_chunk_len(captures.back()) > 0 &&
            frame->second.size() <=
                declared_chunk_len(captures.back()) -
                    captures.back().staged_data.size()) {
            captures.back().staged_data.insert(captures.back().staged_data.end(),
                                               frame->second.begin(),
                                               frame->second.end());
            // Tail entry fully staged while the GPU may still be busy: start
            // the prefetch decode now so its inference begins without paying
            // input prep. Only completions get one — deeper prefetches would
            // pin decoded frames in RAM for no latency gain.
            maybe_launch_head_prefetch(captures);
            continue;
        }

        // A frame that cannot belong to any open capture slot (non-submit,
        // DATA past declared length, or DATA with nothing staged): protocol
        // violation. Cancel so no GPU time is burned for a peer that will
        // never read the result, and drop the connection.
        LOG_WARNING("Coordinator",
            "Peer {} sent an unexpected frame; dropping "
            "(type={} len={} queued={} inflight={})",
            conn.device_id,
            static_cast<unsigned>(frame->first.type), frame->second.size(),
            captures.size(), inflight.size());
        cancel_all_and_join();
        return false;
    }
}

// Worker body of one CLIENT_SUBMIT: decode → process_locally → terminal
// response (+DATA continuations). Every mid-task send happens on this thread,
// so the handler's polled reads never contend with a half-written frame.
void Coordinator::run_client_task(WorkerConnection& conn, ClientTaskSubmit task,
                                  std::vector<uint8_t> compressed_input,
                                  ClientTaskRun& run,
                                  std::shared_ptr<AheadDecode> predecoded) {
    const auto canceled = [&run] {
        return run.cancel.load(std::memory_order_relaxed);
    };
    // Serialize this run's socket writes behind any still-unfinished earlier
    // task of the same session: the phone reads terminal responses FIFO in
    // submit order, so a task that finishes early must not transmit ahead of
    // its turn (see ClientTaskRun::send_ready). The gate always opens once
    // cancel lands — by then the peer is going away either way. Polling
    // fallback so a missed wakeup can never park a worker forever.
    const auto ordered_send = [&](MessageType type, const void* payload,
                                  size_t len) {
        {
            std::unique_lock<std::mutex> lk(run.send_order_m);
            while (!run.send_ready && !canceled()) {
                run.send_order_cv.wait_for(lk, std::chrono::milliseconds(250));
            }
        }
        return send_encrypted(conn, type, payload, len);
    };
    // Per-task error reply that keeps the session alive for the next submit.
    const auto reply_error = [&](const std::string& message) {
        ClientTaskResponse response{};
        response.success = 0;
        std::strncpy(response.error_message, message.c_str(),
                     sizeof(response.error_message) - 1);
        (void)ordered_send(MessageType::MSG_CLIENT_RESPONSE,
                           &response, sizeof(response));
        LOG_ERROR("Coordinator", "Client task failed: " + message);
    };
    // Ensure done is set even if anything below throws unexpectedly.
    struct DoneGuard {
        ClientTaskRun& run;
        ~DoneGuard() { run.done.store(true); }
    } done_guard{run};

    try {
        // Wire char[] fields are not guaranteed NUL-terminated; see
        // bounded_string.
        std::string model_name =
            bounded_string(task.model_name, sizeof(task.model_name));
        std::string output_format =
            bounded_string(task.output_format, sizeof(task.output_format));
        if (output_format.empty()) output_format = "jpg";
        const int output_quality = task.output_quality == 0 ? 90
            : static_cast<int>(task.output_quality);
        const EncoderOptions encoder_opts = from_wire(task.encoder);

        // Cancel raced the upload/parse: don't even decode.
        if (canceled()) {
            reply_error("canceled");
            return;
        }
        // Input prep: either await the submit-ahead prefetch (a helper thread
        // decoded while the previous task still held the GPU) or do it inline
        // — prepare_task_input is the single shared implementation, so both
        // paths build byte-identical inputs and reply identical errors.
        int img_width = 0;
        int img_height = 0;
        int channels = 0;
        std::vector<uint8_t> image_data;
        if (predecoded) {
            std::unique_lock<std::mutex> lk(predecoded->mtx);
            predecoded->cv.wait(lk, [&] { return predecoded->ready; });
            if (!predecoded->ok) {
                reply_error(predecoded->error);
                return;
            }
            image_data = std::move(predecoded->pixels);
            img_width = predecoded->width;
            img_height = predecoded->height;
            channels = predecoded->channels;
        } else {
            const auto prep_error = prepare_task_input(
                task, compressed_input, image_data,
                img_width, img_height, channels);
            compressed_input.clear();
            compressed_input.shrink_to_fit();
            if (prep_error) {
                reply_error(*prep_error);
                return;
            }
        }

        LOG_INFO("Coordinator", "Received authenticated client task: {}x{} model={}",
                 img_width, img_height, model_name);

        ProcessingConfig config;
        config.tile_size = static_cast<int>(task.tile_size);
        config.tile_overlap = static_cast<int>(task.tile_overlap);

        // Stream progress to the phone during the upscale (NAT keepalive +
        // real progress for the UI), throttled to whole-percent changes.
        // The callback runs on this worker thread, so last_sent_pct needs no
        // locking. A failed send means the peer is gone: flag cancel so the
        // remaining tiles are skipped — the PC must not keep computing for a
        // connection that cannot receive the result anymore.
        int last_sent_pct = -1;
        const auto task_start = std::chrono::steady_clock::now();
        auto progress_sink = [&last_sent_pct, &run, &task_start](float frac) {
            const int pct = static_cast<int>(
                std::clamp(frac, 0.0f, 1.0f) * 100.0f);
            if (pct == last_sent_pct) {
                return;
            }
            // Publish only: the handler thread transmits progress frames
            // from its poll loop. Direct sends here convoyed against the
            // polled reader on the shared io_mutex (~10 s per frame).
            // ETA math mirrors the CLI's linear projection.
            run.progress_pct.store(pct, std::memory_order_relaxed);
            if (pct > 0 && pct < 100) {
                const double elapsed =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - task_start).count();
                run.progress_eta.store(
                    static_cast<uint32_t>(elapsed / pct * (100 - pct)),
                    std::memory_order_relaxed);
            }
        };

        auto process_result = process_locally(
            image_data,
            static_cast<int>(img_width),
            static_cast<int>(img_height),
            static_cast<int>(channels),
            model_name,
            config,
            output_format,
            output_quality,
            encoder_opts,
            progress_sink,
            &run.cancel
        );
        // Whatever the engine reported, a cancel-flagged task answers
        // "canceled" — that is the string the phone keys its cancel path on.
        if (!process_result.has_value() && canceled()) {
            process_result = std::unexpected(CoordinatorError{7, "canceled"});
            LOG_INFO("Coordinator", "Task aborted: inference stopped after cancel");
        }

        // Label the encode phase so the phone's UI can show
        // "encoding..." instead of a frozen 100%.
        if (process_result.has_value()) {
            TaskProgress enc{};
            enc.stage = 1;
            (void)ordered_send(MessageType::MSG_TASK_PROGRESS, &enc, sizeof(enc));
        }

        ClientTaskResponse response{};
        if (process_result.has_value()) {
            response.success = 1;
            response.result_data_length = static_cast<uint32_t>(process_result->size());
        } else {
            response.success = 0;
            response.result_data_length = 0;
            std::strncpy(response.error_message,
                         process_result.error().message.c_str(),
                         sizeof(response.error_message) - 1);
        }

        // Response header + result data go in one encrypted (boxed) frame,
        // matching the way the phone opens it (session.open verifies the MAC).
        // Encoded results above MAX_NETWORK_PAYLOAD stream as MSG_CLIENT_DATA
        // continuation frames instead; small results stay single-frame.
        constexpr size_t CLIENT_CHUNK_BYTES = CLIENT_DATA_CHUNK_BYTES;
        // A single RESPONSE frame is bounded by what the RECEIVER accepts on
        // the wire: it rejects any frame whose header payload_length exceeds
        // MAX_NETWORK_PAYLOAD, and sealing adds the nonce+MAC on top of the
        // plaintext. Leave room for both that overhead and the response
        // struct, or a result near the cap would be sent as a first frame the
        // phone rejects — exactly the case this chunked path exists for.
        // Everything past the leading slice streams as MSG_CLIENT_DATA.
        constexpr size_t kSealOverhead = 24 + 16;  // secretbox nonce + MAC
        constexpr size_t kMaxSingleFramePlaintext =
            MAX_NETWORK_PAYLOAD - sizeof(ClientTaskResponse) - kSealOverhead;
        // A reference, not value_or(): the latter would copy the whole encoded
        // result (tens of MB) just so a temporary could be bound.
        const std::vector<uint8_t> no_result;
        const std::vector<uint8_t>& result_bytes =
            process_result.has_value() ? *process_result : no_result;
        const size_t leading_bytes =
            std::min<size_t>(result_bytes.size(), kMaxSingleFramePlaintext);
        std::vector<uint8_t> resp_payload(sizeof(ClientTaskResponse) + leading_bytes);
        std::memcpy(resp_payload.data(), &response, sizeof(response));
        if (!result_bytes.empty()) {
            std::memcpy(resp_payload.data() + sizeof(ClientTaskResponse),
                        result_bytes.data(), leading_bytes);
        }
        const auto t_send_start = std::chrono::steady_clock::now();
        bool sent_ok = ordered_send(MessageType::MSG_CLIENT_RESPONSE,
                                    resp_payload.data(), resp_payload.size());
        for (size_t offset = resp_payload.size() - sizeof(ClientTaskResponse);
             sent_ok && offset < result_bytes.size();
             offset += CLIENT_CHUNK_BYTES) {
            const size_t n = std::min(CLIENT_CHUNK_BYTES, result_bytes.size() - offset);
            sent_ok = ordered_send(MessageType::MSG_CLIENT_DATA,
                                   result_bytes.data() + offset, n);
        }
        const auto t_send_end = std::chrono::steady_clock::now();
        if (!sent_ok) {
            LOG_ERROR("Coordinator", "Failed to send authenticated client response");
            return;
        }
        if (process_result.has_value()) {
            const double send_s =
                std::chrono::duration<double>(t_send_end - t_send_start).count();
            const size_t sent_bytes = process_result->size();
            LOG_INFO("Coordinator", "Task completed: sent " + format_bytes(sent_bytes) +
                     " in " + format_seconds(send_s));
        } else {
            LOG_ERROR("Coordinator", "Authenticated client task failed: " +
                     process_result.error().message);
        }
    } catch (const std::exception& e) {
        reply_error(std::string("internal task error: ") + e.what());
    } catch (...) {
        reply_error("internal task error: unknown");
    }
}

void Coordinator::handle_client(std::stop_token st,
                                std::unique_ptr<TCPSocket> socket,
                                ClientThread& entry) {
    WorkerConnection conn;
    conn.socket = std::move(socket);

    // Register this session's descriptor with the accept-loop record before
    // the first blocking read, so stop() can wake this handler out of any
    // socket call (a peer that never sends its first frame is parked here for
    // the whole 10 s handshake window, an idle authenticated peer for up to
    // SESSION_FRAME_TIMEOUT_MS). Declared AFTER conn: its destructor clears
    // the registration before conn closes the descriptor.
    SessionWakeRegistration wake(entry, conn.socket.get());

    // Shutdown raced with an accepted connection: drop it immediately.
    // Checked AFTER registering — stop() requests the stop token before it
    // wakes registered sessions, so a wake that ran before this registration
    // is always visible here, and a wake issued after it hits the descriptor.
    // No interleaving leaves the handler blocking on a peer that stop() has
    // already given up on.
    if (st.stop_requested()) {
        return;
    }

    // Bound the whole pre-session phase (first header + KEY_HELLO handshake +
    // CONNECT_REQUEST). Accepted sockets are created without SO_RCVTIMEO, so
    // without this a silent peer blocks this thread forever. (The session
    // paths below install their own, longer timeouts.)
    conn.socket->set_timeout(HANDSHAKE_TIMEOUT_MS);

    // First, peek the very first header to decide which kind of client this is.
    //   - MSG_KEY_HELLO → encrypted ECDH+sig handshake, then pairing gate and
    //     an authenticated CLIENT_SUBMIT / model-library op.
    // Anything else is a protocol error → drop.
    std::vector<uint8_t> first_hdr(Protocol::HEADER_SIZE);
    auto first_recv = conn.socket->receive_all(first_hdr.data(), first_hdr.size());
    if (!first_recv.has_value()) {
        return;
    }
    auto first_parsed = Protocol::parse_header(first_hdr);
    if (!first_parsed) {
        return;
    }
    conn.device_id = crypto::generate_uuid().substr(0, 35);

    // ── Branch on the first frame ────────────────────────────────────────────
    if (first_parsed->type == MessageType::MSG_KEY_HELLO) {
        // ── Encrypted worker handshake path ──
        if (first_parsed->payload_length != sizeof(KeyHelloMessage)) {
            return;
        }
        if (!run_handshake(conn)) {
            return;
        }

        // Every peer must open the pairing gate before sending business frames.
        // Known identities still send CONNECT_REQUEST to negotiate capabilities.
        conn.socket->set_timeout(5000);
        auto second_frame = recv_encrypted(conn);
        if (!second_frame) {
            LOG_WARNING("Coordinator", "Peer " + conn.device_id +
                " did not send CONNECT_REQUEST within timeout");
        } else {
            auto [frame_hdr, frame_payload] = std::move(*second_frame);
            if (frame_hdr.type != MessageType::MSG_CLIENT_CONNECT_REQUEST ||
                st.stop_requested() || !running_) {
                LOG_WARNING("Coordinator", "Peer skipped CONNECT_REQUEST or server stopped");
                return;
            }

            // ── Mobile pairing request/allow gate ───────────────────────────
            // The only entry into the session dispatch loop is an approved gate.
            if (frame_hdr.type == MessageType::MSG_CLIENT_CONNECT_REQUEST) {
                if (frame_payload.size() < sizeof(ClientConnectRequest)) {
                    LOG_ERROR("Coordinator", "CONNECT_REQUEST payload too short");
                    return;
                }
                const ClientConnectRequest* creq =
                    reinterpret_cast<const ClientConnectRequest*>(frame_payload.data());
                std::string req_device_name(creq->device_name,
                    std::find(std::begin(creq->device_name), std::end(creq->device_name), '\0'));
                const std::string req_fingerprint = fingerprint(conn.peer_id_pub);

                // Auto-approve already-trusted peers; first-time peers block on
                // an interactive prompt served by the PC main loop.
                bool approved = false;
                if (conn.peer_trusted) {
                    approved = true;
                    LOG_INFO("Coordinator", "Auto-approved trusted peer " +
                             req_device_name + " (" + req_fingerprint + ")");
                } else {
                    PendingApproval pa;
                    pa.device_name = req_device_name;
                    pa.fingerprint = req_fingerprint;
                    pa.identity_pub = conn.peer_id_pub;
                    pa.verdict = std::make_shared<std::promise<uint8_t>>();
                    auto verdict = pa.verdict->get_future();
                    {
                        std::lock_guard<std::mutex> lock(approval_mtx_);
                        if (!running_ || st.stop_requested()) {
                            return;
                        }
                        pa.id = next_approval_id_++;
                        approval_futures_[pa.id] = pa.verdict;
                        pending_approvals_.push(pa);
                    }
                    LOG_INFO("Coordinator", "Pairing request queued from " +
                             req_device_name + " (" + req_fingerprint +
                             ") — waiting for PC approval");
                    // While the UI decides, detect EOF or premature business
                    // traffic without consuming a partial frame or blocking stop().
                    conn.socket->set_recv_timeout(100);
                    while (verdict.wait_for(std::chrono::milliseconds(0)) !=
                           std::future_status::ready) {
                        if (st.stop_requested() || !running_) {
                            resolve_approval(pa.id, false);
                            return;
                        }
                        uint8_t byte = 0;
                        auto received = conn.socket->receive(&byte, 1);
                        if (received || (received.error().code != EAGAIN &&
                                         received.error().code != EWOULDBLOCK)) {
                            resolve_approval(pa.id, false);
                            return;
                        }
                    }
                    approved = verdict.get() != 0;
                    conn.socket->set_recv_timeout(5000);
                }
                if (st.stop_requested() || !running_) {
                    return;
                }

                // Persist an affirmative approval BEFORE replying CONNECT_RESPONSE
                // so an immediate reconnect (or post-response assertion) never
                // races TrustStore persistence. Serialize with stop() so
                // shutdown cannot turn a canceled prompt into trust.
                if (approved && !conn.peer_trusted) {
                    std::lock_guard<std::mutex> approval_lock(approval_mtx_);
                    if (!running_ || st.stop_requested()) {
                        return;
                    }
                    std::lock_guard<std::mutex> trust_lock(trust_store_mtx_);
                    if (trust_store_) {
                        const auto now = std::time(nullptr);
                        std::tm tm_utc{};
#ifdef _WIN32
                        gmtime_s(&tm_utc, &now);
#else
                        gmtime_r(&now, &tm_utc);
#endif
                        char ts_buf[32]{};
                        std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
                        trust_store_->add(conn.peer_id_pub, req_device_name, ts_buf);
                        std::error_code save_ec;
                        if (!trust_store_->save(save_ec)) {
                            LOG_WARNING("Coordinator",
                                        "Failed to persist TrustStore {}: {}",
                                        trust_store_path_.string(), save_ec.message());
                        } else {
                            LOG_INFO("Coordinator", "Peer added to TrustStore: " + req_fingerprint);
                        }
                    }
                }

                ClientConnectResponse cresp{};
                cresp.approved = approved ? 1 : 0;
                cresp.trusted = conn.peer_trusted ? 1 : 0;
                // Cached dlopen probes: the phone gates AVIF input/output on
                // these instead of discovering it through task failures.
                cresp.avif_decode = avif_codec::avif_decode_available() ? 1 : 0;
                cresp.avif_encode = avif_codec::avif_encode_available() ? 1 : 0;
                // Submit-ahead depth this session tolerates: the in-task
                // frame loop keeps up to pipeline_depth+1 CLIENT_SUBMITs
                // alive at once (2 = one computing + one more fully staged;
                // the phone's upload window is keyed on this). The multi-
                // worker pump actually runs depth+1 concurrent tasks so task
                // N's encode/transfer overlaps with N+1's inference.
                cresp.pipeline_depth = 2;
                if (!send_encrypted(conn, MessageType::MSG_CLIENT_CONNECT_RESPONSE,
                                    &cresp, sizeof(cresp))) {
                    LOG_ERROR("Coordinator", "Failed to send CONNECT_RESPONSE");
                    return;
                }
                if (!approved) {
                    LOG_WARNING("Coordinator", "Pairing denied for " + req_device_name);
                    return;
                }

                // Now read the first session frame (third frame after the
                // request): the phone sends MSG_CLIENT_SUBMIT with the image
                // to upscale (or, historically, a model-library op).
                conn.socket->set_timeout(30000);
                auto role_frame = recv_encrypted(conn);
                if (!role_frame) {
                    LOG_WARNING("Coordinator", "Peer " + conn.device_id +
                        " closed after CONNECT_RESPONSE without a CLIENT_SUBMIT");
                    return;
                }
                frame_hdr = std::move(role_frame->first);
                frame_payload = std::move(role_frame->second);
            }

            // ── Session frame loop ────────────────────────────────────────────
            // CLIENT_SUBMIT keeps the connection open: the phone pipelines a
            // whole batch over one session (handshake/pairing once). The
            // multi-task pump owns everything from the first submit until
            // the batch goes quiet — while a task computes it stages further
            // submits and keeps up to kMaxInflightTasks workers alive so the
            // GPU never idles between images. Model-library ops stay one-shot
            // (handle + return). Legacy clients that close after the first
            // response end the loop on the next recv (EOF).
            for (;;) {
            if (frame_hdr.type == MessageType::MSG_CLIENT_SUBMIT) {
                CapturedSubmit incoming;
                incoming.header = frame_hdr;
                incoming.payload = std::move(frame_payload);
                if (!pump_session_submits(conn, st, std::move(incoming))) {
                    return;
                }
                conn.socket->set_timeout(SESSION_FRAME_TIMEOUT_MS);
                auto next_frame = recv_encrypted(conn);
                if (!next_frame) {
                    return;  // clean close, idle timeout, or transport failure
                }
                frame_hdr = std::move(next_frame->first);
                frame_payload = std::move(next_frame->second);
                continue;
            } else if (frame_hdr.type == MessageType::MSG_TASK_CANCEL) {
                // A cancel whose task already completed (the response beat the
                // cancel to the phone, which then sent it anyway): no task is
                // running — ignore and keep the session usable.
                LOG_INFO("Coordinator", "Stray task cancel from peer " +
                         conn.device_id + " (no task running); ignored");
                conn.socket->set_timeout(SESSION_FRAME_TIMEOUT_MS);
                auto next_frame = recv_encrypted(conn);
                if (!next_frame) {
                    return;
                }
                frame_hdr = std::move(next_frame->first);
                frame_payload = std::move(next_frame->second);
                continue;
            } else if (frame_hdr.type == MessageType::MSG_MODEL_LIST_REQUEST) {
                // These ops send a (possibly large) reply and then end the
                // session, so only the receive window is worth shortening:
                // set_timeout() here also shrank SO_SNDTIMEO to 120/300 ms,
                // which aborts a model transfer of tens of MB on any link
                // slower than ~330 MB/s.
                conn.socket->set_recv_timeout(120);
                handle_model_list(conn);
                return;
            } else if (frame_hdr.type == MessageType::MSG_MODEL_UPLOAD) {
                conn.socket->set_recv_timeout(300);
                handle_model_upload(conn, frame_hdr, frame_payload);
                return;
            } else if (frame_hdr.type == MessageType::MSG_MODEL_DOWNLOAD_REQUEST) {
                conn.socket->set_recv_timeout(300);
                handle_model_download(conn, frame_hdr, frame_payload);
                return;
            } else {
                LOG_WARNING("Coordinator", "Peer " + conn.device_id +
                    " sent an unexpected session frame (type " +
                    std::to_string(static_cast<unsigned>(frame_hdr.type)) +
                    "); dropping");
                return;
            }
            }  // session frame loop
        }
    }
    // Any other first message (no handshake, no recognized type) → drop silently.
}

// ── Shared model library ops ────────────────────────────────────────────────

// Defense-in-depth: model names become filenames on the PC, so reject anything
// that could escape the models directory or hide as a dotfile.
static bool is_safe_model_name(const std::string& name) {
    if (name.empty() || name.size() >= 64) {
        return false;
    }
    if (name.front() == '.') {
        return false;
    }
    for (const char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                        c == '_' || c == '-' || c == ' ';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void Coordinator::handle_model_list(WorkerConnection& conn) {
    std::vector<ModelInfo> models;
    {
        std::lock_guard<std::mutex> lk(models_mutex_);
        auto scan = model_manager_.scan_models();
        if (!scan.has_value()) {
            LOG_ERROR("Coordinator", "Model list scan failed: " + scan.error().message);
        }
        models = model_manager_.available_models();
    }

    ModelListResponse list_resp{};
    list_resp.count = static_cast<uint32_t>(models.size());
    std::vector<uint8_t> payload(sizeof(list_resp) +
                                 models.size() * sizeof(ModelLibraryEntry));
    std::memcpy(payload.data(), &list_resp, sizeof(list_resp));
    size_t offset = sizeof(list_resp);
    for (const auto& m : models) {
        ModelLibraryEntry entry{};
        std::strncpy(entry.name, m.name.c_str(), sizeof(entry.name) - 1);
        entry.scale_factor = static_cast<uint32_t>(m.scale_factor);
        entry.total_size = static_cast<uint64_t>(m.total_size);
        std::memcpy(payload.data() + offset, &entry, sizeof(entry));
        offset += sizeof(entry);
    }
    if (!send_encrypted(conn, MessageType::MSG_MODEL_LIST_RESPONSE,
                        payload.data(), payload.size())) {
        LOG_ERROR("Coordinator", "Failed to send model list response");
        return;
    }
    LOG_INFO("Coordinator", "Sent model library list ({} models) to phone",
             models.size());
}

void Coordinator::handle_model_upload(WorkerConnection& conn,
                                      const MessageHeader& hdr,
                                      const std::vector<uint8_t>& payload) {
    (void)hdr;
    ModelOpResponse resp{};
    resp.success = 0;

    auto fail = [&](const std::string& message) {
        std::strncpy(resp.error, message.c_str(), sizeof(resp.error) - 1);
        (void)send_encrypted(conn, MessageType::MSG_MODEL_UPLOAD_RESPONSE,
                             &resp, sizeof(resp));
        LOG_ERROR("Coordinator", "Model upload rejected: " + message);
    };

    if (payload.size() < sizeof(ModelUploadRequest)) {
        fail("upload payload truncated");
        return;
    }
    ModelUploadRequest req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    const std::string name = bounded_string(req.name, sizeof(req.name));
    if (!is_safe_model_name(name)) {
        fail("invalid model name: " + name);
        return;
    }
    if (req.param_size == 0 || req.bin_size == 0 ||
        req.param_size > MAX_NETWORK_PAYLOAD || req.bin_size > MAX_NETWORK_PAYLOAD ||
        req.param_size + req.bin_size > MAX_NETWORK_PAYLOAD ||
        sizeof(req) + req.param_size + req.bin_size != payload.size()) {
        fail("invalid upload sizes");
        return;
    }

    const uint8_t* param_begin = payload.data() + sizeof(req);
    const uint8_t* bin_begin = param_begin + req.param_size;

    // Write flat into the models dir: <name>.param / <name>.bin → ModelManager
    // derives the model name from the file stem alone.
    const std::filesystem::path models_dir = model_manager_.models_directory();
    const std::filesystem::path param_path = models_dir / (name + ".param");
    const std::filesystem::path bin_path = models_dir / (name + ".bin");

    // Stage both files, then rename them into place. Truncation (full disk,
    // short write) and a crash can then never leave a half-written pair for the
    // scanner and the next inference to pick up. The whole install runs under
    // models_mutex_: it serializes two uploads of the same name (which would
    // otherwise interleave through the fixed staging names) and keeps readers
    // out of a file that is being replaced.
    const std::filesystem::path param_tmp = models_dir / (name + ".param.upix-upload");
    const std::filesystem::path bin_tmp = models_dir / (name + ".bin.upix-upload");
    // Backups of an already-installed pair. The two install renames below are
    // not atomic as a pair, so a failure after the first one would otherwise
    // leave a new .param next to an old .bin — a model that no longer loads,
    // with the working copy already destroyed. Staging the previous files
    // aside lets every failure path put them back. (The scanner matches the
    // .param/.bin extension exactly, so leftover .upix-* files are ignored.)
    const std::filesystem::path param_bak = models_dir / (name + ".param.upix-backup");
    const std::filesystem::path bin_bak = models_dir / (name + ".bin.upix-backup");
    // Refuse a destination that is not a regular file (a directory, fifo, ...):
    // installing over it would either fail halfway through the pair or move the
    // user's own entry aside as a "backup" — neither is ours to do. A symlink
    // to a regular file is fine: that is how a model name is pointed at another
    // copy. (Same rule the model downloader applies to its install.)
    auto inspect_destination = [&](const std::filesystem::path& path,
                                   bool& present) -> bool {
        std::error_code st_ec;
        const auto status = std::filesystem::symlink_status(path, st_ec);
        if (status.type() == std::filesystem::file_type::not_found &&
            (!st_ec || st_ec == std::errc::no_such_file_or_directory)) {
            present = false;
            return true;
        }
        if (st_ec) {
            return false;
        }
        present = true;
        if (status.type() == std::filesystem::file_type::regular) {
            return true;
        }
        if (status.type() == std::filesystem::file_type::symlink) {
            std::error_code target_ec;
            const auto target = std::filesystem::status(path, target_ec);
            return !target_ec &&
                   target.type() == std::filesystem::file_type::regular;
        }
        return false;
    };
    bool had_param = false;
    bool had_bin = false;
    if (!inspect_destination(param_path, had_param) ||
        !inspect_destination(bin_path, had_bin)) {
        fail("existing destination for " + name + " is not a regular file");
        return;
    }

    std::error_code write_ec;
    bool write_ok = false;
    {
        std::lock_guard<std::mutex> lk(models_mutex_);
        bool saved_param = false;
        bool saved_bin = false;
        bool installed_param = false;
        bool installed_bin = false;

        write_ok = write_file(param_tmp, param_begin, req.param_size) &&
                   write_file(bin_tmp, bin_begin, req.bin_size);
        if (write_ok && had_param) {
            std::filesystem::rename(param_path, param_bak, write_ec);
            saved_param = !write_ec;
            write_ok = saved_param;
        }
        if (write_ok && had_bin) {
            std::filesystem::rename(bin_path, bin_bak, write_ec);
            saved_bin = !write_ec;
            write_ok = saved_bin;
        }
        if (write_ok) {
            std::filesystem::rename(param_tmp, param_path, write_ec);
            installed_param = !write_ec;
            write_ok = installed_param;
        }
        if (write_ok) {
            std::filesystem::rename(bin_tmp, bin_path, write_ec);
            installed_bin = !write_ec;
            write_ok = installed_bin;
        }

        std::error_code rm_ec;
        if (write_ok) {
            if (saved_param) std::filesystem::remove(param_bak, rm_ec);
            if (saved_bin) std::filesystem::remove(bin_bak, rm_ec);
            // Rescan so the new model is immediately usable and visible to lists.
            (void)model_manager_.scan_models();
        } else {
            // Roll back: drop only what THIS attempt installed, then restore
            // only what it actually moved aside — a stale backup left by an
            // earlier crash must never overwrite a still-good installed file.
            if (installed_param) std::filesystem::remove(param_path, rm_ec);
            if (installed_bin) std::filesystem::remove(bin_path, rm_ec);
            std::filesystem::remove(param_tmp, rm_ec);
            std::filesystem::remove(bin_tmp, rm_ec);
            if (saved_param) std::filesystem::rename(param_bak, param_path, rm_ec);
            if (saved_bin) std::filesystem::rename(bin_bak, bin_path, rm_ec);
            (void)model_manager_.scan_models();
        }
    }
    if (!write_ok) {
        fail("failed to write model files: " +
             (write_ec ? write_ec.message() : std::string("short write")));
        return;
    }
    resp.success = 1;
    if (!send_encrypted(conn, MessageType::MSG_MODEL_UPLOAD_RESPONSE,
                        &resp, sizeof(resp))) {
        LOG_ERROR("Coordinator", "Failed to send upload ack for " + name);
        return;
    }
    LOG_INFO("Coordinator", "Model uploaded from phone: " + name + " (" +
             format_bytes(req.param_size + req.bin_size) + ")");
}

void Coordinator::handle_model_download(WorkerConnection& conn,
                                        const MessageHeader& hdr,
                                        const std::vector<uint8_t>& payload) {
    (void)hdr;
    ModelDownloadResponse resp{};

    auto fail = [&](const std::string& message) {
        resp.success = 0;
        resp.scale_factor = 0;
        resp.total_size = 0;
        resp.param_size = 0;
        resp.bin_size = 0;
        std::strncpy(resp.error, message.c_str(), sizeof(resp.error) - 1);
        (void)send_encrypted(conn, MessageType::MSG_MODEL_DOWNLOAD_RESPONSE,
                             &resp, sizeof(resp));
        LOG_ERROR("Coordinator", "Model download failed: " + message);
    };

    if (payload.size() < sizeof(ModelDownloadRequest)) {
        fail("download request truncated");
        return;
    }
    ModelDownloadRequest req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    const std::string name = bounded_string(req.name, sizeof(req.name));

    std::optional<ModelInfo> model;
    {
        std::lock_guard<std::mutex> lk(models_mutex_);
        auto scan = model_manager_.scan_models();
        if (!scan.has_value()) {
            fail("model scan failed: " + scan.error().message);
            return;
        }
        model = model_manager_.get_model(name);
    }
    if (!model.has_value()) {
        fail("model not found: " + name);
        return;
    }

    std::error_code ec;
    const uint64_t param_size =
        static_cast<uint64_t>(std::filesystem::file_size(model->param_path, ec));
    if (ec) {
        fail("cannot stat param file");
        return;
    }
    const uint64_t bin_size =
        static_cast<uint64_t>(std::filesystem::file_size(model->bin_path, ec));
    if (ec) {
        fail("cannot stat bin file");
        return;
    }
    // The reply is ONE encrypted frame: its wire length is the plaintext
    // (response struct + both files) plus the seal overhead, and the receiver
    // rejects any frame whose payload_length exceeds MAX_NETWORK_PAYLOAD. Cap
    // by what actually fits on the wire so an oversized model fails with a
    // clear message here instead of an opaque disconnect on the phone.
    constexpr uint64_t kSealOverhead = 24 + 16;  // secretbox nonce + MAC
    const uint64_t max_transfer_bytes =
        MAX_NETWORK_PAYLOAD - sizeof(ModelDownloadResponse) - kSealOverhead;
    if (param_size + bin_size > max_transfer_bytes) {
        fail("model too large for the single-frame transfer limit (" +
             format_bytes(param_size + bin_size) + " > " +
             format_bytes(max_transfer_bytes) + ")");
        return;
    }

    std::vector<uint8_t> param_bytes(static_cast<size_t>(param_size));
    std::vector<uint8_t> bin_bytes(static_cast<size_t>(bin_size));
    try {
        std::ifstream param_file(model->param_path, std::ios::binary);
        if (!param_file.read(reinterpret_cast<char*>(param_bytes.data()),
                             static_cast<std::streamsize>(param_size))) {
            fail("failed to read param file");
            return;
        }
        std::ifstream bin_file(model->bin_path, std::ios::binary);
        if (!bin_file.read(reinterpret_cast<char*>(bin_bytes.data()),
                           static_cast<std::streamsize>(bin_size))) {
            fail("failed to read bin file");
            return;
        }
    } catch (const std::exception& e) {
        fail(std::string("read failed: ") + e.what());
        return;
    }

    resp.success = 1;
    std::strncpy(resp.error, "", sizeof(resp.error) - 1);
    resp.scale_factor = static_cast<uint32_t>(model->scale_factor);
    resp.total_size = param_size + bin_size;
    resp.param_size = param_size;
    resp.bin_size = bin_size;

    std::vector<uint8_t> out(sizeof(resp) + param_bytes.size() + bin_bytes.size());
    std::memcpy(out.data(), &resp, sizeof(resp));
    std::memcpy(out.data() + sizeof(resp), param_bytes.data(), param_bytes.size());
    std::memcpy(out.data() + sizeof(resp) + param_bytes.size(),
                bin_bytes.data(), bin_bytes.size());

    if (!send_encrypted(conn, MessageType::MSG_MODEL_DOWNLOAD_RESPONSE,
                        out.data(), out.size())) {
        LOG_ERROR("Coordinator", "Failed to send model download response for " + name);
        return;
    }
    LOG_INFO("Coordinator", "Model sent to phone: " + name + " (" +
             format_bytes(param_size + bin_size) + ")");
}

} // namespace upix
