#include "worker.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include "utils/crypto_sodium.hpp"
#include <thread>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

auto default_recommended_weight(crossscale::DeviceType device_type) -> float {
#if defined(__ANDROID__)
    if (device_type == crossscale::DeviceType::GPU_VULKAN) {
        return 2.0f;
    }
    return 2.0f;
#else
    if (device_type == crossscale::DeviceType::GPU_VULKAN) {
        return 5.0f;
    }
    return 4.0f;
#endif
}

auto default_queue_depth(crossscale::DeviceType device_type) -> size_t {
#if defined(__ANDROID__)
    if (device_type == crossscale::DeviceType::GPU_VULKAN) {
        return 2;
    }
    return 2;
#else
    if (device_type == crossscale::DeviceType::GPU_VULKAN) {
        return 5;
    }
    return 3;
#endif
}

auto default_recommended_tile_size(crossscale::DeviceType device_type) -> int {
    if (device_type == crossscale::DeviceType::GPU_VULKAN) {
        return 256;
    }
    return 192;
}

}

namespace crossscale {

// Maximum payload size for network messages (100 MB)
static constexpr uint32_t MAX_NETWORK_PAYLOAD = 100 * 1024 * 1024;

Worker::Worker() = default;

Worker::~Worker() {
    disconnect();
}

auto Worker::connect(const std::string& host, uint16_t port,
                     const std::array<uint8_t, 32>& peer_identity_pubkey,
                     IdentityKey& local_identity)
    -> expected<void, WorkerError> {

    socket_ = std::make_unique<TCPSocket>();

    auto result = socket_->connect(host, port);
    if (!result.has_value()) {
        return std::unexpected(WorkerError{1, "Connection failed: " + result.error().message});
    }

    peer_id_pub_ = peer_identity_pubkey;
    local_identity_ = &local_identity;
    running_ = true;

    LOG_INFO("Worker", "Connected to " + host + ":" + std::to_string(port));
    return {};
}

void Worker::set_recommended_weight(float weight) {
    recommended_weight_override_ = std::max(weight, 0.0f);
}

void Worker::set_queue_depth(size_t queue_depth) {
    queue_depth_override_ = queue_depth;
}

void Worker::set_recommended_tile_size(int tile_size) {
    recommended_tile_size_override_ = std::max(tile_size, 0);
}

auto Worker::handshake(Engine& engine) -> expected<void, WorkerError> {
    if (!socket_) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }
    if (!local_identity_) {
        return std::unexpected(WorkerError{2, "No local identity configured"});
    }

    sodium::init();

    // ── (1) Generate ephemeral X25519 keypair, send KEY_HELLO (plaintext) ──
    auto eph = sodium::x25519_keygen();
    const auto c_eph_pub = eph.public_key;
    const auto c_id_pub = local_identity_->public_key();

    KeyHelloMessage c_hello{};
    std::memcpy(c_hello.eph_pub, c_eph_pub.data(), 32);
    std::memcpy(c_hello.id_pub, c_id_pub.data(), 32);
    std::memset(c_hello.sig, 0, 64);  // client's hello carries no signature

    {
        MessageHeader hh{};
        hh.type = MessageType::MSG_KEY_HELLO;
        hh.payload_length = sizeof(c_hello);
        auto wire = Protocol::serialize(hh, &c_hello);
        auto send_res = socket_->send_all(wire.data(), wire.size());
        if (!send_res.has_value()) {
            return std::unexpected(WorkerError{100, "KEY_HELLO send failed: " + send_res.error().message});
        }
    }

    // ── (2) Receive server KEY_HELLO with its signature ──
    KeyHelloMessage s_hello{};
    {
        std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
        auto hdr_recv = socket_->receive_all(hdr_buf.data(), hdr_buf.size());
        if (!hdr_recv.has_value()) {
            return std::unexpected(WorkerError{101, "KEY_HELLO header recv failed: " + hdr_recv.error().message});
        }
        auto parsed_hdr = Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
        if (!parsed_hdr || parsed_hdr->type != MessageType::MSG_KEY_HELLO ||
            parsed_hdr->payload_length != sizeof(s_hello)) {
            return std::unexpected(WorkerError{102, "Unexpected message during handshake"});
        }
        std::vector<uint8_t> payload(parsed_hdr->payload_length);
        auto pay_recv = socket_->receive_all(payload.data(), payload.size());
        if (!pay_recv.has_value()) {
            return std::unexpected(WorkerError{101, "KEY_HELLO payload recv failed: " + pay_recv.error().message});
        }
        std::memcpy(&s_hello, payload.data(), sizeof(s_hello));
    }

