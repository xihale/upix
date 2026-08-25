// Shared CLI helpers — see common.hpp for the interface.
#include "common.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <vector>

#include "qrcodegen.hpp"

#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "core/config.hpp"
#include "core/distributed/identity.hpp"
#include "core/tile_processor.hpp"  // ProcessingProgress
#include "core/serve/daemon.hpp"    // ServeDaemon (forward-declared in header)

// ── Global run-state ────────────────────────────────────────────────────────
std::atomic<bool> g_running{true};
std::atomic<bool> g_cancel_requested{false};
crossscale::ServeDaemon* g_daemon = nullptr;

// ── Signal handlers ─────────────────────────────────────────────────────────
void signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
}

void daemon_signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
    if (g_daemon) g_daemon->stop();
}

// ── Defaults ────────────────────────────────────────────────────────────────
// Models directory resolution: `--models-dir` flag (bound by cli_app.cpp) >
// $CROSSSCALE_MODELS_DIR > the per-user store $XDG_DATA_HOME/crossscale/models
// (~/.local/share/crossscale/models by default). A fixed user-level location,
// so the CLI finds its models from any cwd and never scatters a ./models
// directory into whatever folder it was launched from.
auto default_models_dir() -> std::string {
    if (const char* env = std::getenv("CROSSSCALE_MODELS_DIR");
        env && *env) {
        return env;
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::format("{}/crossscale/models", xdg);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::format("{}/.local/share/crossscale/models", home);
    }
    return "./models";  // no HOME/XDG at all (degenerate): keep something usable
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

void print_progress_stderr(const crossscale::ProcessingProgress& prog,
                           std::chrono::steady_clock::time_point start_time) {
    if (prog.total_tiles <= 0) return;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time).count();
    float pct = prog.percentage();

    std::string line = std::format("{:.0f}% ({}/{}", pct,
                                   prog.completed_tiles, prog.total_tiles);

    if (prog.completed_tiles > 0 && elapsed > 0.1 && prog.completed_tiles < prog.total_tiles) {
        double tiles_per_sec = prog.completed_tiles / elapsed;
        double remaining_tiles = prog.total_tiles - prog.completed_tiles;
        double eta_sec = remaining_tiles / tiles_per_sec;
        line += std::format(", ETA {:.0f}s", eta_sec);
    }

    line += ')';
    std::print(stderr, "\r{}", line);
    std::fflush(stderr);

    if (prog.completed_tiles >= prog.total_tiles) {
        std::println(stderr, "");
    }
}

// Internal helper: convert an image's channel count (1/2/3/4) to a target count.
static auto convert_image_channels(const crossscale::Image& source, int target_channels)
    -> std::optional<crossscale::Image> {
    const int src_channels = source.channels();
    if (src_channels < 1 || src_channels > 4 || target_channels < 1 || target_channels > 4) {
        return std::nullopt;
    }

    crossscale::Image converted(source.width(), source.height(), target_channels);
    const size_t pixel_count = static_cast<size_t>(source.width()) * source.height();
    const uint8_t* src = source.data();
    uint8_t* dst = converted.data();

    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t* src_px = src + i * static_cast<size_t>(src_channels);
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;

        switch (src_channels) {
            case 1:
                r = g = b = src_px[0];
                break;
            case 2:
                r = g = b = src_px[0];
                a = src_px[1];
                break;
            case 3:
                r = src_px[0];
                g = src_px[1];
                b = src_px[2];
                break;
            case 4:
                r = src_px[0];
                g = src_px[1];
                b = src_px[2];
                a = src_px[3];
                break;
            default:
                return std::nullopt;
        }

        const uint8_t gray = static_cast<uint8_t>(
            (77u * static_cast<unsigned int>(r) +
             150u * static_cast<unsigned int>(g) +
             29u * static_cast<unsigned int>(b) +
             128u) >> 8
        );

        uint8_t* dst_px = dst + i * static_cast<size_t>(target_channels);
        switch (target_channels) {
            case 1:
                dst_px[0] = gray;
                break;
            case 2:
                dst_px[0] = gray;
                dst_px[1] = a;
                break;
            case 3:
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                break;
            case 4:
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                dst_px[3] = a;
                break;
            default:
                return std::nullopt;
        }
    }

    return converted;
}

// Default output location when the user gave none (`crossscale img.png`):
// sibling of the input, tagged with the model that produced it so results from
// different models coexist — `img.png` → `img_<model>.png` (input extension
// kept when it maps to a known output format, else .png), and a directory
// input `shots/` → `shots_<model>/` next to it.
auto derive_default_output(const std::string& input_path,
                           const std::string& model_name,
                           bool input_is_dir) -> std::string {
    namespace fs = std::filesystem;
    fs::path in(input_path);
    const std::string tag = "_" + crossscale::sanitize_model_for_filename(model_name);

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

auto normalize_channels_for_model(crossscale::Image& image,
                                  int expected_channels,
                                  const std::string& image_path) -> bool {
    if (expected_channels <= 0 || image.channels() == expected_channels) {
        return true;
    }

    auto converted = convert_image_channels(image, expected_channels);
    if (!converted.has_value()) {
        LOG_ERROR("Engine",
                  "Input image channel count {} is incompatible with model input channels {}: {}",
                  image.channels(), expected_channels, image_path);
        return false;
    }

    LOG_WARNING("Engine",
                "Input image channel count {} does not match model input channels {}, converting channels: {}",
                image.channels(), expected_channels, image_path);

    image = std::move(converted.value());
    if (image.channels() != expected_channels) {
        LOG_ERROR("Engine", "Failed to convert input channels from {} to {}: {}",
                  image.channels(), expected_channels, image_path);
        return false;
    }

    if (image.channels() < 1 || image.channels() > 4) {
        LOG_ERROR("Engine", "Converted image has unsupported channel count {}: {}",
                  image.channels(), image_path);
        return false;
    }

    return true;
}

// ── QR / connection card ────────────────────────────────────────────────────
auto generate_qr_ascii(const std::string& text) -> std::string {
    using namespace qrcodegen;

    const QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);

    std::string result;
    int border = 2;

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

void print_connection_card(uint16_t port, const std::array<uint8_t, 32>& identity_pubkey) {
    auto addresses = crossscale::network_utils::get_local_addresses();
    if (addresses.empty()) {
        addresses.push_back("127.0.0.1");
    }

    const std::string fp = crossscale::fingerprint(identity_pubkey);
    const std::string pk_b64 = crossscale::base64_encode(identity_pubkey.data(), 32);

    std::println("\nConnection Information:");
    for (size_t i = 0; i < addresses.size(); ++i) {
        std::print("  IP: {}", addresses[i]);
        if (i == 0) std::print("  (default for QR)");
        std::println("");
    }
    std::println("  Port: {}", port);
    std::println("  Fingerprint: {}\n", fp);
    std::println("QR Code (scan with Android app):\n");

    // v2 JSON: carries fingerprint + identity pubkey. Android rejects v1.
    const std::string json = std::format(
        "{{\"v\":2,\"ip\":\"{}\",\"port\":{},\"fp\":\"{}\""
        ",\"pk\":\"{}\",\"name\":\"CrossScale-Server\"}}",
        addresses[0], port, fp, pk_b64);
    std::println("{}", generate_qr_ascii(json));
}
