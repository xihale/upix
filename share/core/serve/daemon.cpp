#include "core/serve/daemon.hpp"

#include "core/engine.hpp"
#include "core/engine_setup.hpp"
#include "core/model_manager.hpp"
#include "core/tile_processor.hpp"
#include "core/tile_types.hpp"
#include "core/distributed/protocol.hpp"
#include "utils/image.hpp"
#include "utils/logger.hpp"
#include "utils/unix_socket.hpp"

#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

namespace crossscale {

namespace {

// Max image bytes accepted over the socket. Guards against a malicious or
// buggy client streaming an unbounded payload. 256 MB is generous for any
// plausible single-image SR input.
constexpr uint32_t MAX_SERVE_IMAGE_BYTES = 256u * 1024u * 1024u;

// Write a framed message (header + payload) over a UnixSocket.
auto send_via(UnixSocket& sock, MessageType type, const void* payload, size_t len)
    -> bool {
    MessageHeader h{};
    h.magic = Protocol::MAGIC;
    h.version = Protocol::VERSION;
    h.type = type;
    h.payload_length = static_cast<uint32_t>(len);
    h.checksum = Protocol::compute_checksum(payload, len);
    auto buf = Protocol::serialize(h, payload);
    return sock.send_all(buf.data(), buf.size()).has_value();
}

// Send a simple error response for serve requests
auto send_serve_error(UnixSocket& sock, const char* message) -> void {
    ServeResponse r{};
    r.ok = 0;
    std::strncpy(r.error, message, sizeof(r.error) - 1);
    send_via(sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
}

} // namespace

auto default_serve_socket_path() -> std::string {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    std::string base;
    if (cache && *cache) {
        base = cache;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        base = std::string(home) + "/.cache";
    }
    return base + "/crossscale/serve.sock";
}

ServeDaemon::ServeDaemon() = default;
ServeDaemon::~ServeDaemon() {
    stop();
    if (server_) server_->stop();
}

auto ServeDaemon::start(const std::string& socket_path,
                        const std::string& models_dir,
                        const std::string& model_name,
                        ServeDevice device) -> int {
    device_ = device;
    socket_path_ = socket_path;

    // ── Engine init + model load (create→init→scan→select→load recipe) ──────
    // Collapses the previous 2/3/4/5/6 distinct failure codes to a single 2;
    // the only caller (cmd_serve) checks `!= 0` and does not branch on the code.
    auto setup = setup_engine(
        device == ServeDevice::GPU ? SetupDevice::GPU : SetupDevice::CPU,
        models_dir,
        model_name
    );
    if (!setup) {
        LOG_ERROR("Serve", setup.error());
        return 2;
    }
    loaded_model_name_ = setup->selected->name;  // read pointer before moving models
    engine_ = std::move(setup->engine);
    models_ = std::make_unique<ModelManager>(std::move(setup->models));
    processor_ = std::make_unique<TileProcessor>();

    // ── Bind the socket ─────────────────────────────────────────────────────
    // Create the parent directory first (e.g. ~/.cache/crossscale/); UnixServer
    // only creates/unlinks the socket file itself.
    {
        std::error_code ec;
        std::filesystem::path p(socket_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }
    server_ = std::make_unique<UnixServer>();
    auto listen_res = server_->listen(socket_path_);
    if (!listen_res) {
        LOG_ERROR("Serve", "socket listen failed at " + socket_path_ +
                           ": " + listen_res.error().message);
        return 7;
    }

    LOG_INFO("Serve", "daemon ready: socket=" + socket_path_ +
                      " model=" + loaded_model_name_ +
                      " scale=" + std::to_string(engine_->get_scale_factor()) +
                      " device=" + (device == ServeDevice::GPU ? "GPU" : "CPU") +
                      " pid=" + std::to_string(::getpid()));
    LOG_INFO("Serve", "`crossscale process` will now reuse this warm engine");
    return 0;
}

auto ServeDaemon::ensure_model_loaded(const std::string& model_name) -> int {
    // Caller already holds engine_mutex_.
    // Empty model name = "use whatever is currently loaded" (client didn't
    // specify -m). This is the common case and avoids a needless reload.
    if (model_name.empty() || model_name == loaded_model_name_) return 0;
    auto info = models_->get_model(model_name);
    if (!info) {
        LOG_ERROR("Serve", "unknown model requested: " + model_name);
        return 1;
    }
    auto res = engine_->load_model(info->param_path.string(),
                                   info->bin_path.string());
    if (!res) {
        LOG_ERROR("Serve", "model reload failed: " + res.error().message);
        return 2;
    }
    loaded_model_name_ = model_name;
    LOG_INFO("Serve", "switched model to " + model_name);
    return 0;
}

auto ServeDaemon::handle_client(std::unique_ptr<UnixSocket> sock) -> void {
    // sock owns the fd; closed exactly once on return.

    // ── 1. Read request header ──────────────────────────────────────────────
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    if (!sock->receive_all(hdr_buf.data(), hdr_buf.size())) return;
    auto parsed = Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
    if (!parsed || parsed->type != MessageType::MSG_SERVE_REQUEST ||
        parsed->payload_length < sizeof(ServeRequest)) {
        // Malformed: send a minimal error response and bail.
        send_serve_error(*sock, "expected MSG_SERVE_REQUEST");
        return;
    }

    // ── 2. Read payload = [ServeRequest][image] contiguous ──────────────────
    std::vector<uint8_t> req_buf(parsed->payload_length);
    if (!sock->receive_all(req_buf.data(), req_buf.size())) return;
    ServeRequest req{};
    std::memcpy(&req, req_buf.data(), sizeof(ServeRequest));

    // Validate image size
    const size_t expected_size = sizeof(ServeRequest) + req.img_length;
    if (req.img_length > MAX_SERVE_IMAGE_BYTES || req_buf.size() < expected_size) {
        send_serve_error(*sock, "image too large");
        return;
    }
    const uint8_t* img_in = req_buf.data() + sizeof(ServeRequest);

    // ── 3. Inference under engine lock (Vulkan serial constraint) ───────────
    std::vector<uint8_t> out_bytes;
    int out_w = 0, out_h = 0, out_c = req.channels;
    std::string err_msg;
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        if (ensure_model_loaded(req.model_name) != 0) {
            send_serve_error(*sock, "model load failed");
            return;
        }
        int scale = engine_->get_scale_factor();
        out_w = req.width * scale;
        out_h = req.height * scale;
        out_bytes.resize(static_cast<size_t>(out_w) * out_h * out_c);
        TileConfig tc;
        tc.tile_size = req.tile_size;
        tc.overlap = req.tile_overlap;
        tc.batch_size = 1;

        std::atomic<bool> cancel{false};
        auto res = processor_->process_image_u8(
            img_in, out_bytes.data(),
            req.width, req.height, req.channels,
            *engine_, tc, {}, cancel, /*enable_alpha_bicubic=*/true);
        if (!res) {
            err_msg = res.error().message;
            out_bytes.clear();
        }
    }

    // ── 4. Send response: payload = [ServeResponse][image] contiguous ───────
    ServeResponse resp{};
    if (err_msg.empty()) {
        resp.ok = 1;
        resp.width = out_w;
        resp.height = out_h;
        resp.channels = out_c;
        resp.img_length = static_cast<uint32_t>(out_bytes.size());
    } else {
        resp.ok = 0;
        std::strncpy(resp.error, err_msg.c_str(), sizeof(resp.error) - 1);
    }

    // Assemble contiguous payload so Protocol::serialize produces one frame.
    size_t payload_len = sizeof(resp) + (resp.ok ? out_bytes.size() : 0);
    std::vector<uint8_t> payload(payload_len);
    std::memcpy(payload.data(), &resp, sizeof(resp));
    if (resp.ok && !out_bytes.empty()) {
        std::memcpy(payload.data() + sizeof(resp), out_bytes.data(), out_bytes.size());
    }
    send_via(*sock, MessageType::MSG_SERVE_RESPONSE, payload.data(), payload.size());
}

auto ServeDaemon::run() -> void {
    running_.store(true);
    while (running_.load() && !stop_requested_.load()) {
        auto client = server_->accept();
        if (!client) {
            if (client.error().code == EAGAIN) continue;  // poll timeout
            if (!stop_requested_.load()) {
                LOG_WARNING("Serve", "accept error: " + client.error().message);
            }
            continue;
        }
        // Detach: each connection runs on its own thread so a slow GPU inference
        // on one client doesn't block accept() for others. Engine access is
        // still serialized inside handle_client via engine_mutex_.
        std::thread([this, c = std::move(*client)]() mutable {
            handle_client(std::move(c));
        }).detach();
    }
}

auto ServeDaemon::stop() -> void {
    stop_requested_.store(true);
    running_.store(false);
}

} // namespace crossscale
