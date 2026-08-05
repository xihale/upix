#include "coordinator.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include "utils/crypto_sodium.hpp"
#include <thread>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <optional>
#include <numeric>
#include <deque>
#include <unordered_set>

namespace crossscale {

// Maximum payload size for network messages (100 MB)
static constexpr uint32_t MAX_NETWORK_PAYLOAD = 100 * 1024 * 1024;

Coordinator::Coordinator() = default;

Coordinator::~Coordinator() {
    stop();
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
    server_name_ = "CrossScale-Server";
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
    
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.clear();
    }
    
    if (server_) {
        server_.reset();
    }

    LOG_INFO("Coordinator", "Coordinator stopped");
}

bool Coordinator::is_running() const {
    return running_;
}

auto Coordinator::get_connected_devices() -> std::vector<DeviceProfile> {
    return scheduler_.get_all_devices();
}

auto Coordinator::device_count() const -> size_t {
    return scheduler_.device_count();
}

void Coordinator::cancel_current_task() {
    task_cancelled_ = true;
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

    // Helper: read N plaintext bytes off the socket (used for the legacy
    // unauthenticated CLIENT_SUBMIT path only).
    auto read_payload = [&](uint32_t payload_length) -> std::optional<std::vector<uint8_t>> {
        if (payload_length > MAX_NETWORK_PAYLOAD) {
            LOG_ERROR("Coordinator", "Payload too large: " + std::to_string(payload_length) + " bytes");
            return std::nullopt;
        }
        std::vector<uint8_t> payload(payload_length);
        if (payload_length == 0) {
            return payload;
        }
        auto payload_result = conn.socket->receive_all(payload.data(), payload.size());
        if (!payload_result.has_value()) {
            return std::nullopt;
        }
        return payload;
    };

    auto parse_device_profile = [](const DeviceProfileMessage& msg, const std::string& fallback_device_id) {
        DeviceProfile profile;
        profile.device_id = fallback_device_id;
        profile.device_name = std::string(msg.device_name);
        profile.type = static_cast<DeviceType>(msg.device_type);
        profile.tiles_per_second = std::max(msg.tiles_per_second, 1.0f);
        profile.recommended_tile_size = static_cast<int>(msg.recommended_tile_size);
        profile.max_batch_size = static_cast<size_t>(msg.max_batch_size);
        profile.ip_address = std::string(msg.ip_address);
        profile.port = msg.port;
        profile.last_heartbeat = std::chrono::steady_clock::now();
        return profile;
    };

    // ── Branch on the first frame ────────────────────────────────────────────
    if (first_parsed->type == MessageType::MSG_KEY_HELLO) {
        // ── Encrypted worker handshake path ──
        if (first_parsed->payload_length != sizeof(KeyHelloMessage)) {
            return;
        }
        if (!run_handshake(conn)) {
            return;
        }

        // Seed a fallback profile; overwritten by the worker's DEVICE_PROFILE.
        conn.profile.device_id = conn.device_id;
        conn.profile.device_name = "CLI-Worker";
        conn.profile.type = DeviceType::CPU;
        conn.profile.tiles_per_second = 1.0f;
        conn.profile.recommended_tile_size = 256;
        conn.profile.max_batch_size = 1;
        conn.profile.ip_address = conn.socket->peer_address();
        conn.profile.port = conn.socket->peer_port();
        conn.profile.last_heartbeat = std::chrono::steady_clock::now();

        // (Encrypted) second frame: DEVICE_PROFILE or CLIENT_SUBMIT.
        conn.socket->set_timeout(5000);
        auto second_frame = recv_encrypted(conn);
        if (!second_frame) {
            LOG_WARNING("Coordinator", "Worker " + conn.device_id +
                " did not send DEVICE_PROFILE within timeout (engine benchmark too slow?)");
        } else {
            auto& [frame_hdr, frame_payload] = *second_frame;
            if (frame_hdr.type == MessageType::MSG_DEVICE_PROFILE) {
                if (frame_payload.size() >= sizeof(DeviceProfileMessage)) {
                    const DeviceProfileMessage* msg =
                        reinterpret_cast<const DeviceProfileMessage*>(frame_payload.data());
                    conn.profile = parse_device_profile(*msg, conn.device_id);
                }
            } else if (frame_hdr.type == MessageType::MSG_CLIENT_SUBMIT) {
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

                auto process_result = distribute_task(
                    image_data,
                    static_cast<int>(img_width),
                    static_cast<int>(img_height),
                    static_cast<int>(channels),
                    model_name,
                    2,
                    config
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

                // Response header + result data go in one encrypted frame, the
                // same way the legacy path packed them contiguously on the wire.
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

        const std::string worker_id = conn.device_id;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            scheduler_.register_device(conn.profile);
            workers_[worker_id] = std::move(conn);
            auto it = workers_.find(worker_id);
            if (it != workers_.end()) {
                send_model_list(it->second);
            }
        }

        LOG_INFO("Coordinator", "Worker connected: " + worker_id);

        if (on_device_connected_) {
            std::optional<DeviceProfile> connected_profile;
            {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                auto it = workers_.find(worker_id);
                if (it != workers_.end()) {
                    connected_profile = it->second.profile;
                }
            }
            if (connected_profile.has_value()) {
                on_device_connected_(connected_profile.value());
            }
        }
    }
    else if (first_parsed->type == MessageType::MSG_CLIENT_SUBMIT) {
        // ── Legacy unauthenticated client submit (plaintext, one-shot) ──
        // This path is intentionally NOT encrypted: it is a single request over
        // a fresh TCP connection with no handshake, used by the local CLI client.
        auto submit_payload = read_payload(first_parsed->payload_length);
        if (!submit_payload.has_value() || submit_payload->size() < sizeof(ClientTaskSubmit)) {
            LOG_ERROR("Coordinator", "Client submit payload is invalid");
            return;
        }

        const ClientTaskSubmit* task = reinterpret_cast<const ClientTaskSubmit*>(submit_payload->data());
        uint32_t img_width = task->image_width;
        uint32_t img_height = task->image_height;
        uint8_t channels = task->channels;
        std::string model_name(task->model_name);
        uint32_t tile_size = task->tile_size;
        uint32_t tile_overlap = task->tile_overlap;
        uint32_t image_data_len = task->image_data_length;

        if (image_data_len > MAX_NETWORK_PAYLOAD) {
            LOG_ERROR("Coordinator", "Image data too large: " + std::to_string(image_data_len) + " bytes");
            return;
        }

        std::vector<uint8_t> image_data(image_data_len);
        auto image_receive = conn.socket->receive_all(image_data.data(), image_data_len);
        if (!image_receive.has_value()) {
            LOG_ERROR("Coordinator", "Failed to receive client image payload: " + image_receive.error().message);
            return;
        }

        LOG_INFO("Coordinator", "Received client task: " +
                 std::to_string(img_width) + "x" + std::to_string(img_height) +
                 " model=" + model_name);

        ProcessingConfig config;
        config.tile_size = static_cast<int>(tile_size);
        config.tile_overlap = static_cast<int>(tile_overlap);

        auto result = distribute_task(
            image_data,
            static_cast<int>(img_width),
            static_cast<int>(img_height),
            static_cast<int>(channels),
            model_name,
            2,
            config
        );

        ClientTaskResponse resp{};
        if (result.has_value()) {
            resp.success = 1;
            resp.result_data_length = static_cast<uint32_t>(result->size());

            MessageHeader resp_header{};
            resp_header.type = MessageType::MSG_CLIENT_RESPONSE;
            resp_header.payload_length = sizeof(resp);

            auto header_data = Protocol::serialize(resp_header, &resp);
            auto send_header_result = conn.socket->send_all(header_data.data(), header_data.size());
            if (!send_header_result.has_value()) {
                LOG_ERROR("Coordinator", "Failed to send client response header: " + send_header_result.error().message);
                return;
            }

            auto send_result_data = conn.socket->send_all(result->data(), result->size());
            if (!send_result_data.has_value()) {
                LOG_ERROR("Coordinator", "Failed to send client result data: " + send_result_data.error().message);
                return;
            }

            LOG_INFO("Coordinator", "Client task completed, sent " +
                     std::to_string(result->size()) + " bytes");
        } else {
            resp.success = 0;
            resp.result_data_length = 0;
            std::strncpy(resp.error_message, result.error().message.c_str(),
                         sizeof(resp.error_message) - 1);

            MessageHeader resp_header{};
            resp_header.type = MessageType::MSG_CLIENT_RESPONSE;
            resp_header.payload_length = sizeof(resp);

            auto header_data = Protocol::serialize(resp_header, &resp);
            auto send_header_result = conn.socket->send_all(header_data.data(), header_data.size());
            if (!send_header_result.has_value()) {
                LOG_ERROR("Coordinator", "Failed to send client failure response: " + send_header_result.error().message);
                return;
            }

            LOG_ERROR("Coordinator", "Client task failed: " + result.error().message);
        }
    }
    // Unknown first message → drop silently.
}

} // namespace crossscale