    // ── (3) Build transcript c_eph||c_id||s_eph||s_id (128 bytes) ──
    // Both sides MUST construct this identical byte sequence.
    std::array<uint8_t, 128> transcript{};
    std::memcpy(transcript.data() + 0,   c_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 32,  c_hello.id_pub,  32);
    std::memcpy(transcript.data() + 64,  s_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 96,  s_hello.id_pub,  32);

    // ── (4) MITM check: server's identity pubkey must match the one we expect ──
    const std::array<uint8_t, 32> s_id_pub_arr = [&] {
        std::array<uint8_t, 32> a{};
        std::memcpy(a.data(), s_hello.id_pub, 32);
        return a;
    }();
    if (s_id_pub_arr != peer_id_pub_) {
        return std::unexpected(WorkerError{104, "Server identity pubkey mismatch (possible MITM)"});
    }

    // ── (5) Verify server signature over the transcript ──
    std::array<uint8_t, 64> s_sig_arr{};
    std::memcpy(s_sig_arr.data(), s_hello.sig, 64);
    if (!sodium::ed25519_verify(s_id_pub_arr, s_sig_arr, transcript.data(), transcript.size())) {
        return std::unexpected(WorkerError{105, "Server handshake signature verification failed"});
    }

    // ── (6) Derive session key and activate EncryptedSession ──
    std::array<uint8_t, 32> s_eph_pub_arr{};
    std::memcpy(s_eph_pub_arr.data(), s_hello.eph_pub, 32);
    auto session_key = sodium::x25519_shared(eph.private_key, s_eph_pub_arr);
    session_.activate(session_key);

    // ── (7) Sign the same transcript with our identity, send AUTH_PROOF (encrypted) ──
    auto c_sig = local_identity_->sign(transcript.data(), transcript.size());
    AuthProofMessage proof{};
    std::memcpy(proof.sig, c_sig.data(), 64);
    {
        auto send_res = send_encrypted(MessageType::MSG_AUTH_PROOF, &proof, sizeof(proof));
        if (!send_res.has_value()) {
            return std::unexpected(send_res.error());
        }
    }

    // ── (8) Receive AUTH_RESPONSE (encrypted) ──
    auto resp_frame = recv_encrypted();
    if (!resp_frame.has_value()) {
        return std::unexpected(resp_frame.error());
    }
    auto& [resp_hdr, resp_payload] = *resp_frame;
    if (resp_hdr.type != MessageType::MSG_AUTH_RESPONSE ||
        resp_payload.size() < sizeof(AuthResponse)) {
        return std::unexpected(WorkerError{103, "Authentication rejected"});
    }
    const auto* auth_resp = reinterpret_cast<const AuthResponse*>(resp_payload.data());
    if (!auth_resp->authenticated) {
        return std::unexpected(WorkerError{103, "Authentication rejected"});
    }
    device_id_ = auth_resp->device_id;
    peer_trusted_ = (auth_resp->trusted != 0);

    LOG_INFO("Worker", "Encrypted handshake complete (device_id=" + device_id_ +
             ", peer_trusted=" + (peer_trusted_ ? "1" : "0") + ")");

    // ── (9) Build and send the device profile (encrypted) ──
    DeviceProfile profile;
    profile.device_id = device_id_;
    profile.device_name = "CLI-Worker";
    profile.type = engine.current_device();
    const float benchmark_tiles_per_second = engine.benchmark(256);
    const float default_weight = default_recommended_weight(profile.type);
    profile.tiles_per_second = recommended_weight_override_ > 0.0f
        ? recommended_weight_override_
        : std::max(benchmark_tiles_per_second, default_weight);
    profile.recommended_tile_size = recommended_tile_size_override_ > 0
        ? recommended_tile_size_override_
        : default_recommended_tile_size(profile.type);
    profile.max_batch_size = queue_depth_override_ > 0
        ? queue_depth_override_
        : default_queue_depth(profile.type);
    profile.ip_address = socket_->peer_address();
    profile.port = socket_->peer_port();

