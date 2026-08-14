#include "coordinator.hpp"
#include "core/tile_types.hpp"
#include "utils/image.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include "utils/crypto_sodium.hpp"
#include <thread>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <optional>

namespace crossscale {

// Maximum payload size for network messages (100 MB)
static constexpr uint32_t MAX_NETWORK_PAYLOAD = 100 * 1024 * 1024;
// Raw upscaled pixels are a transient buffer (encoded to jpg/png before
// transport), so they may exceed MAX_NETWORK_PAYLOAD. Bound PC memory instead.
static constexpr uint64_t MAX_RAW_OUTPUT_BYTES = 512ULL * 1024 * 1024;

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
        LOG_INFO("Coordinator", "TrustStore loaded (" +
                 std::to_string(trust_store_->size()) + " known peers) from " +
                 trust_store_path_.string());
    }

    server_ = std::make_unique<TCPServer>();

    auto result = server_->listen(port);
    if (!result.has_value()) {
        return std::unexpected(CoordinatorError{1, "Failed to start server: " + result.error().message});
    }

    port_ = port;
    running_ = true;
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    accept_thread_ = std::thread(&Coordinator::accept_loop, this);

    LOG_INFO("Coordinator", "Server started on port " + std::to_string(port_));
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

    if (server_) {
        server_->stop();
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    
    // Join all client handler threads
    {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        for (auto& t : client_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        client_threads_.clear();
    }

    if (server_) {
        server_.reset();
    }

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
        engine_ = create_ncnn_engine();
        auto init_res = engine_->initialize(DeviceType::GPU_VULKAN);
        if (!init_res) {
            LOG_WARNING("Coordinator", "GPU unavailable, falling back to CPU: " + init_res.error().message);
            auto cpu_res = engine_->initialize(DeviceType::CPU);
            if (!cpu_res) {
                LOG_ERROR("Coordinator", "engine initialize failed: " + cpu_res.error().message);
                return false;
            }
        }
        tile_processor_ = std::make_unique<TileProcessor>();
        engine_initialized_ = true;
        LOG_INFO("Coordinator", "Local engine ready on " +
                 std::string(engine_->current_device() == DeviceType::GPU_VULKAN ? "GPU" : "CPU"));
    }

    // Empty model name = "use whatever is loaded" (caller didn't specify).
    if (model_name.empty() || model_name == loaded_model_name_) {
        return true;
    }
    auto info = model_manager_.get_model(model_name);
    if (!info) {
        LOG_ERROR("Coordinator", "unknown model requested: " + model_name);
        return false;
    }
    auto load_res = engine_->load_model(info->param_path.string(), info->bin_path.string());
    if (!load_res) {
        LOG_ERROR("Coordinator", "model load failed: " + load_res.error().message);
        return false;
    }
    loaded_model_name_ = model_name;
    LOG_INFO("Coordinator", "switched model to " + model_name);
    return true;
}

