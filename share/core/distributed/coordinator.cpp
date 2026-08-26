#include "coordinator.hpp"
#include "core/tile_types.hpp"
#include "core/tile_optimizer.hpp"
#include "core/engine_setup.hpp"
#include "utils/image.hpp"
#include "utils/image_avif.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include "utils/crypto_sodium.hpp"
#include "utils/sr_marker.hpp"
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

Coordinator::Coordinator() = default;

Coordinator::~Coordinator() {
    stop();
}

auto Coordinator::pop_pending_approval() -> std::optional<PendingApproval> {
    std::lock_guard<std::mutex> lock(approval_mtx_);
    if (pending_approvals_.empty()) {
        return std::nullopt;
    }
    PendingApproval req = std::move(pending_approvals_.front());
    pending_approvals_.pop();
    // Keep the verdict future indexed by id so resolve_approval() can find it.
    approval_futures_[req.id] = req.verdict;
    return req;
}

void Coordinator::resolve_approval(uint64_t id, bool approved) {
    std::shared_ptr<std::promise<uint8_t>> fut;
    {
        std::lock_guard<std::mutex> lock(approval_mtx_);
        auto it = approval_futures_.find(id);
        if (it == approval_futures_.end()) {
            return;  // stale or unknown id; nothing to wake
        }
        fut = it->second;
        approval_futures_.erase(it);
    }
    if (fut) {
        fut->set_value(approved ? 1 : 0);
    }
}

