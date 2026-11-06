// Shared CLI helpers — see common.hpp for the interface.
#include "common.hpp"
#include "utils/platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#include "qrcodegen.hpp"

#include "args.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "core/config.hpp"
#include "core/distributed/identity.hpp"
#include "core/tile_processor.hpp"  // ProcessingProgress
#include "core/serve/daemon.hpp"    // ServeDaemon (forward-declared in header)

// ── Global run-state ────────────────────────────────────────────────────────
std::atomic<bool> g_running{true};
std::atomic<bool> g_cancel_requested{false};
upix::ServeDaemon* g_daemon = nullptr;

// ── Signal handlers ─────────────────────────────────────────────────────────
void signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
}

auto resolve_processing_config(const args::Process& a) -> EffectiveProcessingConfig {
    EffectiveProcessingConfig effective;
    auto& config = effective.config;
    bool loaded = false;
    if (!a.config_file.empty()) {
        auto result = upix::ProcessingConfig::load(a.config_file);
        if (result) {
            config = std::move(*result);
            loaded = true;
            LOG_INFO("Config", "Loaded config from {}", a.config_file);
        } else {
            LOG_WARNING("Config", "Failed to load config: {}", result.error());
        }
    }
    if (a.tile_size_explicit) {
        config.tile_size = a.tile_size;
        // A fixed CLI size overrides file-enabled auto sizing; supplying both
        // CLI options retains the existing --auto-tile behavior.
        config.auto_tile_size = false;
    }
    if (a.tile_overlap_explicit) config.tile_overlap = a.tile_overlap;
    if (a.batch_size_explicit) config.batch_size = a.batch_size;
    if (a.auto_tile_explicit) config.auto_tile_size = a.auto_tile;
    if (a.min_tile_explicit) config.min_tile_size = a.min_tile;
    if (a.max_tile_explicit) config.max_tile_size = a.max_tile;
    if (a.output_quality >= 0) config.output_quality = a.output_quality;
    if (!a.output_format.empty()) {
        config.output_format = upix::normalize_output_format(a.output_format);
    }
    if (a.png_level_explicit) config.encoder.png_level = a.png_level;
    if (a.jpeg_subsampling_explicit) config.encoder.jpeg_subsampling = a.jpeg_subsampling;
    if (a.webp_method_explicit) config.encoder.webp_method = a.webp_method;
    if (a.webp_lossless_explicit) config.encoder.webp_lossless = a.webp_lossless;
    if (a.avif_speed_explicit) config.encoder.avif_speed = a.avif_speed;
    if (a.avif_subsampling_explicit) config.encoder.avif_subsampling = a.avif_subsampling;
    if (a.avif_threads_explicit) config.encoder.avif_threads = a.avif_threads;
    effective.recommend_tile_size = !loaded && !a.tile_size_explicit &&
                                    !config.auto_tile_size;
    return effective;
}

void daemon_signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
    if (g_daemon) g_daemon->stop();
}

// ── Defaults ────────────────────────────────────────────────────────────────
// Models directory resolution: `--models-dir` flag (bound by cli_app.cpp) >
// $UPIX_MODELS_DIR > the per-user store $XDG_DATA_HOME/upix/models
// (~/.local/share/upix/models by default). A fixed user-level location,
// so the CLI finds its models from any cwd and never scatters a ./models
// directory into whatever folder it was launched from.
auto default_models_dir() -> std::string {
    if (const char* env = std::getenv("UPIX_MODELS_DIR");
        env && *env) {
        return env;
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::format("{}/upix/models", xdg);
    }
    if (const char* home = upix::user_home(); home && *home) {
        return std::format("{}/.local/share/upix/models", home);
    }
    return "./models";  // no HOME/XDG at all (degenerate): keep something usable
}

auto pid_is_upix_daemon(pid_t pid) -> bool {
    if (pid <= 0 || ::kill(pid, 0) != 0) {
        return false;
    }
#if defined(__linux__)
    // A zombie (parent not reaping) and any process that recycled this pid
    // both pass kill(0). The exe symlink is the reliable check: it fails for
    // zombies and names the real binary otherwise.
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink(
        std::format("/proc/{}/exe", pid), ec);
    if (ec || exe.string().find("upix") == std::string::npos) {
        return false;
    }
    // And it must be the daemon subcommand, not an unrelated upix process.
    std::ifstream cmd(std::format("/proc/{}/cmdline", pid), std::ios::binary);
    const std::string argv((std::istreambuf_iterator<char>(cmd)),
                           std::istreambuf_iterator<char>());
    return argv.find("daemon") != std::string::npos;
#elif defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (::proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return false;  // zombies fail here too
    }
    return std::string_view(path).find("upix") != std::string_view::npos;
