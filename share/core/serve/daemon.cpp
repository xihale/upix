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

#include <chrono>
#include <cstring>
#include <filesystem>
#include <malloc.h>
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

// Steady-clock ms — shared clock for the idle-timeout bookkeeping.
auto steady_now_ms() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// RAII: one live connection. Decrements active_conns_ and refreshes
// last_active_ms_ when handle_client returns, so a finished request never
// counts its own runtime toward the idle timeout.
struct ConnGuard {
    std::atomic<int>& conns;
    std::atomic<int64_t>& last_active;
    ~ConnGuard() {
        conns.fetch_sub(1);
        last_active.store(steady_now_ms());
    }
};

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
        LOG_ERROR("Daemon", setup.error());
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
        LOG_ERROR("Daemon", "socket listen failed at " + socket_path_ +
                           ": " + listen_res.error().message);
        return 7;
    }

    LOG_INFO("Daemon", "daemon ready: socket=" + socket_path_ +
                      " model=" + loaded_model_name_ +
                      " scale=" + std::to_string(engine_->get_scale_factor()) +
                      " device=" + (device == ServeDevice::GPU ? "GPU" : "CPU") +
                      " pid=" + std::to_string(::getpid()));
    LOG_INFO("Daemon", "`crossscale process` will now reuse this warm engine");
    return 0;
}

auto ServeDaemon::ensure_model_loaded(const std::string& model_name) -> int {
    // Caller already holds engine_mutex_.
    return switch_model(*engine_, *models_, loaded_model_name_, model_name, "Daemon") ? 0 : 1;
}

auto ServeDaemon::handle_client(std::unique_ptr<UnixSocket> sock) -> void {
    // sock owns the fd; closed exactly once on return. The guard keeps the
    // idle-timeout accounting truthful for this connection's whole lifetime.
    ConnGuard conn{active_conns_, last_active_ms_};

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
    std::string used_model;  // captured under the lock; read below without it
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        if (ensure_model_loaded(req.model_name) != 0) {
            send_serve_error(*sock, "model load failed");
            return;
        }
        used_model = loaded_model_name_;
        int scale = engine_->get_scale_factor();
        out_w = req.width * scale;
        out_h = req.height * scale;
        out_bytes.resize(static_cast<size_t>(out_w) * out_h * out_c);
        TileConfig tc;
        // tile_size 0 = "auto": the client didn't pin a size, so apply the
        // same engine recommendation the inline path uses (cmd_process's
        // `!tile_size_explicit && !auto_tile` branch) — keeps daemon-delegated
        // and inline output byte-identical without the client needing its own
        // engine to ask.
        tc.tile_size = req.tile_size > 0
            ? req.tile_size
            : engine_->get_recommended_tile_size();
        tc.overlap = req.tile_overlap;
        tc.batch_size = 1;

        std::atomic<bool> cancel{false};
        // Stream tile progress to the client so it can render the same live
        // progress line the inline path shows. Throttled to whole-percent
        // changes; a failed progress send is non-fatal (the terminal response
        // reports real errors).
        int last_sent_pct = -1;
        auto progress_sink = [&](const ProcessingProgress& prog) {
            const int pct = static_cast<int>(prog.percentage());
            if (pct == last_sent_pct) return;
            last_sent_pct = pct;
            ServeProgress msg{};
            msg.completed_tiles = static_cast<uint32_t>(prog.completed_tiles);
            msg.total_tiles = static_cast<uint32_t>(prog.total_tiles);
            (void)send_via(*sock, MessageType::MSG_SERVE_PROGRESS, &msg, sizeof(msg));
        };
        auto res = processor_->process_image_u8(
            img_in, out_bytes.data(),
            req.width, req.height, req.channels,
            *engine_, tc, progress_sink, cancel, /*enable_alpha_bicubic=*/true);
        if (!res) {
            err_msg = res.error().message;
            out_bytes.clear();
        }
    }

    // ── 4. Send response: [ServeResponse][image] streamed in bounded frames.
    // MSG_SERVE_RESPONSE carries the struct + the leading slice; MSG_SERVE_DATA
    // frames continue the raw bytes. Slicing straight out of out_bytes keeps
    // peak RAM at one image copy instead of three (payload + frame + image),
    // and no frame ever has to hold the whole result.
    ServeResponse resp{};
    if (!err_msg.empty()) {
        resp.ok = 0;
        std::strncpy(resp.error, err_msg.c_str(), sizeof(resp.error) - 1);
        send_via(*sock, MessageType::MSG_SERVE_RESPONSE, &resp, sizeof(resp));
        return;
    }
    resp.ok = 1;
    resp.width = out_w;
    resp.height = out_h;
    resp.channels = out_c;
    resp.img_length = static_cast<uint32_t>(out_bytes.size());
    // Report the model actually used — ensure_model_loaded may have reloaded
    // (request named none, or a different one).
    std::strncpy(resp.model_name, used_model.c_str(), sizeof(resp.model_name) - 1);

    constexpr size_t SERVE_CHUNK_BYTES = 8u * 1024u * 1024u;
    size_t offset = 0;
    bool first_frame = true;
    while (first_frame || offset < out_bytes.size()) {
        const size_t n = std::min(SERVE_CHUNK_BYTES, out_bytes.size() - offset);
        if (first_frame) {
            std::vector<uint8_t> head(sizeof(resp) + n);
            std::memcpy(head.data(), &resp, sizeof(resp));
            std::memcpy(head.data() + sizeof(resp), out_bytes.data(), n);
            send_via(*sock, MessageType::MSG_SERVE_RESPONSE, head.data(), head.size());
            first_frame = false;
        } else {
            send_via(*sock, MessageType::MSG_SERVE_DATA,
                     out_bytes.data() + offset, n);
        }
        offset += n;
    }
}