    return send_profile(profile);
}

// ── Encrypted-frame helpers ──────────────────────────────────────────────────
// After handshake() activates session_, every wire frame is a secretbox
// ciphertext. These two helpers concentrate the seal/open boilerplate so the
// individual message handlers don't repeat header-read + parse + open logic.

auto Worker::send_encrypted(MessageType type, const void* payload, size_t len)
    -> expected<void, WorkerError> {
    MessageHeader header{};
    header.type = type;
    header.payload_length = static_cast<uint32_t>(len);
    auto wire = session_.seal(header, payload);
    auto res = socket_->send_all(wire.data(), wire.size());
    if (!res.has_value()) {
        return std::unexpected(WorkerError{3, "Encrypted send failed: " + res.error().message});
    }
    return {};
}

auto Worker::recv_encrypted()
    -> expected<std::pair<MessageHeader, std::vector<uint8_t>>, WorkerError> {
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    auto hdr_recv = socket_->receive_all(hdr_buf.data(), hdr_buf.size());
    if (!hdr_recv.has_value()) {
        return std::unexpected(WorkerError{4, "Header recv failed: " + hdr_recv.error().message});
    }
    auto parsed = Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
    if (!parsed) {
        std::string hex;
        hex.reserve(Protocol::HEADER_SIZE * 3);
        static const char* kHex = "0123456789ABCDEF";
        for (size_t i = 0; i < Protocol::HEADER_SIZE; ++i) {
            hex += kHex[(hdr_buf[i] >> 4) & 0xF];
            hex += kHex[hdr_buf[i] & 0xF];
            hex += ' ';
        }
        LOG_ERROR("Worker", "Invalid message header — first " +
            std::to_string(Protocol::HEADER_SIZE) + " bytes: " + hex +
            "(expected magic 0x53534358 \"XCSS\")");
        return std::unexpected(WorkerError{5, "Invalid message header"});
    }
    if (parsed->payload_length > MAX_NETWORK_PAYLOAD) {
        return std::unexpected(WorkerError{7, "Payload too large"});
    }
    std::vector<uint8_t> wire(Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        auto pay_recv = socket_->receive_all(wire.data() + Protocol::HEADER_SIZE, parsed->payload_length);
        if (!pay_recv.has_value()) {
            return std::unexpected(WorkerError{4, "Payload recv failed: " + pay_recv.error().message});
        }
    }
    auto opened = session_.open(wire.data(), wire.size());
    if (!opened) {
        return std::unexpected(WorkerError{5, "Encrypted frame MAC failed or session inactive"});
    }
    return std::move(*opened);
}

void Worker::disconnect() {
    running_ = false;
    stop_heartbeat();

    if (socket_) {
        if (socket_->is_connected() && session_.active()) {
            // Best-effort encrypted DISCONNECT. Failures are logged but never
            // block teardown.
            try {
                MessageHeader header;
                header.type = MessageType::MSG_DISCONNECT;
                header.payload_length = 0;
                auto data = session_.seal(header, nullptr);
                auto send_result = socket_->send_all(data.data(), data.size());
                if (!send_result.has_value()) {
                    LOG_WARNING("Worker", "Failed to notify coordinator before disconnect: " + send_result.error().message);
                }
            } catch (const std::exception& e) {
                LOG_WARNING("Worker", std::string("Could not send encrypted DISCONNECT: ") + e.what());
            }
        }
        socket_->close();
        socket_.reset();
    }
}

bool Worker::is_connected() const {
    return socket_ && socket_->is_connected() && running_;
}

auto Worker::send_profile(const DeviceProfile& profile) -> expected<void, WorkerError> {
    if (!is_connected()) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }

    DeviceProfileMessage msg;
    std::strncpy(msg.device_id, profile.device_id.c_str(), sizeof(msg.device_id) - 1);
    std::strncpy(msg.device_name, profile.device_name.c_str(), sizeof(msg.device_name) - 1);
    msg.device_type = static_cast<uint8_t>(profile.type);
    msg.tiles_per_second = profile.tiles_per_second;
    msg.recommended_tile_size = profile.recommended_tile_size;
    msg.max_batch_size = profile.max_batch_size;
    std::strncpy(msg.ip_address, profile.ip_address.c_str(), sizeof(msg.ip_address) - 1);
    msg.port = profile.port;

    return send_encrypted(MessageType::MSG_DEVICE_PROFILE, &msg, sizeof(msg));
}