// Upscale a submitted image on the PC's own engine. image_data is raw u8 pixels.
// Replaces the old multi-worker distribute_task — no tile dispatch, no workers.
auto Coordinator::process_locally(const std::vector<uint8_t>& image_data,
                                  int width, int height, int channels,
                                  const std::string& model_name,
                                  const ProcessingConfig& config,
                                  const std::string& output_format,
                                  int output_quality)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    if (!ensure_engine_ready(model_name)) {
        return std::unexpected(CoordinatorError{2, "Engine/model not ready: " + model_name});
    }

    // Inference is serialized under engine_mutex_ (Vulkan is not concurrency-safe).
    std::lock_guard<std::mutex> lk(engine_mutex_);
    const int scale = engine_->get_scale_factor();
    const int out_w = width * scale;
    const int out_h = height * scale;
    const size_t out_bytes_n = static_cast<size_t>(out_w) * out_h * channels;
    if (out_w <= 0 || out_h <= 0 || out_bytes_n > MAX_RAW_OUTPUT_BYTES) {
        return std::unexpected(CoordinatorError{3, "Invalid output dimensions"});
    }

    LOG_INFO("Coordinator", "Processing locally: " + std::to_string(width) + "x" +
             std::to_string(height) + " model=" + (model_name.empty() ? loaded_model_name_ : model_name) +
             " tile=" + std::to_string(config.tile_size));

    std::vector<uint8_t> out_bytes(out_bytes_n);

    TileConfig tc;
    tc.tile_size = config.tile_size;
    tc.overlap = config.tile_overlap;
    tc.batch_size = 1;

    // Cancellation has no external hook today (cancel_current_task was removed);
    // keep a local flag so the engine stays interruptible if one is wired in.
    std::atomic<bool> cancel{false};
    auto res = tile_processor_->process_image_u8(
        image_data.data(), out_bytes.data(),
        width, height, channels,
        *engine_, tc,
        [this](const ProcessingProgress& p) {
            if (on_task_progress_) {
                on_task_progress_(p.percentage() / 100.0f);
            }
        },
        cancel,
        /*enable_alpha_bicubic=*/true);

    if (!res) {
        return std::unexpected(CoordinatorError{4, res.error().message});
    }

    // Encode before transport: a camera photo at x2 is hundreds of MB raw but
    // only a few MB as jpg/webp. The phone forwards these bytes to Kotlin as-is.
    //   jpg/webp — lossy; output_quality (default 90) is real compression.
    //   png      — lossless; quality only selects zlib level (inverted), so we
    //              force max compression for transport. PNG can't shrink photos
    //              the way lossy codecs do; the size guard below rejects the
    //              case where even max-compressed PNG exceeds the transport cap.
    int transport_quality = output_quality;
    if (output_format == "png") {
        transport_quality = 1;  // → zlib level 9 via png_compression_level_from_quality
    }
    Image out_image(out_w, out_h, channels);
    std::memcpy(out_image.data(), out_bytes.data(), out_bytes.size());
    auto encoded = out_image.save_to_memory(output_format, transport_quality);
    if (!encoded) {
        return std::unexpected(CoordinatorError{5, "output encode failed: " + encoded.error().message});
    }
    if (encoded->size() > MAX_NETWORK_PAYLOAD) {
        return std::unexpected(CoordinatorError{
            6, "Encoded output too large for transport (" +
               std::to_string(encoded->size() / (1024 * 1024)) +
               " MB); use jpg or webp for large images"});
    }
    return *encoded;
}

