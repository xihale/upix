#include "core/serve/daemon.hpp"

#include "core/engine.hpp"
#include "core/model_manager.hpp"
#include "core/tile_processor.hpp"
#include "core/tile_types.hpp"
#include "core/distributed/protocol.hpp"
#include "utils/image.hpp"
#include "utils/logger.hpp"
#include "utils/unix_socket.hpp"

#include <chrono>
#include <csignal>
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
    start_time_unix_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // ── Engine init (the ~330 ms cost amortized over all future requests) ──
    engine_ = create_ncnn_engine();
    auto init_res = engine_->initialize(device == ServeDevice::GPU
                                        ? DeviceType::GPU_VULKAN
                                        : DeviceType::CPU);
    if (!init_res) {
        LOG_ERROR("Serve", "engine initialize failed: " + init_res.error().message);
        return 2;
    }

    // ── Resolve + load the initial model (the ~160 ms cost amortized) ───────
    models_ = std::make_unique<ModelManager>();
    models_->set_models_directory(models_dir.empty() ? "./models" : models_dir);
    auto scan = models_->scan_models();
    if (!scan) {
        LOG_ERROR("Serve", "model scan failed: " + scan.error().message);
        return 3;
    }
    if (scan->empty()) {
        LOG_ERROR("Serve", "no models found in: " + models_dir);
        return 4;
    }
    const ModelInfo* chosen = nullptr;
    if (!model_name.empty()) {
        for (const auto& m : *scan) {
            if (m.name == model_name) { chosen = &m; break; }
        }
        if (!chosen) {
            LOG_ERROR("Serve", "model not found: " + model_name);
            return 5;
        }
    } else {
        chosen = &scan->front();
    }
    auto load_res = engine_->load_model(chosen->param_path.string(),
                                        chosen->bin_path.string());
    if (!load_res) {
        LOG_ERROR("Serve", "model load failed: " + load_res.error().message);
        return 6;
    }
    loaded_model_name_ = chosen->name;
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
    // sock owns the fd; closed exactly once on return. Use sock->send_all/
    // receive_all for all I/O.

    // ── 1. Send HELLO immediately ───────────────────────────────────────────
    ServeHello hello = this->hello();
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        std::memset(hello.model_name, 0, sizeof(hello.model_name));
        loaded_model_name_.copy(hello.model_name, sizeof(hello.model_name) - 1);
    }
    if (!send_via(*sock, MessageType::MSG_SERVE_HELLO, &hello, sizeof(hello))) return;

    // ── 2. Read the request header ──────────────────────────────────────────
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    if (!sock->receive_all(hdr_buf.data(), hdr_buf.size())) return;
    auto parsed = Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
    if (!parsed) return;

    // Graceful stop request from a client (e.g. `crossscale serve stop`).
    if (parsed->type == MessageType::MSG_SERVE_STOP) {
        stop();
        return;
    }

    if (parsed->type != MessageType::MSG_SERVE_REQUEST || parsed->payload_length < sizeof(ServeRequest)) {
        ServeResponse r{}; r.ok = 0;
        std::strncpy(r.error, "expected MSG_SERVE_REQUEST", sizeof(r.error) - 1);
        send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
        return;
    }

    // ── 3. Read ServeRequest struct + image bytes ───────────────────────────
    std::vector<uint8_t> req_buf(parsed->payload_length);
    if (!sock->receive_all(req_buf.data(), req_buf.size())) return;
    ServeRequest req{};
    std::memcpy(&req, req_buf.data(), sizeof(ServeRequest));

    if (req.img_length > MAX_SERVE_IMAGE_BYTES ||
        sizeof(ServeRequest) + req.img_length > req_buf.size()) {
        ServeResponse r{}; r.ok = 0;
        std::strncpy(r.error, "image too large", sizeof(r.error) - 1);
        send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
        return;
    }
    const uint8_t* img_in = req_buf.data() + sizeof(ServeRequest);

    // ── 4. Run inference under the engine lock (Vulkan serial constraint) ───
    std::vector<uint8_t> out_bytes;
    int out_w = 0, out_h = 0, out_c = req.channels;
    std::string err_msg;
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        if (ensure_model_loaded(req.model_name) != 0) {
            ServeResponse r{}; r.ok = 0;
            std::strncpy(r.error, "model load failed", sizeof(r.error) - 1);
            send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &r, sizeof(r));
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

    // ── 5. Send response header (framed) + raw image bytes ──────────────────
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
    if (!send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &resp, sizeof(resp))) return;
    if (resp.ok && resp.img_length > 0) {
        (void)sock->send_all(out_bytes.data(), out_bytes.size());
    }
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

auto ServeDaemon::hello() const -> ServeHello {
    ServeHello h{};
    h.pid = static_cast<int32_t>(::getpid());
    h.scale = engine_ ? engine_->get_scale_factor() : 0;
    h.device = (device_ == ServeDevice::GPU) ? 1 : 0;
    h.start_time_unix = start_time_unix_;
    return h;
}

} // namespace crossscale