auto Worker::receive_task() -> expected<Task, WorkerError> {
    if (!is_connected()) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }

    auto frame = recv_encrypted();
    if (!frame.has_value()) {
        return std::unexpected(frame.error());
    }
    auto& [header, payload] = *frame;

    if (header.type == MessageType::MSG_TASK_ASSIGNMENT) {
        TaskAssignmentMessage* msg = reinterpret_cast<TaskAssignmentMessage*>(payload.data());

        Task task;
        task.task_id = msg->task_id;
        task.model_name = msg->model_name;

        size_t tile_info_offset = sizeof(TaskAssignmentMessage);
        for (uint32_t i = 0; i < msg->tile_count; ++i) {
            TileInfo* tile = reinterpret_cast<TileInfo*>(payload.data() + tile_info_offset + i * sizeof(TileInfo));
            (void)tile;
            task.tile_indices.push_back(i);
        }

        // Lazy model fetch: if the model files aren't present locally, pull them
        // from the coordinator NOW — before we send TASK_ACCEPT. The
        // coordinator's dispatch loop serves MSG_MODEL_REQUEST here (it has not
        // sent the TILES_BATCH yet, since that only goes out after ACCEPT), so
        // this exchange cannot collide with in-flight batch bytes. Doing the
        // fetch here (rather than during connect-time sync_models) avoids the
        // deadlock where no thread was reading the worker socket.
        if (!task.model_name.empty() && !model_manager_.has_model(task.model_name)) {
            LOG_INFO("Worker", "Task requires model " + task.model_name +
                     " which is missing locally — fetching from coordinator");
            auto param_dl = download_model(task.model_name, 0);
            if (!param_dl.has_value()) {
                return std::unexpected(WorkerError{10, "Failed to fetch model param " + task.model_name +
                                      ": " + param_dl.error().message});
            }
            auto bin_dl = download_model(task.model_name, 1);
            if (!bin_dl.has_value()) {
                return std::unexpected(WorkerError{10, "Failed to fetch model bin " + task.model_name +
                                      ": " + bin_dl.error().message});
            }
            (void)model_manager_.scan_models();
            LOG_INFO("Worker", "Model " + task.model_name + " fetched and ready");
        }

        if (on_task_received_) {
            on_task_received_(task);
        }

        return task;
    }

    return std::unexpected(WorkerError{6, "Unexpected message type"});
}