auto Coordinator::start(uint16_t port, IdentityKey& identity)
    -> expected<void, CoordinatorError> {

    if (running_) {
        return {};
    }

    identity_ = &identity;
    sodium::init();

    // Load the TrustStore (TOFU bookkeeping). A missing file yields an empty
    // store, which simply means every connecting worker is flagged first-time.
    if (!trust_store_path_.empty()) {
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
    trust_store_path_ = path;
}

void Coordinator::stop() {
    if (!running_ && !server_) {
        return;
    }

    LOG_INFO("Coordinator", "Stopping coordinator...");
    running_ = false;

    // Shutdown ordering matters:
    //   1. Close the listener first — request_stop() cannot interrupt a thread
    //      blocked in a socket syscall; closing the fd makes the next 500 ms
    //      poll tick in TCPServer::accept() fail immediately (same pattern as
    //      the serve daemon).
    //   2. Then join the accept thread; its loop observes the stop token on
    //      every poll-tick wake.
    //   3. Then stop+join the client handler threads. A handler sitting in
    //      recv() wakes via its own socket timeouts and drops the connection
    //      at the next stop-token check between frames; the pairing-gate wait
    //      resolves through its promise as before.
    if (server_) {
        server_->stop();
        server_.reset();
    }

    if (accept_thread_.joinable()) {
        accept_thread_.request_stop();
        accept_thread_.join();
    }

    for (auto& t : client_threads_) {
        t.request_stop();
        if (t.joinable()) {
            t.join();
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
                                  std::function<void(float)> progress_sink)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    if (!ensure_engine_ready(model_name)) {
        return std::unexpected(CoordinatorError{2, "Engine/model not ready: " + model_name});
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
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        scale = engine_->get_scale_factor();
        out_w = width * scale;
        out_h = height * scale;
        const size_t out_bytes_n = static_cast<size_t>(out_w) * out_h * channels;
        if (out_w <= 0 || out_h <= 0 || out_bytes_n > MAX_RAW_OUTPUT_BYTES) {
            return std::unexpected(CoordinatorError{3, "Invalid output dimensions"});
        }

        // tile_size 0 = remote auto: the phone did not pin a size, so the PC
        // picks one with the same TileSizeOptimizer probe the CLI's
        // --auto-tile uses. The probe result is cached per (model, device,
        // dimension bucket) process-wide, so the one-time measurement cost
        // (~8 sizes x 7 single-tile forwards) is paid on the first submit of
        // a model, not per task; the engine recommendation is the fallback
        // if the probe cannot produce a usable size.
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

        LOG_INFO("Coordinator", "Processing locally: {}x{} model={} tile={}", width, height,
                 model_name.empty() ? loaded_model_name_ : model_name, tile_size);

        out_bytes.resize(out_bytes_n);

        TileConfig tc;
        tc.tile_size = tile_size;
        tc.overlap = config.tile_overlap;
        tc.batch_size = 1;

        // Cancellation has no external hook today (cancel_current_task was removed);
        // keep a local flag so the engine stays interruptible if one is wired in.
        std::atomic<bool> cancel{false};
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
        model_name.empty() ? loaded_model_name_ : model_name, scale,
        static_cast<int64_t>(std::time(nullptr)));
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

void Coordinator::accept_loop(std::stop_token st) {
    // The stop token is checked on every iteration; TCPServer::accept() polls
    // with a 500 ms timeout so this loop re-runs at least every 500 ms even
    // with no incoming connections.
    while (!st.stop_requested()) {
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

        client_threads_.emplace_back(
            [this, sock = std::move(client.value())](std::stop_token st) mutable {
                handle_client(st, std::move(sock));
            });
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
    // trusted=1 means "this peer was already in the TrustStore"; trusted=0
    // means "first time we're seeing this identity (TOFU)". We capture the
    // first-time flag BEFORE persisting, then record the peer so the *next*
    // connection reports trusted=1.
    const bool was_known = trust_store_ ? trust_store_->contains(c_id_pub_arr) : false;
    conn.peer_trusted = was_known;
    if (trust_store_ && !was_known) {
        trust_store_->add(c_id_pub_arr);
        trust_store_->save();
    }

    // (6) Send AUTH_RESPONSE (encrypted).
    AuthResponse resp{};
    resp.authenticated = 1;
    std::strncpy(resp.device_id, conn.device_id.c_str(), sizeof(resp.device_id) - 1);
    resp.device_id[sizeof(resp.device_id) - 1] = '\0';
    resp.trusted = was_known ? 1 : 0;  // 0 = first-time/TOFU this session
    if (!send_encrypted(conn, MessageType::MSG_AUTH_RESPONSE, &resp, sizeof(resp))) {
        LOG_WARNING("Coordinator", "Handshake aborted: failed to send AUTH_RESPONSE");
        return false;
    }

    conn.authenticated = true;
    LOG_INFO("Coordinator", "Worker handshake ok (device_id=" + conn.device_id +
             ", trusted=" + (was_known ? "1" : "0 (first-time TOFU)") + ")");
    return true;
}

// Handle one CLIENT_SUBMIT: decode the shipped file bytes, upscale on the
// local engine, encode the result and send CLIENT_RESPONSE (+CLIENT_DATA
// continuations). Per-task failures (decode/engine/encode) come back as a
// success=0 response and the session stays usable for the next submit.
// Returns false only when the transport itself failed (or the frame was
// malformed) — the caller must then drop the connection.
bool Coordinator::handle_client_submit(WorkerConnection& conn, std::stop_token& st,
                                       std::vector<uint8_t> frame_payload) {
    // Compressed-input submit: the phone ships original file bytes; decode
    // here so raw pixels never cross the radio.
    if (frame_payload.size() < sizeof(ClientTaskSubmit)) {
        LOG_ERROR("Coordinator", "Authenticated client submit payload is invalid");
        return false;
    }
    const ClientTaskSubmit* task =
        reinterpret_cast<const ClientTaskSubmit*>(frame_payload.data());
    std::string model_name = task->model_name;
    std::string output_format = task->output_format;
    if (output_format.empty()) output_format = "jpg";
    const int output_quality = task->output_quality == 0 ? 90
        : static_cast<int>(task->output_quality);
    const EncoderOptions encoder_opts = from_wire(task->encoder);
    const uint32_t data_len = task->input_data_length;
    if (data_len > MAX_NETWORK_PAYLOAD ||
        sizeof(ClientTaskSubmit) + data_len > frame_payload.size()) {
        LOG_ERROR("Coordinator", "Compressed client image payload truncated");
        return false;
    }
    std::vector<uint8_t> compressed(
        frame_payload.data() + sizeof(ClientTaskSubmit),
        frame_payload.data() + sizeof(ClientTaskSubmit) + data_len);

    auto decoded = Image::load_from_memory(task->input_format, compressed);
    compressed.clear();
    compressed.shrink_to_fit();
    if (!decoded) {
        LOG_ERROR("Coordinator", "Input decode failed: " + decoded.error().message);
        ClientTaskResponse response{};
        response.success = 0;
        std::strncpy(response.error_message,
                     ("input decode failed: " + decoded.error().message).c_str(),
                     sizeof(response.error_message) - 1);
        return send_encrypted(conn, MessageType::MSG_CLIENT_RESPONSE,
                              &response, sizeof(response));
    }
    const int img_width = decoded->width();
    const int img_height = decoded->height();
    const int channels = decoded->channels();
    std::vector<uint8_t> image_data(decoded->data(),
                                    decoded->data() + decoded->size());

    LOG_INFO(
        "Coordinator",
        "Received authenticated client task: {}x{} model={}",
        img_width, img_height, model_name
    );

    ProcessingConfig config;
    config.tile_size = static_cast<int>(task->tile_size);
    config.tile_overlap = static_cast<int>(task->tile_overlap);

    // Stream progress to the phone during the upscale. This keeps traffic
    // flowing on the connection (so NAT/stateful firewalls cannot evict it as
    // idle while the PC works for tens of seconds) and lets the phone show
    // real progress. Throttled to whole-percent changes to avoid flooding a
    // many-tile image. The progress callback runs on this same thread
    // (process_locally is synchronous), so no locking on last_sent_pct is
    // needed.
    int last_sent_pct = -1;
    // Stop requested while queued: don't start a long upscale.
    if (st.stop_requested()) {
        return false;
    }
    const auto task_start = std::chrono::steady_clock::now();
    auto progress_sink = [&conn, &last_sent_pct, &task_start, this](float frac) {
        const int pct = static_cast<int>(
            std::clamp(frac, 0.0f, 1.0f) * 100.0f);
        if (pct == last_sent_pct) {
            return;
        }
        last_sent_pct = pct;
        // Real time estimate: linear projection from observed rate
        // (same math the CLI's ETA uses).
        TaskProgress msg{};
        msg.stage = 0;
        msg.percent = static_cast<uint32_t>(pct);
        if (pct > 0 && pct < 100) {
            const double elapsed =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - task_start).count();
            msg.eta_seconds = static_cast<uint32_t>(
                elapsed / pct * (100 - pct));
        }
        // A failed progress send (peer gone) is non-fatal: processing
        // continues and the terminal response send reports the error.
        (void)send_encrypted(conn, MessageType::MSG_TASK_PROGRESS,
                             &msg, sizeof(msg));
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
        progress_sink
    );

    // Label the encode phase so the phone's UI can show
    // "encoding..." instead of a frozen 100%.
    if (process_result.has_value()) {
        TaskProgress enc{};
        enc.stage = 1;
        (void)send_encrypted(conn, MessageType::MSG_TASK_PROGRESS,
                             &enc, sizeof(enc));
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
    constexpr size_t CLIENT_CHUNK_BYTES = 8u * 1024u * 1024u;
    const auto& result_bytes = process_result.value_or(std::vector<uint8_t>{});
    const size_t leading_bytes =
        std::min<size_t>(result_bytes.size(), MAX_NETWORK_PAYLOAD);
    std::vector<uint8_t> resp_payload(sizeof(ClientTaskResponse) + leading_bytes);
    std::memcpy(resp_payload.data(), &response, sizeof(response));
    if (!result_bytes.empty()) {
        std::memcpy(resp_payload.data() + sizeof(ClientTaskResponse),
                    result_bytes.data(), leading_bytes);
    }
    const auto t_send_start = std::chrono::steady_clock::now();
    bool sent_ok = send_encrypted(conn, MessageType::MSG_CLIENT_RESPONSE,
                                  resp_payload.data(), resp_payload.size());
    for (size_t offset = resp_payload.size() - sizeof(ClientTaskResponse);
         sent_ok && offset < result_bytes.size();
         offset += CLIENT_CHUNK_BYTES) {
        const size_t n = std::min(CLIENT_CHUNK_BYTES, result_bytes.size() - offset);
        sent_ok = send_encrypted(conn, MessageType::MSG_CLIENT_DATA,
                                 result_bytes.data() + offset, n);
    }
    const auto t_send_end = std::chrono::steady_clock::now();
    if (!sent_ok) {
        LOG_ERROR("Coordinator", "Failed to send authenticated client response");
        return false;
    }
    if (process_result.has_value()) {
        const double send_s =
            std::chrono::duration<double>(t_send_end - t_send_start).count();
        const size_t sent_bytes = process_result->size();
        LOG_INFO("Coordinator", "Task completed: sent " + format_bytes(sent_bytes) +
                 " in " + format_seconds(send_s));
    } else {
        LOG_ERROR("Coordinator", "Authenticated client task failed: " + process_result.error().message);
    }
    return true;
}

void Coordinator::handle_client(std::stop_token st,
                                std::unique_ptr<TCPSocket> socket) {
    // Shutdown raced with an accepted connection: drop it immediately.
    if (st.stop_requested()) {
        return;
    }

    // First, peek the very first header to decide which kind of client this is.
    //   - MSG_KEY_HELLO → encrypted ECDH+sig handshake, then pairing gate and
    //     an authenticated CLIENT_SUBMIT / model-library op.
    // Anything else is a protocol error → drop.
    std::vector<uint8_t> first_hdr(Protocol::HEADER_SIZE);
    auto first_recv = socket->receive_all(first_hdr.data(), first_hdr.size());
    if (!first_recv.has_value()) {
        return;
    }
    auto first_parsed = Protocol::parse_header(first_hdr);
    if (!first_parsed) {
        return;
    }
    WorkerConnection conn;
    conn.socket = std::move(socket);
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

        // (Encrypted) second frame. The phone always opens the pairing gate
        // first: MSG_CLIENT_CONNECT_REQUEST, then (once approved) a third frame
        // carrying the MSG_CLIENT_SUBMIT with the image to upscale. A phone
        // that skips the gate and submits bare is tolerated as auto-approved.
        conn.socket->set_timeout(5000);
        auto second_frame = recv_encrypted(conn);
        if (!second_frame) {
            LOG_WARNING("Coordinator", "Peer " + conn.device_id +
                " did not send CONNECT_REQUEST within timeout");
        } else {
            auto [frame_hdr, frame_payload] = std::move(*second_frame);

            // ── Mobile pairing request/allow gate ───────────────────────────
            // Run only if the peer explicitly opened with CONNECT_REQUEST.
            if (frame_hdr.type == MessageType::MSG_CLIENT_CONNECT_REQUEST) {
                if (frame_payload.size() < sizeof(ClientConnectRequest)) {
                    LOG_ERROR("Coordinator", "CONNECT_REQUEST payload too short");
                    return;
                }
                const ClientConnectRequest* creq =
                    reinterpret_cast<const ClientConnectRequest*>(frame_payload.data());
                std::string req_device_name(creq->device_name);
                std::string req_fingerprint(creq->fingerprint);

                // Auto-approve already-trusted peers; first-time peers block on
                // an interactive prompt served by the PC main loop.
                bool approved = false;
                if (conn.peer_trusted) {
                    approved = true;
                    LOG_INFO("Coordinator", "Auto-approved trusted peer " +
                             req_device_name + " (" + req_fingerprint + ")");
                } else {
                    PendingApproval pa;
                    {
                        std::lock_guard<std::mutex> lock(approval_mtx_);
                        pa.id = next_approval_id_++;
                        pa.verdict = std::make_shared<std::promise<uint8_t>>();
                        approval_futures_[pa.id] = pa.verdict;
                    }
                    pa.device_name = req_device_name;
                    pa.fingerprint = req_fingerprint;
                    pa.identity_pub = conn.peer_id_pub;
                    {
                        std::lock_guard<std::mutex> lock(approval_mtx_);
                        pending_approvals_.push(pa);
                    }
                    LOG_INFO("Coordinator", "Pairing request queued from " +
                             req_device_name + " (" + req_fingerprint +
                             ") — waiting for PC approval");
                    // Block this connection thread until the PC resolves it.
                    approved = (pa.verdict->get_future().get() != 0);
                }

                ClientConnectResponse cresp{};
                cresp.approved = approved ? 1 : 0;
                cresp.trusted = conn.peer_trusted ? 1 : 0;
                // Cached dlopen probes: the phone gates AVIF input/output on
                // these instead of discovering it through task failures.
                cresp.avif_decode = avif_codec::avif_decode_available() ? 1 : 0;
                cresp.avif_encode = avif_codec::avif_encode_available() ? 1 : 0;
                if (!send_encrypted(conn, MessageType::MSG_CLIENT_CONNECT_RESPONSE,
                                    &cresp, sizeof(cresp))) {
                    LOG_ERROR("Coordinator", "Failed to send CONNECT_RESPONSE");
                    return;
                }
                if (!approved) {
                    LOG_WARNING("Coordinator", "Pairing denied for " + req_device_name);
                    return;
                }
                // First-time approval ⇒ persist so future connects auto-pass.
                if (!conn.peer_trusted && trust_store_) {
                    trust_store_->add(conn.peer_id_pub);
                    trust_store_->save();
                    LOG_INFO("Coordinator", "Peer added to TrustStore: " + req_fingerprint);
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
            // whole batch over one session (handshake/pairing once) and sends
            // submit N+1 while the PC still processes N. Model-library ops
            // stay one-shot (handle + return). Legacy clients that close
            // after the first response end the loop on the next recv (EOF).
            for (;;) {
            if (frame_hdr.type == MessageType::MSG_CLIENT_SUBMIT) {
                if (!handle_client_submit(conn, st, std::move(frame_payload))) {
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
            } else if (frame_hdr.type == MessageType::MSG_MODEL_LIST_REQUEST) {
                conn.socket->set_timeout(120);
                handle_model_list(conn);
                return;
            } else if (frame_hdr.type == MessageType::MSG_MODEL_UPLOAD) {
                conn.socket->set_timeout(300);
                handle_model_upload(conn, frame_hdr, frame_payload);
                return;
            } else if (frame_hdr.type == MessageType::MSG_MODEL_DOWNLOAD_REQUEST) {
                conn.socket->set_timeout(300);
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
    const std::string name(req.name);
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
    const std::filesystem::path param_path = model_manager_.models_directory() / (name + ".param");
    const std::filesystem::path bin_path = model_manager_.models_directory() / (name + ".bin");

    bool write_ok = true;
    try {
        {
            std::ofstream param_file(param_path, std::ios::binary | std::ios::trunc);
            param_file.write(reinterpret_cast<const char*>(param_begin),
                             static_cast<std::streamsize>(req.param_size));
            param_file.close();
            if (!param_file) write_ok = false;
        }
        {
            std::ofstream bin_file(bin_path, std::ios::binary | std::ios::trunc);
            bin_file.write(reinterpret_cast<const char*>(bin_begin),
                           static_cast<std::streamsize>(req.bin_size));
            bin_file.close();
            if (!bin_file) write_ok = false;
        }
    } catch (const std::exception& e) {
        fail(std::string("write failed: ") + e.what());
        return;
    }
    if (!write_ok) {
        fail("failed to write model files");
        return;
    }

    // Rescan so the new model is immediately usable and visible to lists.
    {
        std::lock_guard<std::mutex> lk(models_mutex_);
        (void)model_manager_.scan_models();
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
    const std::string name(req.name);

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
    if (param_size + bin_size > MAX_NETWORK_PAYLOAD) {
        fail("model too large for transfer (" + format_bytes(param_size + bin_size) + ")");
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
