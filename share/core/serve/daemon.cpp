#include "core/serve/daemon.hpp"

#include "core/engine.hpp"
#include "core/config.hpp"
#include "core/engine_setup.hpp"
#include "core/model_manager.hpp"
#include "core/tile_processor.hpp"
#include "core/tile_types.hpp"
#include "core/distributed/protocol.hpp"
#include "utils/image.hpp"
#include "utils/exif.hpp"
#include "utils/logger.hpp"
#include "utils/unix_socket.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#ifdef __linux__
#include <malloc.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "utils/sr_marker.hpp"

namespace upix {

namespace {

// Path length cap for the path-driven request (client sends absolute paths).
constexpr uint32_t MAX_SERVE_PATH_BYTES = 4096u;

// Payload can never legitimately exceed the fixed struct plus both capped
// paths. Without this bound, a corrupt or hostile client declares a ~4 GB
// payload_length and the vector below either OOMs the daemon or throws an
// uncaught bad_alloc inside the connection thread (→ terminate).
constexpr uint32_t MAX_SERVE_PAYLOAD_BYTES =
    static_cast<uint32_t>(sizeof(ServeRequest)) + 2u * MAX_SERVE_PATH_BYTES;

// Fixed-width wire fields are not guaranteed NUL-terminated; std::string
// construction from a bare char* would read past the struct. Copy with an
// explicit bound instead.
auto bounded_string(const char* src, size_t capacity) -> std::string {
    return {src, ::strnlen(src, capacity)};  // POSIX: not in std:: on glibc
}

// Write a framed message (header + payload) over a UnixSocket. serialize()
// computes the whole-frame checksum itself; anything set on h.checksum here
// would be overwritten.
auto send_via(UnixSocket& sock, MessageType type, const void* payload, size_t len)
    -> bool {
    MessageHeader h{};
    h.magic = Protocol::MAGIC;
    h.type = type;
    h.payload_length = static_cast<uint32_t>(len);
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

// Accessor subclass: StreamSocket keeps its fd protected with no public getter,
// but teardown needs the fd number to ::shutdown() a connection whose thread is
// blocked in recv(). (Calling close() from another thread would deadlock on the
// rx lock, which receive_all holds across the blocking read.) Legal C++: a
// derived class may read protected base members through its own objects.
class ConnSock : public UnixSocket {
public:
    explicit ConnSock(UnixSocket&& s) : UnixSocket(std::move(s)) {}
    [[nodiscard]] auto fd() const -> int { return socket_; }
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
    return base + "/upix/serve.sock";
}

ServeDaemon::ServeDaemon() = default;
ServeDaemon::~ServeDaemon() {
    stop();
    if (server_) server_->stop();
    // Join connection threads before engine_/processor_ (members below) are
    // destroyed, so no thread can touch them after teardown.
    drain_connections();
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
    // Create the parent directory first (e.g. ~/.cache/upix/); UnixServer
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
        LOG_ERROR("Daemon", "socket listen failed at {}: {}", socket_path_,
                  listen_res.error().message);
        return 7;
    }

    LOG_INFO("Daemon", "daemon ready: socket={} model={} scale={} device={} pid={}",
             socket_path_, loaded_model_name_, engine_->get_scale_factor(),
             device == ServeDevice::GPU ? "GPU" : "CPU", ::getpid());
    LOG_INFO("Daemon", "`upix process` will now reuse this warm engine");
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
    try {
        handle_client_inner(sock);
    } catch (const std::exception& e) {
        LOG_ERROR("Daemon", "request failed with exception: {}", e.what());
        try {
            send_serve_error(*sock, "internal error (see daemon log)");
        } catch (...) {}  // the client may be gone; nothing left to do
    } catch (...) {
        try {
            send_serve_error(*sock, "internal error (see daemon log)");
        } catch (...) {}
    }
}

auto ServeDaemon::handle_client_inner(std::unique_ptr<UnixSocket>& sock)
    -> void {
    // ── 1. Read request header ──────────────────────────────────────────────
    std::vector<uint8_t> hdr_buf(Protocol::HEADER_SIZE);
    if (!sock->receive_all(hdr_buf.data(), hdr_buf.size())) {
        LOG_DEBUG("Daemon", "conn: EOF/err before header");
        return;
    }
    auto parsed = Protocol::parse_header(hdr_buf);
    if (!parsed || parsed->type != MessageType::MSG_SERVE_REQUEST ||
        parsed->payload_length < sizeof(ServeRequest) ||
        parsed->payload_length > MAX_SERVE_PAYLOAD_BYTES) {
        // Malformed: send a minimal error response and bail.
        LOG_DEBUG("Daemon", "conn: header rejected (type/plen)");
        send_serve_error(*sock, "expected MSG_SERVE_REQUEST");
        return;
    }
    LOG_DEBUG("Daemon", "conn: header ok plen={}", parsed->payload_length);

    // ── 2. Read payload = [ServeRequest][input path][output path] ───────────
    std::vector<uint8_t> req_buf(parsed->payload_length);
    if (!sock->receive_all(req_buf.data(), req_buf.size())) {
        LOG_DEBUG("Daemon", "conn: EOF/err before payload");
        return;
    }
    ServeRequest req{};
    std::memcpy(&req, req_buf.data(), sizeof(req));

    // Plaintext frames enforce the header checksum (encrypted frames carry 0
    // and rely on Poly1305 instead — they never appear on this socket).
    // Reject before touching the payload: a mismatch means corruption or a
    // non-upix writer on the socket path.
    if (!Protocol::verify_frame(hdr_buf.data(), req_buf.data(), req_buf.size())) {
        LOG_DEBUG("Daemon", "conn: frame checksum mismatch");
        send_serve_error(*sock, "frame checksum mismatch");
        return;
    }

    const size_t paths_bytes = static_cast<size_t>(req.input_path_length) +
                               req.output_path_length;
    if (paths_bytes != req_buf.size() - sizeof(ServeRequest) ||
        req.input_path_length == 0 ||
        req.input_path_length > MAX_SERVE_PATH_BYTES ||
        req.output_path_length > MAX_SERVE_PATH_BYTES) {
        send_serve_error(*sock, "malformed request paths");
        return;
    }
    const char* base = reinterpret_cast<const char*>(req_buf.data() + sizeof(ServeRequest));
    const std::string input_path(base, req.input_path_length);
    const std::string output_path(base + req.input_path_length, req.output_path_length);
    req_buf.clear();
    req_buf.shrink_to_fit();

    // A client-named output format must be validated BEFORE the (possibly
    // minutes-long) inference: an unsupported value would otherwise burn a
    // full upscale and then die in the encoder. Empty = derive from paths.
    {
        const std::string req_fmt = bounded_string(req.output_format,
                                                   sizeof(req.output_format));
        if (!req_fmt.empty() &&
            !is_supported_output_format(normalize_output_format(req_fmt))) {
            send_serve_error(*sock,
                             ("unsupported output format: " + req_fmt).c_str());
            return;
        }
    }

    // FIFOs (and sockets) as input would block a connection thread inside
    // file I/O with no deadline — request_stop/::shutdown in teardown cannot
    // unblock it, so a single request could hang `daemon stop` forever.
    // Character devices (/dev/null, /dev/zero) read 0 bytes and fail cleanly,
    // so they stay allowed.
    {
        struct stat st{};
        if (::stat(input_path.c_str(), &st) == 0 &&
            (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
            send_serve_error(*sock,
                             "load failed: refusing fifo/socket input");
            return;
        }
    }

    // ── 3. Full hot-path pipeline: read → decode → upscale → encode → write.
    // The client never touches image bytes; its own footprint stays near zero.
    // Bytes are read once and shared by the EXIF orientation sniff and the
    // decode (same split as Image::load): portrait JPEGs are baked upright
    // right after decode, matching the inline and remote paths.
    auto raw = Image::load_raw(input_path);
    if (!raw) {
        send_serve_error(*sock, ("load failed: " + raw.error().message).c_str());
        return;
    }
    const int exif_orientation =
        exif::parse_jpeg_exif_orientation(raw->data(), raw->size());
    auto img_res = Image::load_from_memory(
        std::filesystem::path(input_path).extension().string(), *raw);
    raw->clear();
    raw->shrink_to_fit();
    if (!img_res) {
        send_serve_error(*sock, ("load failed: " + img_res.error().message).c_str());
        return;
    }
    Image& image = img_res.value();
    if (image.size() == 0) {
        send_serve_error(*sock, "decoded image is empty");
        return;
    }
    if (exif_orientation != 1 &&
        exif::autorotate_image(image, exif_orientation)) {
        LOG_INFO("Daemon", "applied EXIF orientation {}", exif_orientation);
    }

    int out_w = 0, out_h = 0;
    int used_scale = 0;
    Image out_image;
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        if (ensure_model_loaded(bounded_string(req.model_name,
                                               sizeof(req.model_name))) != 0) {
            send_serve_error(*sock, "model load failed");
            return;
        }

        // Scale comes from the model that was just ensured under this lock.
        const int scale = engine_->get_scale_factor();
        used_scale = scale;
        out_w = image.width() * scale;
        out_h = image.height() * scale;
        out_image = Image(out_w, out_h, image.channels());

        TileConfig tc;
        // Wire tile/overlap are clamped to the same envelope ProcessingConfig
        //::validate enforces on CLI configs (tile ≤ 2048, overlap ≤ tile/2).
        // A hostile overlap ≥ tile would degenerate the grid to step=1 — tens
        // of thousands of full-size tiles on one request (hours of engine
        // time); a huge tile turns a large input into a single giant forward.
        if (req.tile_size > 0) {
            tc.tile_size = std::clamp(req.tile_size, 1, 2048);
        } else {
            // tile_size 0 = "auto": the client didn't pin a size, so apply the
            // same engine recommendation the inline path uses (cmd_process's
            // `!tile_size_explicit && !auto_tile` branch) — keeps daemon-delegated
            // and inline output byte-identical without the client needing its own
            // engine to ask.
            tc.tile_size = engine_->get_recommended_tile_size();
        }
        tc.overlap = static_cast<uint32_t>(std::clamp(
            static_cast<int32_t>(req.tile_overlap), 0, tc.tile_size / 2));
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
            msg.stage = 0;
            (void)send_via(*sock, MessageType::MSG_SERVE_PROGRESS, &msg, sizeof(msg));
        };
        auto res = processor_->process_image_u8(
            image.data(), out_image.data(),
            image.width(), image.height(), image.channels(),
            *engine_, tc, progress_sink, cancel, /*enable_alpha_bicubic=*/true);
        if (!res) {
            send_serve_error(*sock, ("inference failed: " + res.error().message).c_str());
            return;
        }
    }