auto Worker::execute_task(const Task& task, Engine& engine,
                            std::function<void(float)> progress_callback)
    -> expected<std::vector<uint8_t>, WorkerError> {

    if (!is_connected()) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }

    // Send TASK_ACCEPT (encrypted). Payload: task_id + NUL terminator (same
    // wire shape as the old plaintext path).
    const std::string accept_payload = task.task_id + '\0';
    auto accept_res = send_encrypted(MessageType::MSG_TASK_ACCEPT,
                                     accept_payload.data(), accept_payload.size());
    if (!accept_res.has_value()) {
        return std::unexpected(WorkerError{8, "Failed to send task acceptance"});
    }

    if (engine.model_name() != task.model_name) {
        std::filesystem::path models_dir = model_manager_.models_directory();
        if (models_dir.empty()) {
            models_dir = "./models";
        }
        std::string param_path = (models_dir / (task.model_name + ".param")).string();
        std::string bin_path = (models_dir / (task.model_name + ".bin")).string();

        auto load_result = engine.load_model(param_path, bin_path);
        if (!load_result.has_value()) {
            return std::unexpected(WorkerError{9, "Failed to load model: " + task.model_name +
                                  " - " + load_result.error().message});
        }
    }

    int scale_factor = engine.get_scale_factor();
    std::vector<ProcessedTile> processed_tiles;
    processed_tiles.reserve(task.tile_indices.size());

    // Receive batch of tiles in a single encrypted message
    auto batch_frame = recv_encrypted();
    if (!batch_frame.has_value()) {
        return std::unexpected(WorkerError{4, "Failed to receive batch: " + batch_frame.error().message});
    }
    auto& [batch_hdr, batch_payload] = *batch_frame;
    if (batch_hdr.type != MessageType::MSG_TILES_BATCH) {
        return std::unexpected(WorkerError{6, "Expected TILES_BATCH message, got type " +
                               std::to_string(static_cast<int>(batch_hdr.type))});
    }

    size_t offset = 0;
    size_t tile_index = 0;
    while (offset < batch_payload.size()) {
        if (offset + sizeof(ImageTileMessage) > batch_payload.size()) {
            return std::unexpected(WorkerError{5, "Invalid batch: incomplete ImageTileMessage"});
        }
        ImageTileMessage* tile_msg = reinterpret_cast<ImageTileMessage*>(batch_payload.data() + offset);
        offset += sizeof(ImageTileMessage);

        size_t data_size = tile_msg->data_length;
        if (offset + data_size > batch_payload.size()) {
            return std::unexpected(WorkerError{5, "Invalid batch: tile data overflow"});
        }
        std::vector<uint8_t> tile_data(data_size);
        memcpy(tile_data.data(), batch_payload.data() + offset, data_size);
        offset += data_size;

        if (!running_) {
            return std::unexpected(WorkerError{7, "Task cancelled"});
        }

        uint32_t tile_x = tile_msg->tile_x;
        uint32_t tile_y = tile_msg->tile_y;
        uint32_t tile_width = tile_msg->width;
        uint32_t tile_height = tile_msg->height;
        uint8_t channels = tile_msg->channels;

        LOG_INFO("Worker", "Processing tile " + std::to_string(tile_index + 1) + "/" + 
                 std::to_string(task.tile_indices.size()) + " [" + std::to_string(tile_width) + 
                 "x" + std::to_string(tile_height) + "]");

        // Convert to float
        std::vector<float> input_float(tile_data.size());
        for (size_t j = 0; j < tile_data.size(); ++j) {
            input_float[j] = static_cast<float>(tile_data[j]) / 255.0f;
        }

        int out_width = tile_width * scale_factor;
        int out_height = tile_height * scale_factor;
        std::vector<float> output_float(out_width * out_height * channels);

        if (!engine.process(input_float.data(), output_float.data(),
                           tile_width, tile_height, channels)) {
            return std::unexpected(WorkerError{10, "Engine processing failed for tile " + std::to_string(tile_index)});
        }

        // Convert back to uint8_t
        std::vector<uint8_t> output_data(output_float.size());
        for (size_t j = 0; j < output_float.size(); ++j) {
            float val = output_float[j] * 255.0f;
            output_data[j] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
        }

        ProcessedTile pt;
        pt.tile_x = tile_x;
        pt.tile_y = tile_y;
        pt.original_width = tile_width;
        pt.original_height = tile_height;
        pt.output_width = out_width;
        pt.output_height = out_height;
        pt.channels = channels;
        pt.data = std::move(output_data);
        processed_tiles.push_back(std::move(pt));

        float progress = static_cast<float>(tile_index + 1) / task.tile_indices.size();
        if (progress_callback) {
            progress_callback(progress);
        }
        send_progress_update(task.task_id, progress);

        ++tile_index;
    }

    if (tile_index != task.tile_indices.size()) {
        LOG_WARNING("Worker", "Expected " + std::to_string(task.tile_indices.size()) +
                    " tiles but received " + std::to_string(tile_index) + " tiles");
    }

    return serialize_processed_tiles(processed_tiles);
}


void Worker::send_progress_update(const std::string& task_id, float progress) {
    if (!socket_ || !socket_->is_connected() || !session_.active()) {
        return;
    }

    struct ProgressMessage {
        char task_id[36];
        float progress;
    };

    ProgressMessage msg;
    std::strncpy(msg.task_id, task_id.c_str(), sizeof(msg.task_id) - 1);
    msg.progress = progress;

    (void)send_encrypted(MessageType::MSG_TASK_PROGRESS, &msg, sizeof(msg));
}