#else
    return true;  // no executable probe on this platform: keep kill(0) semantics
#endif
}

// ── Image / format helpers ──────────────────────────────────────────────────
auto infer_format_from_extension(const std::string& path) -> std::string {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "png") return "png";
    if (ext == "jpg" || ext == "jpeg") return "jpg";
    if (ext == "webp") return "webp";
    if (ext == "avif") return "avif";
    return "";
}

// ── Machine-readable output ─────────────────────────────────────────────────
namespace {
std::atomic<bool> g_machine_output{false};
}  // namespace

auto machine_output_enabled() -> bool {
    return g_machine_output.load(std::memory_order_relaxed);
}

void enable_machine_output() {
    if (g_machine_output.exchange(true, std::memory_order_relaxed)) return;
    // stdout now carries events; logs would corrupt the stream.
    upix::Logger::instance().set_output_stderr(true);
}

auto json_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", c);
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void emit_json_line(const std::string& json) {
    std::println("{}", json);
    // GUI consumers read incrementally; the pairing prompt blocks on stdin
    // right after an event, so buffering here would deadlock the handshake.
    std::fflush(stdout);
}

auto read_line_interruptible(std::string& line,
                             const std::function<bool()>& still_waiting) -> bool {
    line.clear();
    for (;;) {
        if (!still_waiting()) {
            return false;  // shutting down: the caller answers the request
        }
        pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;  // signal: re-check still_waiting()
            return false;
        }
        if (pr == 0) {
            continue;  // tick: re-check still_waiting()
        }
        if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            return false;
        }
        // Bytes are ready, so a single read() cannot block. A partial line from
        // a GUI writer (no '\n' yet) just loops — with a poll tick in between,
        // so a signal still gets through.
        char buf[256];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            // EOF: bytes already read count as the answer (std::getline
            // behaved the same); an empty buffer means stdin is unusable.
            return !line.empty();
        }
        line.append(buf, static_cast<size_t>(n));
        const size_t nl = line.find('\n');
        if (nl != std::string::npos) {
            line.resize(nl);
            return true;
        }
        if (line.size() > kMaxPromptLineBytes) {
            line.resize(kMaxPromptLineBytes);
            return true;
        }
    }
}

// Shared ETA estimate: 0 until a full second of throughput is observable and
// once the last tile is done (matches what the human line always showed).
auto progress_eta_seconds(const upix::ProcessingProgress& prog,
                          std::chrono::steady_clock::time_point start_time)
    -> long long {
    if (prog.completed_tiles <= 0 || prog.completed_tiles >= prog.total_tiles)
        return 0;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time)
            .count();
    if (elapsed <= 0.1) return 0;
    const double tiles_per_sec = prog.completed_tiles / elapsed;
    return std::llround((prog.total_tiles - prog.completed_tiles) / tiles_per_sec);
}

void report_progress(const upix::ProcessingProgress& prog,
                     std::chrono::steady_clock::time_point start_time) {
    if (prog.total_tiles <= 0) return;
    if (machine_output_enabled()) {
        emit_json_line(std::format(
            R"({{"t":"progress","pct":{:.0f},"done":{},"total":{},"eta":{}}})",
            prog.percentage(), prog.completed_tiles, prog.total_tiles,
            progress_eta_seconds(prog, start_time)));
        return;
    }
    print_progress_stderr(prog, start_time);
}

void print_progress_stderr(const upix::ProcessingProgress& prog,
                           std::chrono::steady_clock::time_point start_time) {
    if (prog.total_tiles <= 0) return;

    std::string line = std::format("{:.0f}% ({}/{}", prog.percentage(),
                                   prog.completed_tiles, prog.total_tiles);

    const long long eta_sec = progress_eta_seconds(prog, start_time);
    if (eta_sec > 0) {
        line += std::format(", ETA {}s", eta_sec);
    }

    line += ')';
    std::print(stderr, "\r{}", line);
    std::fflush(stderr);

    if (prog.completed_tiles >= prog.total_tiles) {
        std::println(stderr, "");
    }
}