auto ServeDaemon::run() -> void {
    running_.store(true);
    last_active_ms_.store(steady_now_ms());
    while (running_.load() && !stop_requested_.load()) {
        auto client = server_->accept();
        if (!client) {
            if (client.error().code == EAGAIN) {
                // Poll-timeout tick: exit when idle. A connection in flight
                // (active_conns_ > 0) always suppresses this, and ConnGuard
                // refreshes last_active_ms_ on completion, so a long request
                // never counts its own runtime as idle time.
                if (idle_timeout_s_ > 0 && active_conns_.load() == 0) {
                    auto idle_ms = steady_now_ms() - last_active_ms_.load();
                    if (idle_ms / 1000 >= idle_timeout_s_) {
                        LOG_INFO("Daemon", "idle for " +
                                  std::to_string(idle_ms / 1000) + "s, exiting");
                        break;
                    }
                }
                continue;
            }
            if (!stop_requested_.load()) {
                LOG_WARNING("Daemon", "accept error: " + client.error().message);
            }
            continue;
        }
        active_conns_.fetch_add(1);
        last_active_ms_.store(steady_now_ms());
        // Detach: each connection runs on its own thread so a slow GPU inference
        // on one client doesn't block accept() for others. Engine access is
        // still serialized inside handle_client via engine_mutex_.
        std::thread([this, c = std::move(*client)]() mutable {
            handle_client(std::move(c));
            // A served request leaves hundreds of MB of freed heap (result
            // buffer, staging) that glibc keeps for reuse. Hand it back to
            // the OS so the daemon's RSS tracks what it is actually holding,
            // not the largest request it ever saw.
            malloc_trim(0);
        }).detach();
    }
}

auto ServeDaemon::stop() -> void {
    stop_requested_.store(true);
    running_.store(false);
}

auto ServeDaemon::set_idle_timeout(int seconds) -> void {
    idle_timeout_s_ = seconds > 0 ? seconds : 0;
}

} // namespace crossscale