auto Worker::serialize_processed_tiles(const std::vector<ProcessedTile>& tiles)
    -> std::vector<uint8_t> {

    size_t total_size = sizeof(uint32_t);
    for (const auto& tile : tiles) {
        total_size += sizeof(uint32_t) * 6;
        total_size += sizeof(uint8_t);
        total_size += sizeof(uint32_t);
        total_size += tile.data.size();
    }

    std::vector<uint8_t> result;
    result.reserve(total_size);

    uint32_t tile_count = static_cast<uint32_t>(tiles.size());
    result.insert(result.end(),
                  reinterpret_cast<uint8_t*>(&tile_count),
                  reinterpret_cast<uint8_t*>(&tile_count) + sizeof(tile_count));

    for (const auto& tile : tiles) {
        uint32_t x = tile.tile_x;
        uint32_t y = tile.tile_y;
        uint32_t orig_w = tile.original_width;
        uint32_t orig_h = tile.original_height;
        uint32_t out_w = tile.output_width;
        uint32_t out_h = tile.output_height;
        uint8_t ch = tile.channels;
        uint32_t data_size = static_cast<uint32_t>(tile.data.size());

        result.insert(result.end(), reinterpret_cast<uint8_t*>(&x),
                      reinterpret_cast<uint8_t*>(&x) + sizeof(x));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&y),
                      reinterpret_cast<uint8_t*>(&y) + sizeof(y));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&orig_w),
                      reinterpret_cast<uint8_t*>(&orig_w) + sizeof(orig_w));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&orig_h),
                      reinterpret_cast<uint8_t*>(&orig_h) + sizeof(orig_h));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&out_w),
                      reinterpret_cast<uint8_t*>(&out_w) + sizeof(out_w));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&out_h),
                      reinterpret_cast<uint8_t*>(&out_h) + sizeof(out_h));
        result.insert(result.end(), &ch, &ch + 1);
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&data_size),
                      reinterpret_cast<uint8_t*>(&data_size) + sizeof(data_size));
        result.insert(result.end(), tile.data.begin(), tile.data.end());
    }

    return result;
}

auto Worker::submit_results(const std::vector<uint8_t>& results)
    -> expected<void, WorkerError> {

    if (!is_connected()) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }

    return send_encrypted(MessageType::MSG_TASK_RESULT, results.data(), results.size());
}

void Worker::start_heartbeat(std::chrono::seconds interval) {
    stop_heartbeat();
    heartbeat_running_ = true;
    heartbeat_thread_ = std::thread(&Worker::heartbeat_loop, this, interval);
}

void Worker::stop_heartbeat() {
    heartbeat_running_ = false;
    if (heartbeat_thread_.joinable()) {
        if (heartbeat_thread_.get_id() == std::this_thread::get_id()) {
            heartbeat_thread_.detach();
        } else {
            heartbeat_thread_.join();
        }
    }
}

void Worker::heartbeat_loop(std::chrono::seconds interval) {
    while (heartbeat_running_ && running_) {
        if (socket_ && socket_->is_connected() && session_.active()) {
            auto res = send_encrypted(MessageType::MSG_HEARTBEAT, nullptr, 0);
            if (!res.has_value()) {
                LOG_WARNING("Worker", "Heartbeat send failed: " + res.error().message);
                break;
            }
        }

        std::this_thread::sleep_for(interval);
    }
    heartbeat_running_ = false;
}

void Worker::set_models_directory(const std::filesystem::path& dir) {
    model_manager_.set_models_directory(dir);
}

auto Worker::sync_models() -> expected<void, WorkerError> {
    if (!is_connected()) {
        return std::unexpected(WorkerError{2, "Not connected"});
    }

    LOG_INFO("Worker", "sync_models: waiting for model list from coordinator...");

    auto frame = recv_encrypted();
    if (!frame.has_value()) {
        return std::unexpected(WorkerError{11, "Failed to receive model list: " + frame.error().message});
    }
    auto& [header, payload] = *frame;

    if (header.type != MessageType::MSG_MODEL_LIST) {
        return std::unexpected(WorkerError{13, "Expected MODEL_LIST message"});
    }
    if (payload.size() < sizeof(ModelListMessage)) {
        return std::unexpected(WorkerError{14, "Model list payload too small"});
    }

    ModelListMessage* msg = reinterpret_cast<ModelListMessage*>(payload.data());
    uint32_t model_count = msg->model_count;

    LOG_INFO("Worker", "Received model list with " + std::to_string(model_count) +
             " models (will fetch on demand during tasks)");

    // We deliberately do NOT download models here. At connect time no
    // coordinator thread is reading our socket, so a MSG_MODEL_REQUEST would
    // deadlock. Models are fetched lazily in receive_task() once a task is
    // assigned — at that point the coordinator's dispatch loop serves requests.
    (void)model_manager_.scan_models();
    LOG_INFO("Worker", "Model sync complete");

    return {};
}