    // Tell the client inference is over and the encode (which can take a
    // couple of seconds for avif) is starting, so it can label the wait.
    ServeProgress enc_msg{};
    enc_msg.completed_tiles = 0;
    enc_msg.total_tiles = 0;
    enc_msg.stage = 1;
    (void)send_via(*sock, MessageType::MSG_SERVE_PROGRESS, &enc_msg, sizeof(enc_msg));

    // Model actually used (ensure_model_loaded may have reloaded); read under
    // the lock — it feeds both the derived output name and the response.
    std::string used_model;
    {
        std::lock_guard<std::mutex> lk(engine_mutex_);
        used_model = loaded_model_name_;
    }

    // ── 4. Encode + write. The output format comes from the request (which
    // resolved --format / config file client-side); with none given, keep the
    // input extension when it maps to a known encoder (same rule as the CLI's
    // derive_default_output).
    ProcessingConfig out_cfg;
    out_cfg.output_format = bounded_string(req.output_format,
                                           sizeof(req.output_format));
    const std::filesystem::path eff_out = [&]() -> std::filesystem::path {
        if (!output_path.empty()) return output_path;
        std::filesystem::path in(input_path);
        std::string ext = in.extension().string();
        if (!is_supported_output_format(normalize_output_format(
                ext.empty() ? "" : ext.substr(1)))) {
            ext = ".png";
        }
        return in.parent_path() /
               (in.stem().string() + "_" +
                sanitize_model_for_filename(used_model) + ext);
    }();
    auto final_path = resolve_output_path(eff_out, out_cfg);
    if (!final_path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(final_path.parent_path(), ec);
    }
    // Same SR marker the inline CLI path writes, so daemon-served outputs
    // are recognized on re-input too.
    const auto sr_xmp = sr_marker::build_xmp(
        used_model, used_scale, static_cast<int64_t>(std::time(nullptr)));
    auto save_res = out_image.save(final_path,
                                   static_cast<int>(req.output_quality), sr_xmp,
                                   from_wire(req.encoder));
    if (!save_res) {
        send_serve_error(*sock, ("save failed: " + save_res.error().message).c_str());
        return;
    }
    LOG_INFO("Daemon", "served {}x{} -> {}x{} => {}",
             image.width(), image.height(), out_w, out_h, final_path.string());