void Coordinator::accept_loop() {
    while (running_) {
        auto client = server_->accept();
        if (!client.has_value()) {
            // EAGAIN/ETIMEDOUT is the normal poll-timeout wake-up: loop back
            // and re-check running_. Only log genuinely unexpected errors.
            const int code = client.error().code;
            if (running_ && code != EAGAIN && code != ETIMEDOUT) {
                LOG_ERROR("Coordinator", "Accept failed: " + client.error().message);
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(client_threads_mutex_);
            // Clean up finished threads
            client_threads_.erase(
                std::remove_if(client_threads_.begin(), client_threads_.end(),
                    [](const std::thread& t) { return !t.joinable(); }),
                client_threads_.end());
            client_threads_.emplace_back(
                &Coordinator::handle_client, this, std::move(client.value()));
        }
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
    auto parsed = Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
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
// the 15 plaintext bytes of the client's KEY_HELLO header (peeked by the
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

void Coordinator::handle_client(std::unique_ptr<TCPSocket> socket) {

    // First, peek the very first header to decide which kind of client this is.
    //   - MSG_KEY_HELLO   → worker running the encrypted ECDH+sig handshake.
    //   - MSG_CLIENT_SUBMIT → legacy unauthenticated client submit (one-shot
    //     request over a fresh TCP connection; no session is ever established).
    // Anything else is a protocol error → drop.
    std::vector<uint8_t> first_hdr(Protocol::HEADER_SIZE);
    auto first_recv = socket->receive_all(first_hdr.data(), first_hdr.size());
    if (!first_recv.has_value()) {
        return;
    }
    auto first_parsed = Protocol::parse_header(first_hdr.data(), first_hdr.size());
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
                cresp.stream_encrypted = 0;  // stream encryption removed; wire field kept for compat
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

                // Now read the submit frame (third frame after the request):
                // the phone sends MSG_CLIENT_SUBMIT with the image to upscale.
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

            if (frame_hdr.type == MessageType::MSG_CLIENT_SUBMIT) {
                if (frame_payload.size() < sizeof(ClientTaskSubmit)) {
                    LOG_ERROR("Coordinator", "Authenticated client submit payload is invalid");
                    return;
                }

                const ClientTaskSubmit* task =
                    reinterpret_cast<const ClientTaskSubmit*>(frame_payload.data());
                uint32_t img_width = task->image_width;
                uint32_t img_height = task->image_height;
                uint8_t channels = task->channels;
                std::string model_name(task->model_name);
                uint32_t tile_size = task->tile_size;
                uint32_t tile_overlap = task->tile_overlap;
                uint32_t image_data_len = task->image_data_length;
                std::string output_format(task->output_format);
                if (output_format.empty()) {
                    output_format = "jpg";
                }
                int output_quality = task->output_quality == 0 ? 90 : task->output_quality;

                if (image_data_len > MAX_NETWORK_PAYLOAD) {
                    LOG_ERROR("Coordinator", "Image data too large: " + std::to_string(image_data_len) + " bytes");
                    return;
                }
                if (img_width == 0 || img_height == 0 || img_width > 65536 || img_height > 65536) {
                    LOG_ERROR("Coordinator", "Invalid image dimensions: " + std::to_string(img_width) + "x" + std::to_string(img_height));
                    return;
                }
                if (channels == 0 || channels > 4) {
                    LOG_ERROR("Coordinator", "Invalid channel count: " + std::to_string(channels));
                    return;
                }

                // The image payload follows the ClientTaskSubmit struct inside
                // the same encrypted frame (worker packs them contiguously).
                const size_t submit_hdr_size = sizeof(ClientTaskSubmit);
                if (submit_hdr_size + image_data_len > frame_payload.size()) {
                    LOG_ERROR("Coordinator", "Authenticated client image payload truncated");
                    return;
                }
                std::vector<uint8_t> image_data(
                    frame_payload.data() + submit_hdr_size,
                    frame_payload.data() + submit_hdr_size + image_data_len);

                LOG_INFO(
                    "Coordinator",
                    "Received authenticated client task: " +
                        std::to_string(img_width) + "x" + std::to_string(img_height) +
                        " model=" + model_name
                );

                ProcessingConfig config;
                config.tile_size = static_cast<int>(tile_size);
                config.tile_overlap = static_cast<int>(tile_overlap);

                auto process_result = process_locally(
                    image_data,
                    static_cast<int>(img_width),
                    static_cast<int>(img_height),
                    static_cast<int>(channels),
                    model_name,
                    config,
                    output_format,
                    output_quality
                );

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
                std::vector<uint8_t> resp_payload(sizeof(ClientTaskResponse) +
                                                  process_result.value_or(std::vector<uint8_t>{}).size());
                std::memcpy(resp_payload.data(), &response, sizeof(response));
                if (process_result.has_value()) {
                    std::memcpy(resp_payload.data() + sizeof(ClientTaskResponse),
                                process_result->data(), process_result->size());
                }
                if (!send_encrypted(conn, MessageType::MSG_CLIENT_RESPONSE,
                                    resp_payload.data(), resp_payload.size())) {
                    LOG_ERROR("Coordinator", "Failed to send authenticated client response");
                    return;
                }
                if (process_result.has_value()) {
                    LOG_INFO("Coordinator", "Authenticated client task completed, sent " +
                             std::to_string(process_result->size()) + " bytes");
                } else {
                    LOG_ERROR("Coordinator", "Authenticated client task failed: " + process_result.error().message);
                }
                return;
            } else {
                // Unknown second frame; already drained via recv_encrypted.
            }
        }
    }
    // Any other first message (no handshake, no recognized type) → drop silently.
}

} // namespace crossscale