auto Worker::download_model(const std::string& model_name, uint8_t file_type)
    -> expected<void, WorkerError> {

    // Validate model name to prevent path traversal
    if (model_name.empty() || model_name.size() > 256) {
        return std::unexpected(WorkerError{21, "Invalid model name length"});
    }
    if (model_name.find("..") != std::string::npos ||
        model_name.find('/') != std::string::npos ||
        model_name.find('\\') != std::string::npos ||
        model_name.find('\0') != std::string::npos) {
        return std::unexpected(WorkerError{21, "Model name contains invalid characters: " + model_name});
    }

    uint64_t offset = 0;
    std::vector<uint8_t> file_data;

    std::filesystem::path models_dir = model_manager_.models_directory();
    std::string suffix = (file_type == 0 ? ".param" : ".bin");
    std::filesystem::path temp_path = models_dir / (model_name + suffix + ".tmp");
    std::filesystem::path final_path = models_dir / (model_name + suffix);

    // Verify final path is within models directory
    auto canonical_temp = std::filesystem::weakly_canonical(temp_path);
    auto canonical_dir = std::filesystem::weakly_canonical(models_dir);
    if (canonical_temp.string().find(canonical_dir.string()) != 0) {
        return std::unexpected(WorkerError{21, "Model path escapes models directory: " + model_name});
    }

    // Maximum model file size (500 MB)
    constexpr uint64_t MAX_MODEL_FILE_SIZE = 500ULL * 1024 * 1024;

    while (true) {
        ModelDataMessage request;
        std::strncpy(request.model_name, model_name.c_str(), sizeof(request.model_name) - 1);
        request.model_name[sizeof(request.model_name) - 1] = '\0';
        request.file_type = file_type;
        request.file_offset = offset;
        request.data_length = 0;

        auto send_result = send_encrypted(MessageType::MSG_MODEL_REQUEST, &request, sizeof(request));
        if (!send_result.has_value()) {
            return std::unexpected(WorkerError{15, "Failed to send model request"});
        }

        auto frame = recv_encrypted();
        if (!frame.has_value()) {
            return std::unexpected(WorkerError{16, "Failed to receive model data: " + frame.error().message});
        }
        auto& [resp_header, resp_payload] = *frame;

        if (resp_header.type != MessageType::MSG_MODEL_DATA) {
            return std::unexpected(WorkerError{18, "Expected MODEL_DATA message"});
        }
        if (resp_payload.size() < sizeof(ModelDataMessage)) {
            return std::unexpected(WorkerError{17, "Invalid model data payload"});
        }

        const ModelDataMessage* data_msg = reinterpret_cast<const ModelDataMessage*>(resp_payload.data());
        uint32_t data_length = data_msg->data_length;

        if (data_length == 0) {
            break;
        }

        // Validate data_length doesn't exceed payload
        size_t data_offset = sizeof(ModelDataMessage);
        if (data_offset + data_length > resp_payload.size()) {
            return std::unexpected(WorkerError{23, "Model data length exceeds payload size"});
        }

        // Check total file size limit
        if (file_data.size() + data_length > MAX_MODEL_FILE_SIZE) {
            return std::unexpected(WorkerError{24, "Model file too large (>" + std::to_string(MAX_MODEL_FILE_SIZE / (1024*1024)) + " MB)"});
        }

        file_data.insert(file_data.end(),
                         resp_payload.data() + data_offset,
                         resp_payload.data() + data_offset + data_length);

        offset += data_length;

        if (data_length < 65536) {
            break;
        }
    }

    std::ofstream file(temp_path, std::ios::binary);
    if (!file) {
        return std::unexpected(WorkerError{20, "Cannot create temp file"});
    }

    file.write(reinterpret_cast<const char*>(file_data.data()), file_data.size());
    file.close();

    std::filesystem::rename(temp_path, final_path);

    LOG_INFO("Worker", "Downloaded " + model_name + suffix + 
             " (" + std::to_string(file_data.size()) + " bytes)");

    return {};
}

} // namespace crossscale