    // ── 5. Response: [ServeResponse][output path]. No image bytes follow —
    // the daemon already wrote the file.
    ServeResponse resp{};
    resp.ok = 1;
    resp.width = out_w;
    resp.height = out_h;
    resp.channels = image.channels();
    const std::string out_str = final_path.string();
    resp.out_path_length = static_cast<uint32_t>(out_str.size());
    // Report the model actually used — see used_model above.
    std::strncpy(resp.model_name, used_model.c_str(), sizeof(resp.model_name) - 1);
    std::vector<uint8_t> head(sizeof(resp) + out_str.size());
    std::memcpy(head.data(), &resp, sizeof(resp));
    std::memcpy(head.data() + sizeof(resp), out_str.data(), out_str.size());
    send_via(*sock, MessageType::MSG_SERVE_RESPONSE, head.data(), head.size());
}

auto ServeDaemon::reap_finished_connections() -> void {
    // conn_threads_ is touched only by run()/the destructor; this runs on the
    // accept thread. join() on a done thread returns immediately.
    for (auto it = conn_threads_.begin(); it != conn_threads_.end();) {
        if (!it->get()->done.load()) {
            ++it;
            continue;
        }
        if (it->get()->thread.joinable()) it->get()->thread.join();
        if (it->get()->watch_fd >= 0) {
            ::close(it->get()->watch_fd);
            it->get()->watch_fd = -1;
        }
        it = conn_threads_.erase(it);
    }
}

auto ServeDaemon::drain_connections() -> void {
    // Interrupt readers before joining: a client that connected but never sent
    // bytes would block recv() forever and hang the joins below. ::shutdown on
    // the dup'd watch fd wakes the blocked recv() with an error; the watch fd
    // itself is closed immediately (the connection's real fd belongs to the
    // thread's UnixSocket).
    for (auto& ct : conn_threads_) ct->thread.request_stop();
    for (auto& ct : conn_threads_) {
        if (ct->watch_fd >= 0) {
            ::shutdown(ct->watch_fd, SHUT_RDWR);
            ::close(ct->watch_fd);
            ct->watch_fd = -1;
        }
    }
    for (auto& ct : conn_threads_) {
        if (ct->thread.joinable()) ct->thread.join();
    }
    conn_threads_.clear();
}

auto ServeDaemon::run() -> void {
    const std::stop_token st = stop_src_.get_token();
    last_active_ms_.store(steady_now_ms());
    while (!st.stop_requested()) {
        reap_finished_connections();
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
                        LOG_INFO("Daemon", "idle for {}s, exiting", idle_ms / 1000);
                        break;
                    }
                }
                continue;
            }
            if (!st.stop_requested()) {
                LOG_WARNING("Daemon", "accept error: {}", client.error().message);
            }
            continue;
        }
        active_conns_.fetch_add(1);
        last_active_ms_.store(steady_now_ms());
        // Each connection runs on its own tracked jthread so a slow GPU
        // inference on one client doesn't block accept() for others; drain at
        // loop exit guarantees none outlives run()/the daemon. Engine access is
        // still serialized inside handle_client via engine_mutex_. The watch fd
        // lets drain_connections() unstick a reader blocked on this client.
        auto sock = std::make_unique<ConnSock>(std::move(**client));
        const int watch_fd = ::dup(sock->fd());
        // Heap-held entry: the connection thread needs a stable pointer to
        // its own record (a growing vector<ConnThread> would invalidate one).
        auto entry = std::make_unique<ConnThread>();
        entry->watch_fd = watch_fd;
        entry->thread = std::jthread(
            [this, e = entry.get(), c = std::move(sock)]() mutable {
                handle_client(std::move(c));
                // glibc keeps freed heap for reuse; hand it back so RSS
                // tracks live allocations, not the largest request served.
#ifdef __linux__
                malloc_trim(0);
#endif
                e->done.store(true);  // reaper may now join us
            });
        conn_threads_.push_back(std::move(entry));
    }
    // Same teardown path for stop(), fatal error, and idle exit.
    drain_connections();
}

auto ServeDaemon::stop() -> void {
    stop_src_.request_stop();
}

auto ServeDaemon::set_idle_timeout(int seconds) -> void {
    idle_timeout_s_ = seconds > 0 ? seconds : 0;
}

} // namespace upix