// Internal helper: convert an image's channel count (1/2/3/4) to a target count.
// Default output location when the user gave none (`upix img.png`):
// sibling of the input, tagged with the model that produced it so results from
// different models coexist — `img.png` → `img_<model>.png` (input extension
// kept when it maps to a known output format, else .png), and a directory
// input `shots/` → `shots_<model>/` next to it.
auto derive_default_output(const std::string& input_path,
                           const std::string& model_name,
                           bool input_is_dir) -> std::string {
    namespace fs = std::filesystem;
    fs::path in(input_path);
    const std::string tag = "_" + upix::sanitize_model_for_filename(model_name);

    if (input_is_dir) {
        // filename() of "shots/" is "shots"; of "/" is "" (rejected upstream).
        return (in.parent_path() / (in.filename().string() + tag)).string() + "/";
    }

    std::string ext = in.extension().string();
    if (ext.empty() || infer_format_from_extension(in.filename().string()).empty()) {
        ext = ".png";
    }
    fs::path out = in.parent_path() / (in.stem().string() + tag + ext);
    return out.string();
}

// convert_image_channels / normalize_channels_for_model now live in
// share/utils/image (upix::) so the coordinator's remote task path applies the
// exact same conversion; the CLI wrapper keeps the historical signature.
auto normalize_channels_for_model(upix::Image& image,
                                  int expected_channels,
                                  const std::string& image_path) -> bool {
    return upix::normalize_image_channels(image, expected_channels, image_path);
}

// ── QR / connection card ────────────────────────────────────────────────────
auto generate_qr_ascii(const std::string& text) -> std::string {
    using namespace qrcodegen;

    const QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);

    // 半块字符渲染：两模块竖叠一字符，配合 1:2 终端字体格得到 ~1:1 方形模块。
    // quiet zone 取标准 4 模块——此前用 2 模块在暗色终端/滚动边框干扰下
    // 识别率明显下降。
    std::string result;
    constexpr int border = 4;

    for (int y = -border; y < qr.getSize() + border; y += 2) {
        for (int x = -border; x < qr.getSize() + border; ++x) {
            bool top = qr.getModule(x, y);
            bool bottom = (y + 1 < qr.getSize() + border) ? qr.getModule(x, y + 1) : false;

            if (top && bottom) result += "█";
            else if (top && !bottom) result += "▀";
            else if (!top && bottom) result += "▄";
            else result += " ";
        }
        result += "\n";
    }

    return result;
}

auto build_connection_card(uint16_t port,
                           const std::array<uint8_t, 32>& identity_pubkey)
    -> ConnectionCard {
    ConnectionCard card;
    card.addresses = upix::network_utils::get_local_addresses();
    if (card.addresses.empty()) {
        card.addresses.push_back("127.0.0.1");
    }
    card.port = port;
    card.fingerprint = upix::fingerprint(identity_pubkey);
    card.pubkey_b64 = upix::base64_encode(identity_pubkey.data(), 32);

    // v2 JSON: carries fingerprint + identity pubkey. Android rejects v1.
    // ("name" removed: Android's JoinInviteParser never reads it — 13 bytes
    // of payload cost QR modules for nothing.)
    card.qr_payload = std::format(
        "{{\"v\":2,\"ip\":\"{}\",\"port\":{},\"fp\":\"{}\""
        ",\"pk\":\"{}\"}}",
        card.addresses[0], port, card.fingerprint, card.pubkey_b64);
    return card;
}

void print_connection_card(uint16_t port, const std::array<uint8_t, 32>& identity_pubkey) {
    const ConnectionCard card = build_connection_card(port, identity_pubkey);

    std::println("\nConnection Information:");
    for (size_t i = 0; i < card.addresses.size(); ++i) {
        std::print("  IP: {}", card.addresses[i]);
        if (i == 0) std::print("  (default for QR)");
        std::println("");
    }
    std::println("  Port: {}", card.port);
    std::println("  Fingerprint: {}\n", card.fingerprint);
    std::println("QR Code (scan with Android app):\n");

    // 极性：QR 规定亮底暗码。半块字符靠前景色渲染码块、空格露出背景，
    // 深色终端下会呈现反色的黑底白码，扫码器（含 ZXing）无法识别。
    // 在彩色终端上强制 SGR 白底黑前景（47/30），与终端主题无关。
    // 无色环境（管道/NO_COLOR）保持纯文本，由查看环境决定极性。
    const std::string qr_text = generate_qr_ascii(card.qr_payload);
    if (upix::console_color_enabled()) {
        for (const auto& line : qr_text | std::views::split('\n')) {
            const std::string_view seg(line.begin(), line.end());
            if (seg.empty()) continue;
            std::println("\033[47m\033[30m{}\033[0m", seg);
        }
    } else {
        std::println("{}", qr_text);
    }
    std::fflush(stdout);
}
