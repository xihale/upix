#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <csignal>
#include <future>
#include <mutex>
#include <deque>
#include <condition_variable>

#include "qrcodegen.hpp"

#include "utils/logger.hpp"
#include "utils/image.hpp"
#include "utils/benchmark.hpp"
#include "utils/crypto.hpp"
#include "core/engine.hpp"
#include "core/tile_processor.hpp"
#include "core/model_manager.hpp"
#include "core/config.hpp"
#include "core/tile_optimizer.hpp"
#include "core/distributed/coordinator.hpp"
#include "core/distributed/worker.hpp"
#include "core/distributed/protocol.hpp"
#include "core/engine_pool.hpp"
#include "utils/mdns.hpp"
#include "utils/network.hpp"
#include "utils/unix_socket.hpp"
#include "core/serve/daemon.hpp"
#include "perf/runner.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <optional>
#include <cstdlib>

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_cancel_requested{false};

void signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
}

// Safe integer parsing with error handling
template<typename T>
T safe_parse_int(const std::string& str, const std::string& param_name, T min_val = std::numeric_limits<T>::min(), T max_val = std::numeric_limits<T>::max()) {
    try {
        long long val = std::stoll(str);
        if (val < static_cast<long long>(min_val) || val > static_cast<long long>(max_val)) {
            throw std::out_of_range("Value " + str + " out of range for " + param_name);
        }
        return static_cast<T>(val);
    } catch (const std::exception& e) {
        LOG_ERROR("CLI", "Invalid value for " + param_name + ": " + str + " (" + e.what() + ")");
        throw;
    }
}

float safe_parse_float(const std::string& str, const std::string& param_name) {
    try {
        return std::stof(str);
    } catch (const std::exception& e) {
        LOG_ERROR("CLI", "Invalid value for " + param_name + ": " + str + " (" + e.what() + ")");
        throw;
    }
}

// Maximum payload size for network messages (100 MB)
static constexpr uint32_t MAX_NETWORK_PAYLOAD = 100 * 1024 * 1024;

// Infer output format from file extension (for auto-detection)
static std::string infer_format_from_extension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "png") return "png";
    if (ext == "jpg" || ext == "jpeg") return "jpg";
    if (ext == "webp") return "webp";
    return "";
}

// Format progress line for stderr:  45.2% [12/27 tiles, ETA 3.2s]
static void print_progress_stderr(const crossscale::ProcessingProgress& prog,
                                  std::chrono::steady_clock::time_point start_time) {
    if (prog.total_tiles <= 0) return;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time).count();
    float pct = prog.percentage();

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << pct << "%  ["
        << prog.completed_tiles << "/" << prog.total_tiles << " tiles";

    if (prog.completed_tiles > 0 && elapsed > 0.1) {
        double tiles_per_sec = prog.completed_tiles / elapsed;
        double remaining_tiles = prog.total_tiles - prog.completed_tiles;
        double eta_sec = remaining_tiles / tiles_per_sec;
        oss << ", " << std::setprecision(1) << tiles_per_sec << " tiles/s"
            << ", ETA " << std::setprecision(1) << eta_sec << "s";
    }

    oss << "]";
    std::cerr << "\r" << oss.str() << std::flush;

    if (prog.completed_tiles >= prog.total_tiles) {
        std::cerr << "\n";
    }
}

auto convert_image_channels(const crossscale::Image& source, int target_channels)
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

auto normalize_channels_for_model(
    crossscale::Image& image,
    int expected_channels,
    const std::string& image_path
) -> bool {
    if (expected_channels <= 0 || image.channels() == expected_channels) {
        return true;
    }

    auto converted = convert_image_channels(image, expected_channels);
    if (!converted.has_value()) {
        LOG_ERROR(
            "Engine",
            "Input image channel count " + std::to_string(image.channels()) +
                " is incompatible with model input channels " + std::to_string(expected_channels) +
                ": " + image_path
        );
        return false;
    }

    LOG_WARNING(
        "Engine",
        "Input image channel count " + std::to_string(image.channels()) +
            " does not match model input channels " + std::to_string(expected_channels) +
            ", converting channels: " + image_path
    );

    image = std::move(converted.value());
    if (image.channels() != expected_channels) {
        LOG_ERROR(
            "Engine",
            "Failed to convert input channels from " + std::to_string(image.channels()) +
                " to " + std::to_string(expected_channels) +
                ": " + image_path
        );
        return false;
    }

    if (image.channels() < 1 || image.channels() > 4) {
        LOG_ERROR(
            "Engine",
            "Converted image has unsupported channel count " + std::to_string(image.channels()) +
            ": " + image_path
        );
        return false;
    }

    return true;
}

void print_usage() {
    std::cout << "CrossScale - Distributed Image Super-Resolution System\n\n"
              << "Usage:\n"
              << "  crossscale -i <input> -o <output> [-m <model>] [options]\n"
              << "  crossscale process -i <input> -o <output> -m <model> [options]\n"
              << "  crossscale server start -p <port> [-k <key>]\n"
              << "  crossscale server info -p <port> -k <key>\n"
              << "  crossscale worker connect <host>:<port> -k <key> [--recommended-weight <num>] [--queue-depth <num>] [--recommended-tile-size <num>]\n"
              << "  crossscale model list\n"
              << "  crossscale benchmark\n"
              << "  crossscale serve start [--model <name>] [--fg]\n"
              << "  crossscale --help\n\n"
              << "Options (process):\n"
              << "  -i <path>           Input image or directory\n"
              << "  -o <path>           Output image or directory\n"
              << "  -m <name>           Model name (default: first available)\n"
              << "  --tile-size <n>     Tile size (default: auto based on GPU VRAM)\n"
              << "  --tile-overlap <n>  Tile overlap (default: 16)\n"
              << "  --batch-size <n>    Batch size (default: 1)\n"
              << "  --jobs <n>          Parallel workers for directory mode\n"
              << "  --output-format <fmt>  Output format: png/jpg/jpeg/webp (default: auto from extension)\n"
              << "  --output-quality <n>   Output quality 0-100 (default: 95)\n";
}

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

// Prints the connection card shown to the user: all local IPv4 addresses
// (first non-loopback one is used as the QR default), the auth key, and a QR
// code carrying the JSON the Android app's JoinInviteParser.parseJsonInvite()
// understands. Shared by `server start` and `server info` to stay in sync.
void print_connection_card(uint16_t port, const std::string& key) {
    auto addresses = crossscale::network_utils::get_local_addresses();
    if (addresses.empty()) {
        addresses.push_back("127.0.0.1");
    }

    std::cout << "\nConnection Information:\n";
    for (size_t i = 0; i < addresses.size(); ++i) {
        std::cout << "  IP: " << addresses[i];
        if (i == 0) std::cout << "  (default for QR)";
        std::cout << "\n";
    }
    std::cout << "  Port: " << port << "\n"
              << "  Key: " << key << "\n\n"
              << "QR Code (scan with Android app):\n\n";

    const std::string json =
        "{\"v\":1,\"ip\":\"" + addresses[0] +
        "\",\"port\":" + std::to_string(port) +
        ",\"key\":\"" + key + "\""
        ",\"name\":\"CrossScale-Server\"}";
    std::cout << generate_qr_ascii(json) << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    g_running = true;
    g_cancel_requested = false;

    std::string command = argv[1];

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    // Implicit process command: if argv[1] is not a known subcommand and contains
    // '-i' or '-o' flags, treat the entire invocation as 'crossscale process ...'
    bool implicit_process = false;
    if (command != "process" && command != "server" && command != "worker" &&
        command != "model" && command != "benchmark" && command != "client" &&
        command != "pair" && command != "perf" && command != "serve") {
        // Check if any arg looks like a process flag
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-i" || a == "-o" || a == "-m") {
                implicit_process = true;
                break;
            }
        }
        if (!implicit_process) {
            LOG_ERROR("CLI", "Unknown command: " + command);
            print_usage();
            return 1;
        }
    }

    if (command == "process" || implicit_process) {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::string input_path, output_path, model_name;
        std::string models_dir = "./models";
        std::string config_file;
        int cli_output_quality = -1;
        std::string cli_output_format;

        // 瓦片配置参数
        int tile_size = 256;
        int tile_overlap = 16;
        bool auto_tile = false;
        bool tile_size_explicit = false;  // Track if user explicitly set --tile-size
        int min_tile = 64;
        int max_tile = 512;
        int batch_size = 1;
        int jobs = 1;
        bool use_gpu = false;  // for directory mode with --jobs > 1
        bool no_daemon = false;  // --no-daemon: skip serve-daemon delegation

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-i" && i + 1 < argc) input_path = argv[++i];
            else if (arg == "-o" && i + 1 < argc) output_path = argv[++i];
            else if (arg == "-m" && i + 1 < argc) model_name = argv[++i];
            else if (arg == "--models" && i + 1 < argc) models_dir = argv[++i];
            else if (arg == "--tile-size" && i + 1 < argc) { tile_size = safe_parse_int<int>(argv[++i], "--tile-size", 16, 8192); tile_size_explicit = true; }
            else if (arg == "--tile-overlap" && i + 1 < argc) tile_overlap = safe_parse_int<int>(argv[++i], "--tile-overlap", 0, 1024);
            else if (arg == "--batch-size" && i + 1 < argc) batch_size = safe_parse_int<int>(argv[++i], "--batch-size", 1, 64);
            else if (arg == "--auto-tile") auto_tile = true;
            else if (arg == "--min-tile" && i + 1 < argc) min_tile = safe_parse_int<int>(argv[++i], "--min-tile", 16, 8192);
            else if (arg == "--max-tile" && i + 1 < argc) max_tile = safe_parse_int<int>(argv[++i], "--max-tile", 16, 8192);
            else if (arg == "--jobs" && i + 1 < argc) jobs = safe_parse_int<int>(argv[++i], "--jobs", 1, 1024);
            else if (arg == "-j" && i + 1 < argc) jobs = safe_parse_int<int>(argv[++i], "-j", 1, 1024);
            else if (arg == "--gpu") use_gpu = true;
            else if (arg == "--no-daemon") no_daemon = true;
            else if (arg == "--output-quality" && i + 1 < argc) cli_output_quality = safe_parse_int<int>(argv[++i], "--output-quality", 0, 100);
            else if (arg == "--output-format" && i + 1 < argc) cli_output_format = argv[++i];
            else if (arg == "--config" && i + 1 < argc) config_file = argv[++i];
        }

        if (input_path.empty() || output_path.empty()) {
            LOG_ERROR("CLI", "Missing input or output path");
            return 1;
        }

        // Check if input is a directory for batch processing
        bool is_dir = fs::is_directory(input_path);
        LOG_INFO("Debug", "Input path: " + input_path + " is_dir: " + std::to_string(is_dir));

        // ── Serve-daemon delegation (single-file only) ──────────────────────
        // If a local serve daemon is running, send it the image and let the
        // warm engine do inference — skipping the ~490 ms init+load fixed
        // cost. Falls back silently (returns nullopt) if no daemon or the
        // connection fails; on a real daemon-side error returns 1.
        if (!is_dir && !no_daemon) {
            auto delegate = [&]() -> std::optional<int> {
                // 1. Probe: no socket file / connect fails → no daemon, fall back.
                std::string sock_path = crossscale::default_serve_socket_path();
                if (sock_path.empty()) return std::nullopt;
                std::error_code ec;
                if (!std::filesystem::exists(sock_path, ec)) return std::nullopt;

                crossscale::UnixSocket sock;
                auto conn = sock.connect(sock_path);
                if (!conn) return std::nullopt;  // stale socket → no daemon

                // 2. Load image; on failure let the inline path report the error.
                auto img_res = crossscale::Image::load(input_path);
                if (!img_res) return std::nullopt;
                auto& image = img_res.value();
                if (image.size() == 0) return std::nullopt;

                // 3. Build contiguous payload = [ServeRequest][image], one frame.
                crossscale::ServeRequest req{};
                std::memset(req.model_name, 0, sizeof(req.model_name));
                model_name.copy(req.model_name, sizeof(req.model_name) - 1);
                req.tile_size = tile_size;
                req.tile_overlap = tile_overlap;
                req.width = image.width();
                req.height = image.height();
                req.channels = image.channels();
                req.img_length = static_cast<uint32_t>(image.size());

                std::vector<uint8_t> payload(sizeof(req) + image.size());
                std::memcpy(payload.data(), &req, sizeof(req));
                std::memcpy(payload.data() + sizeof(req), image.data(), image.size());

                crossscale::MessageHeader h{};
                h.magic = crossscale::Protocol::MAGIC;
                h.version = crossscale::Protocol::VERSION;
                h.type = crossscale::MessageType::MSG_SERVE_REQUEST;
                h.payload_length = static_cast<uint32_t>(payload.size());
                h.checksum = 0;  // filled by serialize
                auto frame = crossscale::Protocol::serialize(h, payload.data());
                if (!sock.send_all(frame.data(), frame.size())) return std::nullopt;

                // 4. Read response header, then contiguous payload
                //    [ServeResponse][image].
                std::vector<uint8_t> rhdr(crossscale::Protocol::HEADER_SIZE);
                if (!sock.receive_all(rhdr.data(), rhdr.size())) {
                    LOG_ERROR("Engine", "daemon connection lost mid-request");
                    return 1;
                }
                auto rparsed = crossscale::Protocol::parse_header(rhdr.data(), rhdr.size());
                if (!rparsed || rparsed->type != crossscale::MessageType::MSG_SERVE_RESPONSE ||
                    rparsed->payload_length < sizeof(crossscale::ServeResponse)) {
                    LOG_ERROR("Engine", "malformed daemon response");
                    return 1;
                }
                if (rparsed->payload_length > MAX_NETWORK_PAYLOAD) {
                    LOG_ERROR("Engine", "daemon response too large");
                    return 1;
                }
                std::vector<uint8_t> rpayload(rparsed->payload_length);
                if (!sock.receive_all(rpayload.data(), rpayload.size())) {
                    LOG_ERROR("Engine", "truncated daemon response");
                    return 1;
                }

                crossscale::ServeResponse resp{};
                std::memcpy(&resp, rpayload.data(), sizeof(resp));
                if (!resp.ok) {
                    LOG_ERROR("Engine", "daemon error: " + std::string(resp.error));
                    return 1;
                }
                const uint8_t* out = rpayload.data() + sizeof(resp);
                size_t out_size = rparsed->payload_length - sizeof(resp);
                if (out_size != resp.img_length) {
                    LOG_ERROR("Engine", "daemon response length mismatch");
                    return 1;
                }

                // 5. Wrap into Image and save (same path as inline single-file).
                crossscale::Image out_image(resp.width, resp.height, resp.channels);
                std::memcpy(out_image.data(), out, out_size);

                crossscale::ProcessingConfig config;
                if (!config_file.empty()) {
                    auto cfg = crossscale::ProcessingConfig::load(config_file);
                    if (cfg) config = cfg.value();
                }
                if (cli_output_quality >= 0) config.output_quality = cli_output_quality;
                if (!cli_output_format.empty()) config.output_format = cli_output_format;

                auto final_path = crossscale::resolve_output_path(output_path, config);
                if (!final_path.parent_path().empty()) {
                    std::filesystem::create_directories(final_path.parent_path());
                }
                auto save_res = out_image.save(final_path, config.output_quality);
                if (!save_res) {
                    LOG_ERROR("Engine", "Failed to save: " + save_res.error().message);
                    return 1;
                }
                LOG_INFO("Engine", "Output saved (via warm daemon): " + final_path.string());
                LOG_INFO("Engine", "Processing complete: " + std::to_string(resp.width) +
                                   "x" + std::to_string(resp.height));
                return 0;
            };

            auto delegated = delegate();
            if (delegated.has_value()) return delegated.value();
            // else: no daemon / probe failed → fall through to inline path
        }
        if (is_dir) {
            // Validate output path
            if (output_path.empty()) {
                LOG_ERROR("CLI", "Output path required when input is a directory");
                return 1;
            }
            if (fs::exists(output_path) && !fs::is_directory(output_path)) {
                LOG_ERROR("CLI", "Output path must be a directory when input is a directory");
                return 1;
            }
            fs::create_directories(output_path);
            
            // Gather image files recursively
            std::vector<std::string> extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tiff", ".tif"};
            std::vector<std::pair<std::string, std::string>> tasks;
            for (auto& p : fs::recursive_directory_iterator(input_path)) {
                if (!p.is_regular_file()) continue;
                std::string ext = p.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                bool is_img = false;
                for (const auto& e : extensions) {
                    if (ext == e) { is_img = true; break; }
                }
                if (!is_img) continue;
                std::string rel = fs::relative(p.path(), input_path).string();
                std::string out_file = fs::path(output_path) / rel;
                tasks.emplace_back(p.path().string(), out_file);
            }
            if (tasks.empty()) {
                LOG_WARNING("CLI", "No image files found in input directory");
                return 0;
            }
            LOG_INFO("CLI", "Found " + std::to_string(tasks.size()) + " images to process");
            
            // Model selection
            crossscale::ModelManager manager(fs::current_path() / "models");
            auto models_opt = manager.scan_models();
            if (!models_opt.has_value() || models_opt->empty()) {
                LOG_ERROR("Engine", "No models found in " + models_dir);
                return 1;
            }
            auto& models = models_opt.value();
            
            const crossscale::ModelInfo* selected_model = nullptr;
            if (model_name.empty()) {
                selected_model = &models[0];
                LOG_INFO("Engine", "No model specified, using first available: " + selected_model->name);
            } else {
                for (const auto& m : models) {
                    if (m.name == model_name) {
                        selected_model = &m;
                        break;
                    }
                }
                if (!selected_model) {
                    LOG_ERROR("Engine", "Model not found: " + model_name);
                    LOG_ERROR("Engine", "Available models:");
                    for (const auto& m : models) {
                        LOG_ERROR("Engine", "  - " + m.name + " (" + std::to_string(m.scale_factor) + "x)");
                    }
                    return 1;
                }
            }
            
            std::string param_path = selected_model->param_path.string();
            std::string bin_path = selected_model->bin_path.string();
            std::string model_name_used = selected_model->name;
            size_t model_total_size = selected_model->total_size;
            int model_input_channels = std::max(1, selected_model->input_channels);
            LOG_INFO("Engine", "Using model: " + model_name_used + " (" + std::to_string(selected_model->scale_factor) + "x)");
            
            // Load configuration if specified
            crossscale::ProcessingConfig config;
            if (!config_file.empty()) {
                auto cfg_result = crossscale::ProcessingConfig::load(config_file);
                if (cfg_result.has_value()) {
                    config = cfg_result.value();
                    LOG_INFO("Config", "Loaded config from " + config_file);
                } else {
                    LOG_WARNING("Config", "Failed to load config: " + cfg_result.error());
                }
            }
            
            // Override with CLI arguments
            config.tile_size = tile_size;
            config.tile_overlap = tile_overlap;
            config.batch_size = batch_size;
            config.auto_tile_size = auto_tile;
            config.min_tile_size = min_tile;
            config.max_tile_size = max_tile;
            if (cli_output_quality >= 0) {
                config.output_quality = cli_output_quality;
            }
            if (!cli_output_format.empty()) {
                config.output_format = crossscale::normalize_output_format(cli_output_format);
            } else {
                // Auto-infer output format from first output file extension
                if (!tasks.empty()) {
                    std::string inferred = infer_format_from_extension(tasks[0].second);
                    if (!inferred.empty()) {
                        config.output_format = crossscale::normalize_output_format(inferred);
                        LOG_INFO("Config", "Auto-detected output format: " + inferred);
                    }
                }
            }
            
            // Validate configuration
            auto errors = config.validate();
            if (!errors.empty()) {
                LOG_ERROR("Config", "Configuration errors:");
                for (const auto& err : errors) {
                    LOG_ERROR("Config", "  - " + err);
                }
                return 1;
            }
            


            struct PreparedTask {
                std::string in_path;
                std::string out_path;
                int width = 0;
                int height = 0;
                int channels = 0;
                crossscale::Image input_image;
            };

            auto prepare_task = [&](const std::string& in_path, const std::string& out_path) -> std::optional<PreparedTask> {
                if (g_cancel_requested.load()) {
                    return std::nullopt;
                }
                auto img_res = crossscale::Image::load(in_path);
                if (!img_res.has_value()) {
                    LOG_ERROR("Task", "Failed to load image: " + in_path);
                    return std::nullopt;
                }

                PreparedTask prepared;
                prepared.in_path = in_path;
                prepared.out_path = out_path;
                prepared.input_image = std::move(img_res.value());

                if (!normalize_channels_for_model(prepared.input_image, model_input_channels, in_path)) {
                    return std::nullopt;
                }

                prepared.width = prepared.input_image.width();
                prepared.height = prepared.input_image.height();
                prepared.channels = prepared.input_image.channels();

                return prepared;
            };

            bool enable_progress_log = false;
            if (const char* progress_env = std::getenv("CROSSSCALE_PROGRESS")) {
                enable_progress_log = std::string(progress_env) == "1";
            }
            // Track processing start time for ETA
            auto batch_start_time = std::chrono::steady_clock::now();
            size_t gpu_submit_queue_max = 3;
            if (const char* queue_env = std::getenv("CROSSSCALE_GPU_SUBMIT_QUEUE")) {
                try {
                    gpu_submit_queue_max = static_cast<size_t>(std::stoul(queue_env));
                } catch (...) {
                    gpu_submit_queue_max = 3;
                }
            }
            gpu_submit_queue_max = std::clamp(gpu_submit_queue_max, static_cast<size_t>(1), static_cast<size_t>(8));

            size_t gpu_submit_queue_min = 1;
            if (const char* queue_min_env = std::getenv("CROSSSCALE_GPU_SUBMIT_QUEUE_MIN")) {
                try {
                    gpu_submit_queue_min = static_cast<size_t>(std::stoul(queue_min_env));
                } catch (...) {
                    gpu_submit_queue_min = 1;
                }
            }
            gpu_submit_queue_min = std::clamp(gpu_submit_queue_min, static_cast<size_t>(1), gpu_submit_queue_max);

            bool gpu_dynamic_submit_queue = true;
            if (const char* queue_dynamic_env = std::getenv("CROSSSCALE_GPU_SUBMIT_QUEUE_DYNAMIC")) {
                gpu_dynamic_submit_queue = std::string(queue_dynamic_env) != "0";
            }

            size_t gpu_submit_queue_target_tiles = 64;
            if (const char* queue_target_env = std::getenv("CROSSSCALE_GPU_SUBMIT_QUEUE_TARGET_TILES")) {
                try {
                    gpu_submit_queue_target_tiles = static_cast<size_t>(std::stoul(queue_target_env));
                } catch (...) {
                    gpu_submit_queue_target_tiles = 64;
                }
            }
            gpu_submit_queue_target_tiles = std::clamp(
                gpu_submit_queue_target_tiles,
                static_cast<size_t>(16),
                static_cast<size_t>(256)
            );

            size_t gpu_min_submit_batch = gpu_submit_queue_min;
            if (const char* min_batch_env = std::getenv("CROSSSCALE_GPU_MIN_SUBMIT_BATCH")) {
                try {
                    gpu_min_submit_batch = static_cast<size_t>(std::stoul(min_batch_env));
                } catch (...) {
                    gpu_min_submit_batch = gpu_submit_queue_min;
                }
            }
            gpu_min_submit_batch = std::clamp(gpu_min_submit_batch, static_cast<size_t>(1), gpu_submit_queue_max);

            int gpu_batch_wait_ms = 0;
            if (const char* batch_wait_env = std::getenv("CROSSSCALE_GPU_BATCH_WAIT_MS")) {
                try {
                    gpu_batch_wait_ms = std::stoi(batch_wait_env);
                } catch (...) {
                    gpu_batch_wait_ms = 0;
                }
            }
            gpu_batch_wait_ms = std::clamp(gpu_batch_wait_ms, 0, 10);

            struct ProcessedTask {
                std::string out_path;
                int out_width = 0;
                int out_height = 0;
                int channels = 0;
                crossscale::Image output_image;
            };

            auto infer_prepared_task = [&](PreparedTask&& prepared, crossscale::Engine& eng) -> std::optional<ProcessedTask> {
                if (enable_progress_log) {
                    LOG_INFO("Engine", "Processing image: " + prepared.in_path);
                }

                crossscale::ProcessingConfig local_config = config;
                
                if (auto_tile || local_config.auto_tile_size) {
                    LOG_INFO("Task", "Auto-tile for " + prepared.in_path);
                    crossscale::TileSizeOptimizer optimizer;
                    crossscale::ModelConstraints mc;
                    mc.model_name = model_name_used;
                    mc.scale_factor = eng.get_scale_factor();
                    mc.input_channels = prepared.channels;
                    mc.model_size_mb = static_cast<float>(model_total_size) / (1024 * 1024);
                    mc.min_tile_size = local_config.min_tile_size;
                    mc.max_tile_size = local_config.max_tile_size;
                    
                    local_config.tile_size = optimizer.find_optimal_tile_size(
                        eng, mc, local_config,
                        prepared.width, prepared.height
                    );
                    local_config.auto_tile_size = true;
                    LOG_INFO("Task", "Optimal tile size: " + std::to_string(local_config.tile_size));
                }
                
                crossscale::TileConfig tile_config;
                tile_config.tile_size = local_config.tile_size;
                tile_config.overlap = local_config.tile_overlap;
                tile_config.batch_size = local_config.batch_size;
                
                int out_width = prepared.width * eng.get_scale_factor();
                int out_height = prepared.height * eng.get_scale_factor();
                crossscale::Image output_image(out_width, out_height, prepared.channels);
                
                crossscale::TileProcessor processor;
                auto result = processor.process_image_u8(
                    prepared.input_image.data(), output_image.data(),
                    prepared.width, prepared.height, prepared.channels,
                    eng, tile_config,
                    [&](const crossscale::ProcessingProgress& prog) {
                        // Always print progress to stderr
                        print_progress_stderr(prog, batch_start_time);
                        if (enable_progress_log &&
                            prog.total_tiles > 0 &&
                            (prog.completed_tiles % 10 == 0 || prog.completed_tiles == prog.total_tiles)) {
                            float pct = prog.percentage();
                            LOG_INFO("Engine", "Progress: " + std::to_string(prog.completed_tiles) + "/" + std::to_string(prog.total_tiles) + " tiles (" + std::to_string(pct) + "%)");
                        }
                    },
                    g_cancel_requested
                );
                
                if (!result.has_value()) {
                    if (g_cancel_requested.load()) {
                        LOG_WARNING("Task", "Processing cancelled by signal");
                    }
                    LOG_ERROR("Task", "Processing failed: " + result.error().message);
                    return std::nullopt;
                }
                
                ProcessedTask processed;
                processed.out_path = prepared.out_path;
                processed.out_width = out_width;
                processed.out_height = out_height;
                processed.channels = prepared.channels;
                processed.output_image = std::move(output_image);
                return processed;
            };

            auto save_processed_task = [&](ProcessedTask&& processed) -> bool {
                std::filesystem::path out_path_obj(processed.out_path);
                const std::filesystem::path final_out_path = crossscale::resolve_output_path(out_path_obj, config);
                if (!final_out_path.parent_path().empty()) {
                    std::filesystem::create_directories(final_out_path.parent_path());
                }

                auto save_res = processed.output_image.save(final_out_path, config.output_quality);
                if (!save_res.has_value()) {
                    LOG_ERROR("Task", "Failed to save output: " + save_res.error().message + " -> " + final_out_path.string());
                    return false;
                }

                if (enable_progress_log) {
                    LOG_INFO("Task", "Completed: " + final_out_path.string());
                }
                return true;
            };

            auto process_one = [&](const std::string& in_path, const std::string& out_path, crossscale::Engine& eng) -> bool {
                auto prepared = prepare_task(in_path, out_path);
                if (!prepared.has_value()) {
                    return false;
                }
                auto processed = infer_prepared_task(std::move(prepared.value()), eng);
                if (!processed.has_value()) {
                    return false;
                }
                return save_processed_task(std::move(processed.value()));
            };
            
            if (jobs <= 1) {

                auto engine = crossscale::create_ncnn_engine();
                auto device_type = use_gpu ? crossscale::DeviceType::GPU_VULKAN : crossscale::DeviceType::CPU;
                auto init_result = engine->initialize(device_type);
                if (!init_result.has_value()) {
                    LOG_ERROR("Engine", "Failed to initialize engine");
                    return 1;
                }
                const bool initialized_on_gpu = engine->current_device() == crossscale::DeviceType::GPU_VULKAN;
                LOG_INFO("Engine", "Engine initialized on " + std::string(initialized_on_gpu ? "GPU" : "CPU"));
                
                // Load model once
                auto load_result = engine->load_model(param_path, bin_path);
                if (!load_result.has_value()) {
                    LOG_ERROR("Engine", "Failed to load model: " + load_result.error().message);
                    return 1;
                }
                LOG_INFO("Engine", "Model loaded successfully");
                
                int scale = engine->get_scale_factor();
                LOG_INFO("Engine", "Scale factor: " + std::to_string(scale) + "x");

                // Auto tile size: use engine's VRAM-aware recommendation if not explicitly set
                if (!tile_size_explicit && !auto_tile) {
                    tile_size = engine->get_recommended_tile_size();
                    config.tile_size = tile_size;
                    LOG_INFO("Engine", "Auto-selected tile size: " + std::to_string(tile_size));
                }
                
                size_t success_count = 0;
                if (use_gpu && tasks.size() > 1) {
                    size_t kSaveWorkers = 2;
                    if (unsigned int hw_threads = std::thread::hardware_concurrency(); hw_threads > 0) {
                        kSaveWorkers = std::clamp(static_cast<size_t>(hw_threads / 8), static_cast<size_t>(2), static_cast<size_t>(4));
                    }
                    size_t kPrepareWorkers = 2;
                    if (unsigned int hw_threads = std::thread::hardware_concurrency(); hw_threads > 0) {
                        kPrepareWorkers = std::clamp(static_cast<size_t>(hw_threads / 4), static_cast<size_t>(2), static_cast<size_t>(6));
                    }
                    const size_t prepared_queue_limit = std::max(static_cast<size_t>(4), kPrepareWorkers * 2);
                    const size_t processed_queue_limit = std::max(static_cast<size_t>(2), kSaveWorkers * 2);
                    std::deque<PreparedTask> prepared_queue;
                    std::deque<ProcessedTask> processed_queue;
                    std::mutex prepared_mutex;
                    std::mutex processed_mutex;
                    std::condition_variable prepared_cv;
                    std::condition_variable processed_cv;
                    constexpr auto kCancelPoll = std::chrono::milliseconds(50);
                    bool prepare_done = false;
                    bool process_done = false;
                    std::atomic<size_t> save_success_count{0};
                    std::atomic<size_t> active_preparers{kPrepareWorkers};

                    std::atomic<size_t> next_prepare{0};
                    std::vector<std::thread> prepare_threads;
                    prepare_threads.reserve(kPrepareWorkers);
                    for (size_t i = 0; i < kPrepareWorkers; ++i) {
                        prepare_threads.emplace_back([&]() {
                            while (true) {
                                if (g_cancel_requested.load()) {
                                    break;
                                }
                                size_t idx = next_prepare.fetch_add(1);
                                if (idx >= tasks.size()) {
                                    break;
                                }
                                const auto& task = tasks[idx];
                                auto prepared = prepare_task(task.first, task.second);
                                if (!prepared.has_value()) {
                                    continue;
                                }
                                std::unique_lock<std::mutex> lock(prepared_mutex);
                                while (prepared_queue.size() >= prepared_queue_limit && !g_cancel_requested.load()) {
                                    prepared_cv.wait_for(lock, kCancelPoll);
                                }
                                if (g_cancel_requested.load()) {
                                    break;
                                }
                                prepared_queue.emplace_back(std::move(prepared.value()));
                                lock.unlock();
                                prepared_cv.notify_one();
                            }
                            if (active_preparers.fetch_sub(1) == 1) {
                                std::lock_guard<std::mutex> lock(prepared_mutex);
                                prepare_done = true;
                                prepared_cv.notify_all();
                            }
                        });
                    }

                    std::thread process_thread([&]() {
                        crossscale::TileProcessor queue_processor;
                        const bool use_gpu_submit_queue =
                            gpu_submit_queue_max > 1 &&
                            engine->supports_gpu_tile_pipeline_u8_queue() &&
                            !(auto_tile || config.auto_tile_size);
                        size_t runtime_queue_depth_cap = gpu_submit_queue_max;
                        size_t last_avg_tiles_per_image = 0;
                        if (use_gpu_submit_queue) {
                            LOG_INFO("Engine", "GPU submit queue enabled, depth=" +
                                               std::to_string(gpu_submit_queue_min) + "-" +
                                               std::to_string(gpu_submit_queue_max));
                            if (gpu_dynamic_submit_queue) {
                                LOG_INFO("Engine", "GPU submit queue auto target_tiles=" +
                                                   std::to_string(gpu_submit_queue_target_tiles));
                            }
                            if (gpu_batch_wait_ms > 0 || gpu_min_submit_batch > 1) {
                                LOG_INFO("Engine", "GPU submit batching min=" + std::to_string(gpu_min_submit_batch) +
                                                       " wait_ms=" + std::to_string(gpu_batch_wait_ms));
                            }
                        }

                        auto enqueue_processed = [&](ProcessedTask&& processed) {
                            std::unique_lock<std::mutex> lock(processed_mutex);
                            while (processed_queue.size() >= processed_queue_limit && !g_cancel_requested.load()) {
                                processed_cv.wait_for(lock, kCancelPoll);
                            }
                            if (g_cancel_requested.load()) {
                                return false;
                            }
                            processed_queue.emplace_back(std::move(processed));
                            lock.unlock();
                            processed_cv.notify_one();
                            return true;
                        };

                        while (true) {
                            if (g_cancel_requested.load()) {
                                std::lock_guard<std::mutex> lock(processed_mutex);
                                process_done = true;
                                processed_cv.notify_all();
                                prepared_cv.notify_all();
                                break;
                            }
                            std::vector<PreparedTask> prepared_batch;
                            {
                                std::unique_lock<std::mutex> lock(prepared_mutex);
                                while (prepared_queue.empty() && !prepare_done && !g_cancel_requested.load()) {
                                    prepared_cv.wait_for(lock, kCancelPoll);
                                }
                                if ((prepared_queue.empty() && prepare_done) || g_cancel_requested.load()) {
                                    break;
                                }

                                const size_t wait_target = use_gpu_submit_queue
                                    ? std::clamp(
                                        std::max(gpu_min_submit_batch, gpu_submit_queue_min),
                                        static_cast<size_t>(1),
                                        runtime_queue_depth_cap
                                    )
                                    : static_cast<size_t>(1);

                                // Give prepare threads a short window to fill a fuller GPU submit batch.
                                if (use_gpu_submit_queue &&
                                    gpu_batch_wait_ms > 0 &&
                                    wait_target > 1 &&
                                    !prepare_done &&
                                    prepared_queue.size() < wait_target) {
                                    const auto wait_deadline = std::chrono::steady_clock::now() +
                                                               std::chrono::milliseconds(gpu_batch_wait_ms);
                                    while (!prepare_done &&
                                           prepared_queue.size() < wait_target &&
                                           std::chrono::steady_clock::now() < wait_deadline) {
                                        if (prepared_cv.wait_until(lock, wait_deadline, [&]() {
                                                return prepare_done || prepared_queue.size() >= wait_target;
                                            })) {
                                            break;
                                        }
                                    }
                                }

                                size_t batch_target = use_gpu_submit_queue ? runtime_queue_depth_cap : static_cast<size_t>(1);
                                if (use_gpu_submit_queue && gpu_dynamic_submit_queue) {
                                    const size_t queued_now = prepared_queue.size();
                                    const size_t prep_fill = (!prepare_done && queued_now < runtime_queue_depth_cap)
                                        ? std::min(runtime_queue_depth_cap - queued_now, kPrepareWorkers)
                                        : 0;
                                    const size_t anticipated_ready = queued_now + prep_fill;
                                    batch_target = std::clamp(
                                        anticipated_ready,
                                        gpu_submit_queue_min,
                                        runtime_queue_depth_cap
                                    );
                                    if (last_avg_tiles_per_image > 0) {
                                        const size_t tiles_target_depth = std::clamp(
                                            (gpu_submit_queue_target_tiles + last_avg_tiles_per_image - 1) /
                                                last_avg_tiles_per_image,
                                            gpu_submit_queue_min,
                                            runtime_queue_depth_cap
                                        );
                                        batch_target = std::min(batch_target, tiles_target_depth);
                                    }
                                    if (!prepare_done) {
                                        batch_target = std::max(batch_target, wait_target);
                                    }
                                }
                                batch_target = std::max(static_cast<size_t>(1), batch_target);

                                prepared_batch.emplace_back(std::move(prepared_queue.front()));
                                prepared_queue.pop_front();
                                while (prepared_batch.size() < batch_target && !prepared_queue.empty()) {
                                    prepared_batch.emplace_back(std::move(prepared_queue.front()));
                                    prepared_queue.pop_front();
                                }
                            }
                            prepared_cv.notify_all();

                            if (!use_gpu_submit_queue) {
                                for (auto& prepared : prepared_batch) {
                                    auto processed = infer_prepared_task(std::move(prepared), *engine);
                                    if (!processed.has_value()) {
                                        continue;
                                    }
                                    if (!enqueue_processed(std::move(processed.value()))) {
                                        break;
                                    }
                                }
                                if (g_cancel_requested.load()) {
                                    break;
                                }
                                continue;
                            }

                            struct QueueBatchItem {
                                PreparedTask prepared;
                                ProcessedTask processed;
                                std::vector<crossscale::TileRegion> regions;
                                crossscale::TileConfig tile_config;
                            };
                            std::vector<QueueBatchItem> queue_items;
                            queue_items.reserve(prepared_batch.size());
                            size_t batch_tiles_total = 0;

                            for (auto& prepared : prepared_batch) {
                                if (enable_progress_log) {
                                    LOG_INFO("Engine", "Processing image: " + prepared.in_path);
                                }

                                crossscale::TileConfig tile_config;
                                tile_config.tile_size = config.tile_size;
                                tile_config.overlap = config.tile_overlap;
                                tile_config.batch_size = config.batch_size;

                                QueueBatchItem item;
                                item.prepared = std::move(prepared);
                                item.tile_config = tile_config;
                                item.regions = queue_processor.compute_tile_layout(
                                    item.prepared.width, item.prepared.height, tile_config
                                );
                                batch_tiles_total += item.regions.size();
                                item.processed.out_path = item.prepared.out_path;
                                item.processed.out_width = item.prepared.width * engine->get_scale_factor();
                                item.processed.out_height = item.prepared.height * engine->get_scale_factor();
                                item.processed.channels = item.prepared.channels;
                                item.processed.output_image = crossscale::Image(
                                    item.processed.out_width,
                                    item.processed.out_height,
                                    item.processed.channels
                                );
                                queue_items.emplace_back(std::move(item));
                            }
                            if (!queue_items.empty()) {
                                last_avg_tiles_per_image = std::max(
                                    static_cast<size_t>(1),
                                    (batch_tiles_total + queue_items.size() - 1) / queue_items.size()
                                );
                            }

                            std::vector<crossscale::GPUTileTaskU8> gpu_tasks;
                            gpu_tasks.reserve(queue_items.size());
                            for (auto& item : queue_items) {
                                gpu_tasks.push_back(crossscale::GPUTileTaskU8{
                                    .input_data = item.prepared.input_image.data(),
                                    .output_data = item.processed.output_image.data(),
                                    .input_width = item.prepared.width,
                                    .input_height = item.prepared.height,
                                    .channels = item.prepared.channels,
                                    .regions = &item.regions,
                                    .config = item.tile_config,
                                    .progress_callback = {}
                                });
                            }

                            auto queue_result = engine->process_tiles_gpu_u8_queue(gpu_tasks, g_cancel_requested);
                            if (!queue_result.has_value()) {
                                if (g_cancel_requested.load()) {
                                    break;
                                }
                                if (prepared_batch.size() > 1 && runtime_queue_depth_cap > gpu_submit_queue_min) {
                                    runtime_queue_depth_cap = std::max(gpu_submit_queue_min, runtime_queue_depth_cap - 1);
                                    LOG_WARNING("Engine", "Reducing GPU submit queue depth cap to " +
                                                          std::to_string(runtime_queue_depth_cap));
                                }
                                LOG_WARNING("Engine", "GPU submit queue failed, fallback to single image path: " +
                                                      queue_result.error().message);
                                for (auto& item : queue_items) {
                                    auto processed = infer_prepared_task(std::move(item.prepared), *engine);
                                    if (!processed.has_value()) {
                                        continue;
                                    }
                                    if (!enqueue_processed(std::move(processed.value()))) {
                                        break;
                                    }
                                }
                                if (g_cancel_requested.load()) {
                                    break;
                                }
                                continue;
                            }
                            if (runtime_queue_depth_cap < gpu_submit_queue_max &&
                                prepared_batch.size() >= runtime_queue_depth_cap) {
                                runtime_queue_depth_cap++;
                            }

                            for (auto& item : queue_items) {
                                if (!enqueue_processed(std::move(item.processed))) {
                                    break;
                                }
                            }
                            if (g_cancel_requested.load()) {
                                break;
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(processed_mutex);
                            process_done = true;
                        }
                        processed_cv.notify_all();
                    });

                    std::vector<std::thread> save_threads;
                    save_threads.reserve(kSaveWorkers);
                    for (size_t i = 0; i < kSaveWorkers; ++i) {
                        save_threads.emplace_back([&]() {
                            while (true) {
                                std::optional<ProcessedTask> processed;
                                {
                                    std::unique_lock<std::mutex> lock(processed_mutex);
                                    while (processed_queue.empty() && !process_done && !g_cancel_requested.load()) {
                                        processed_cv.wait_for(lock, kCancelPoll);
                                    }
                                    if ((processed_queue.empty() && process_done) || g_cancel_requested.load()) {
                                        break;
                                    }
                                    processed = std::move(processed_queue.front());
                                    processed_queue.pop_front();
                                }
                                processed_cv.notify_one();

                                if (save_processed_task(std::move(processed.value()))) {
                                    save_success_count.fetch_add(1);
                                }
                            }
                        });
                    }

                    for (auto& prepare_thread : prepare_threads) {
                        prepare_thread.join();
                    }
                    process_thread.join();
                    for (auto& save_thread : save_threads) {
                        save_thread.join();
                    }
                    success_count = save_success_count.load();
                } else {
                    for (const auto& task : tasks) {
                        if (g_cancel_requested.load()) {
                            break;
                        }
                        if (process_one(task.first, task.second, *engine)) {
                            success_count++;
                        }
                    }
                }
                
                LOG_INFO("CLI", "Processing summary: " + std::to_string(success_count) + "/" + std::to_string(tasks.size()) + " succeeded");
                return success_count == tasks.size() ? 0 : 1;
            } else {
                if (use_gpu) {
                    size_t requested_workers = static_cast<size_t>(std::max(1, jobs));
                    size_t worker_count = std::min(requested_workers, tasks.size());

                    struct EngineInitResult {
                        std::unique_ptr<crossscale::Engine> engine;
                        std::string error;
                    };

                    std::vector<std::future<EngineInitResult>> init_futures;
                    init_futures.reserve(worker_count);
                    for (size_t i = 0; i < worker_count; ++i) {
                        init_futures.emplace_back(std::async(std::launch::async, [&, i]() -> EngineInitResult {
                            auto engine = crossscale::create_ncnn_engine();
                            auto init_result = engine->initialize(crossscale::DeviceType::GPU_VULKAN);
                            if (!init_result.has_value()) {
                                return {nullptr, "Failed to initialize GPU engine #" + std::to_string(i)};
                            }
                            auto load_result = engine->load_model(param_path, bin_path);
                            if (!load_result.has_value()) {
                                return {nullptr, "Failed to load model into GPU engine #" + std::to_string(i) +
                                                    ": " + load_result.error().message};
                            }
                            return {std::move(engine), ""};
                        }));
                    }

                    std::vector<std::unique_ptr<crossscale::Engine>> gpu_engines;
                    gpu_engines.reserve(worker_count);
                    for (auto& init_future : init_futures) {
                        auto init_result = init_future.get();
                        if (!init_result.engine) {
                            LOG_ERROR("Engine", init_result.error);
                            return 1;
                        }
                        gpu_engines.push_back(std::move(init_result.engine));
                    }
                    LOG_INFO("Engine", "GPU engine workers ready: " + std::to_string(worker_count));

                    std::atomic<size_t> next_task{0};
                    std::atomic<size_t> success_count{0};
                    std::vector<std::thread> workers;
                    workers.reserve(worker_count);

                    for (size_t i = 0; i < worker_count; ++i) {
                        workers.emplace_back([&, i]() {
                            auto& eng = *gpu_engines[i];
                            while (true) {
                                size_t idx = next_task.fetch_add(1);
                                if (idx >= tasks.size()) {
                                    break;
                                }
                                if (process_one(tasks[idx].first, tasks[idx].second, eng)) {
                                    success_count.fetch_add(1);
                                }
                            }
                        });
                    }

                    for (auto& w : workers) {
                        w.join();
                    }

                    LOG_INFO("CLI", "Processing summary: " + std::to_string(success_count) + "/" + std::to_string(tasks.size()) + " succeeded");
                    return success_count == tasks.size() ? 0 : 1;
                } else {
                    // CPU EnginePool
                    crossscale::EnginePool pool(static_cast<size_t>(jobs), param_path, bin_path, crossscale::DeviceType::CPU);
                    LOG_INFO("Engine", "Created engine pool with " + std::to_string(jobs) + " CPU engines");

                    std::vector<std::future<bool>> futures;
                    futures.reserve(tasks.size());

                    for (const auto& task : tasks) {
                        futures.emplace_back(std::async(std::launch::async, [&, task]() -> bool {
                            crossscale::Engine* eng = pool.acquire();
                            if (!eng) {
                                LOG_ERROR("Engine", "Failed to acquire engine from pool");
                                return false;
                            }
                            bool ok = false;
                            try {
                                ok = process_one(task.first, task.second, *eng);
                            } catch (const std::exception& e) {
                                LOG_ERROR("Engine", std::string("Task processing exception: ") + e.what());
                                ok = false;
                            } catch (...) {
                                LOG_ERROR("Engine", "Task processing unknown exception");
                                ok = false;
                            }
                            try {
                                pool.release(eng);
                            } catch (...) {
                                LOG_ERROR("Engine", "Failed to release engine back to pool");
                            }
                            return ok;
                        }));
                    }

                    size_t success_count = 0;
                    for (auto& fut : futures) {
                        if (fut.get()) success_count++;
                    }

                    LOG_INFO("CLI", "Processing summary: " + std::to_string(success_count) + "/" + std::to_string(tasks.size()) + " succeeded");
                    return success_count == tasks.size() ? 0 : 1;
                }
            }
        }
        
        // Single file processing continues...
        LOG_INFO("Engine", "Loading image: " + input_path);

        auto image_result = crossscale::Image::load(input_path);
        if (!image_result.has_value()) {
            LOG_ERROR("Engine", "Failed to load image: " + image_result.error().message);
            return 1;
        }

        auto& image = image_result.value();
        LOG_INFO("Engine", "Image loaded: " + std::to_string(image.width()) + "x" +
                 std::to_string(image.height()) + " (" + std::to_string(image.channels()) + " channels)");

        auto engine = crossscale::create_ncnn_engine();
        auto init_result = engine->initialize();  // Try GPU first, fallback to CPU
        if (!init_result.has_value()) {
            LOG_ERROR("Engine", "Failed to initialize engine");
            return 1;
        }

        std::string param_path, bin_path;

        // Load model using ModelManager
        crossscale::ModelManager manager(fs::current_path() / "models");
        auto models_opt = manager.scan_models();
        if (!models_opt.has_value() || models_opt->empty()) {
            LOG_ERROR("Engine", "No models found in " + models_dir);
            return 1;
        }
        auto& models = models_opt.value();

        const crossscale::ModelInfo* selected_model = nullptr;
        if (model_name.empty()) {
            selected_model = &models[0];
            LOG_INFO("Engine", "No model specified, using first available: " + selected_model->name);
        } else {
            for (const auto& m : models) {
                if (m.name == model_name) {
                    selected_model = &m;
                    break;
                }
            }
            if (!selected_model) {
                LOG_ERROR("Engine", "Model not found: " + model_name);
                LOG_ERROR("Engine", "Available models:");
                for (const auto& m : models) {
                    LOG_ERROR("Engine", "  - " + m.name + " (" + std::to_string(m.scale_factor) + "x)");
                }
                return 1;
            }
        }

        param_path = selected_model->param_path.string();
        bin_path = selected_model->bin_path.string();
        model_name = selected_model->name;
        int model_input_channels = std::max(1, selected_model->input_channels);
        LOG_INFO("Engine", "Using model: " + model_name + " (" + std::to_string(selected_model->scale_factor) + "x)");

        auto load_result = engine->load_model(param_path, bin_path);
        if (!load_result.has_value()) {
            LOG_ERROR("Engine", "Failed to load model: " + load_result.error().message);
            return 1;
        }

        LOG_INFO("Engine", "Model loaded successfully");

        int scale = engine->get_scale_factor();
        LOG_INFO("Engine", "Scale factor: " + std::to_string(scale) + "x");

        // Auto tile size: use engine's VRAM-aware recommendation if not explicitly set
        if (!tile_size_explicit && !auto_tile) {
            tile_size = engine->get_recommended_tile_size();
            LOG_INFO("Engine", "Auto-selected tile size: " + std::to_string(tile_size));
        }

        int input_channels_before = image.channels();
        if (!normalize_channels_for_model(image, model_input_channels, input_path)) {
            return 1;
        }
        if (image.channels() != input_channels_before) {
            LOG_INFO(
                "Engine",
                "Adjusted input channels: " + std::to_string(input_channels_before) +
                    " -> " + std::to_string(image.channels())
            );
        }

        // 加载配置文件（如果指定）
        crossscale::ProcessingConfig config;
        if (!config_file.empty()) {
            auto cfg_result = crossscale::ProcessingConfig::load(config_file);
            if (cfg_result.has_value()) {
                config = cfg_result.value();
                LOG_INFO("Config", "Loaded config from " + config_file);
            } else {
                LOG_WARNING("Config", "Failed to load config: " + cfg_result.error());
            }
        }

        // 命令行参数覆盖配置文件（显式指定的参数优先）
        config.tile_size = tile_size;
        config.tile_overlap = tile_overlap;
        config.auto_tile_size = auto_tile;
        config.min_tile_size = min_tile;
        config.max_tile_size = max_tile;
        if (cli_output_quality >= 0) {
            config.output_quality = cli_output_quality;
        }
        if (!cli_output_format.empty()) {
            config.output_format = crossscale::normalize_output_format(cli_output_format);
        } else {
            // Auto-infer output format from output file extension
            std::string inferred = infer_format_from_extension(output_path);
            if (!inferred.empty()) {
                config.output_format = crossscale::normalize_output_format(inferred);
                LOG_INFO("Config", "Auto-detected output format: " + inferred);
            }
        }

        // 如果启用自动调优，运行基准测试选择最优 tile size
        if (auto_tile || config.auto_tile_size) {
            LOG_INFO("Config", "Auto-tile enabled, benchmarking...");

            crossscale::TileSizeOptimizer optimizer;
            crossscale::ModelConstraints model_constraints;
            model_constraints.model_name = model_name;
            model_constraints.scale_factor = scale;
            model_constraints.input_channels = image.channels();
            model_constraints.model_size_mb = selected_model->total_size / (1024 * 1024);
            model_constraints.min_tile_size = config.min_tile_size;
            model_constraints.max_tile_size = config.max_tile_size;

            config.tile_size = optimizer.find_optimal_tile_size(
                *engine, model_constraints, config,
                image.width(), image.height()
            );
            config.auto_tile_size = true;

            LOG_INFO("Config", "Optimal tile size determined: " + std::to_string(config.tile_size));
        }

        // 验证配置
        auto errors = config.validate();
        if (!errors.empty()) {
            LOG_ERROR("Config", "Configuration errors:");
            for (const auto& err : errors) {
                LOG_ERROR("Config", "  - " + err);
            }
            return 1;
        }

        // 使用 TileProcessor 进行瓦片处理
        crossscale::TileProcessor processor;
        crossscale::TileConfig tile_config;
        tile_config.tile_size = config.tile_size;
        tile_config.overlap = config.tile_overlap;
        tile_config.batch_size = batch_size;

        int out_width = image.width() * scale;
        int out_height = image.height() * scale;
        crossscale::Image output_image(out_width, out_height, image.channels());

        LOG_INFO("Engine", "Processing image with tile_size=" + std::to_string(config.tile_size) +
                " overlap=" + std::to_string(config.tile_overlap) +
                " batch_size=" + std::to_string(batch_size) + "...");

        std::atomic<bool> cancel_flag{false};
        bool enable_progress_log_single = false;
        if (const char* progress_env = std::getenv("CROSSSCALE_PROGRESS")) {
            enable_progress_log_single = std::string(progress_env) == "1";
        }

        auto process_start_time = std::chrono::steady_clock::now();
        auto process_result = processor.process_image_u8(
            image.data(), output_image.data(),
            image.width(), image.height(), image.channels(),
            *engine, tile_config,
            [&](const crossscale::ProcessingProgress& progress) {
                // Always print progress to stderr with ETA
                print_progress_stderr(progress, process_start_time);
                if (enable_progress_log_single && progress.total_tiles > 0) {
                    float pct = progress.percentage();
                    if (progress.completed_tiles % 10 == 0 ||
                        progress.completed_tiles == progress.total_tiles) {
                        LOG_INFO("Engine", "Progress: " + std::to_string(progress.completed_tiles) +
                                "/" + std::to_string(progress.total_tiles) +
                                " tiles (" + std::to_string(pct) + "%)");
                    }
                }
            },
            cancel_flag
        );

        if (!process_result.has_value()) {
            LOG_ERROR("Engine", "Processing failed: " + process_result.error().message);
            return 1;
        }

        const auto final_output_path = crossscale::resolve_output_path(output_path, config);
        auto save_result = output_image.save(final_output_path, config.output_quality);
        if (!save_result.has_value()) {
            LOG_ERROR("Engine", "Failed to save output: " + save_result.error().message);
            return 1;
        }

        LOG_INFO("Engine", "Output saved: " + final_output_path.string());
        LOG_INFO("Engine", "Processing complete: " + std::to_string(out_width) + "x" + std::to_string(out_height));
        return 0;
    }



    if (command == "model") {
        std::string subcommand = argc > 2 ? argv[2] : "";
        
        if (subcommand == "list") {
            crossscale::ModelManager manager(fs::current_path() / "models");
            auto models = manager.scan_models();
            
            if (!models.has_value()) {
                LOG_ERROR("Model", "Failed to scan models: " + models.error().message);
                return 1;
            }

            std::cout << "Available models:\n";
            for (const auto& m : models.value()) {
                std::cout << "  " << m.name << " (" << m.scale_factor << "x, " 
                          << (m.total_size / 1024 / 1024) << " MB)\n";
            }
            return 0;
        }
        
        LOG_ERROR("CLI", "Unknown model subcommand: " + subcommand);
        return 1;
    }

    if (command == "benchmark") {
        LOG_INFO("Benchmark", "Running performance benchmark...");
        
        crossscale::Timer timer;
        auto result = crossscale::benchmark("test_iteration", []() {
            volatile int x = 0;
            for (int i = 0; i < 1000000; ++i) x += i;
            (void)x;
        }, 10, 2);
        
        std::cout << "Benchmark: " << result.name << "\n"
                  << "  Iterations: " << result.iterations << "\n"
                  << "  Avg time: " << result.avg_time_ms << " ms\n"
                  << "  Ops/sec: " << result.ops_per_second << "\n";
        return 0;
    }

    if (command == "perf") {
        // Perf: modular performance testing.
        //   perf gpu-efficiency [--model <name>] [--tile 256] [--batch 1]
        //                       [--duration 5s] [--warmup 1s] [--no-cpu-baseline]
        //                       [--out events.jsonl] [--sample-ms 100] [--models-dir ./models]
        //   perf list-probes
        std::string subcommand = argc > 2 ? argv[2] : "";

        if (subcommand.empty() || subcommand == "--help" || subcommand == "-h") {
            std::cout << "CrossScale perf - performance testing\n\n"
                      << "Usage:\n"
                      << "  crossscale perf gpu-efficiency [options]\n"
                      << "  crossscale perf gpu-concurrency [options]\n"
                      << "  crossscale perf gpu-reuse [options]\n"
                      << "  crossscale perf list-probes\n\n"
                      << "gpu-efficiency options:\n"
                      << "  --model <name>        Model name (default: first available)\n"
                      << "  --tile <n>            Tile size (default: 256)\n"
                      << "  --duration <Ns>       Run length, e.g. 5s (default: 5s)\n"
                      << "  --warmup <Ns>         Warmup window (default: 1s)\n"
                      << "  --no-cpu-baseline     Skip CPU comparison run\n"
                      << "  --out <path>          JSONL output file (default: stdout)\n"
                      << "  --sample-ms <n>       Probe sample interval (default: 100)\n"
                      << "  --models-dir <path>   Models directory (default: ./models)\n\n"
                      << "gpu-concurrency options:\n"
                      << "  --concurrency <n>     Independent GPU engines/threads (default: 2)\n"
                      << "  (also accepts --model/--tile/--duration/--warmup/--out/--sample-ms/--models-dir)\n\n"
                      << "gpu-reuse options:\n"
                      << "  --concurrency <n>     Worker threads (default: 1; use 2 to test overlap)\n"
                      << "  (also accepts --model/--tile/--duration/--warmup/--out/--sample-ms/--models-dir)\n";
            return 0;
        }

        if (subcommand == "list-probes") {
            crossscale::perf::list_available();
            return 0;
        }

        if (subcommand == "gpu-efficiency") {
            std::string model_name;
            std::string models_dir = "./models";
            std::string out_path;
            int tile_size = 256;
            double duration_s = 5.0;
            double warmup_s = 1.0;
            int sample_ms = 100;
            bool no_cpu_baseline = false;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--model" && i + 1 < argc) model_name = argv[++i];
                else if (arg == "--models-dir" && i + 1 < argc) models_dir = argv[++i];
                else if (arg == "--tile" && i + 1 < argc) tile_size = safe_parse_int<int>(argv[++i], "--tile", 16, 8192);
                else if (arg == "--duration" && i + 1 < argc) {
                    std::string d = argv[++i];
                    if (!d.empty() && d.back() == 's') d.pop_back();
                    duration_s = std::stod(d);
                }
                else if (arg == "--warmup" && i + 1 < argc) {
                    std::string w = argv[++i];
                    if (!w.empty() && w.back() == 's') w.pop_back();
                    warmup_s = std::stod(w);
                }
                else if (arg == "--sample-ms" && i + 1 < argc) sample_ms = safe_parse_int<int>(argv[++i], "--sample-ms", 10, 10000);
                else if (arg == "--no-cpu-baseline") no_cpu_baseline = true;
                else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
            }

            // Resolve model via ModelManager.
            crossscale::ModelManager mgr;
            mgr.set_models_directory(models_dir);
            auto scan_res = mgr.scan_models();
            if (!scan_res) {
                LOG_ERROR("Perf", "model scan failed: " + scan_res.error().message);
                return 1;
            }
            auto& models = scan_res.value();
            if (models.empty()) {
                LOG_ERROR("Perf", "no models found in: " + models_dir);
                return 1;
            }
            const auto* sel = &models[0];
            if (!model_name.empty()) {
                auto it = std::find_if(models.begin(), models.end(),
                    [&](const auto& m) { return m.name == model_name; });
                if (it == models.end()) {
                    LOG_ERROR("Perf", "model not found: " + model_name);
                    for (const auto& m : models) LOG_INFO("Perf", "  available: " + m.name);
                    return 1;
                }
                sel = &*it;
            }

            crossscale::perf::TestConfig cfg;
            cfg.model_param = sel->param_path.string();
            cfg.model_bin = sel->bin_path.string();
            cfg.model_name = sel->name;
            cfg.tile_width = tile_size;
            cfg.tile_height = tile_size;
            cfg.scale = sel->scale_factor;
            cfg.duration_s = duration_s;
            cfg.warmup_s = warmup_s;
            cfg.sample_ms = sample_ms;
            cfg.cpu_baseline = !no_cpu_baseline;

            return crossscale::perf::run_test("gpu-efficiency", cfg, out_path);
        }

        if (subcommand == "gpu-concurrency") {
            std::string model_name;
            std::string models_dir = "./models";
            std::string out_path;
            int tile_size = 256;
            int concurrency = 2;
            double duration_s = 6.0;
            double warmup_s = 2.0;
            int sample_ms = 100;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--model" && i + 1 < argc) model_name = argv[++i];
                else if (arg == "--models-dir" && i + 1 < argc) models_dir = argv[++i];
                else if (arg == "--tile" && i + 1 < argc) tile_size = safe_parse_int<int>(argv[++i], "--tile", 16, 8192);
                else if (arg == "--concurrency" && i + 1 < argc) concurrency = safe_parse_int<int>(argv[++i], "--concurrency", 1, 16);
                else if (arg == "--duration" && i + 1 < argc) {
                    std::string d = argv[++i];
                    if (!d.empty() && d.back() == 's') d.pop_back();
                    duration_s = std::stod(d);
                }
                else if (arg == "--warmup" && i + 1 < argc) {
                    std::string w = argv[++i];
                    if (!w.empty() && w.back() == 's') w.pop_back();
                    warmup_s = std::stod(w);
                }
                else if (arg == "--sample-ms" && i + 1 < argc) sample_ms = safe_parse_int<int>(argv[++i], "--sample-ms", 10, 10000);
                else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
            }

            crossscale::ModelManager mgr;
            mgr.set_models_directory(models_dir);
            auto scan_res = mgr.scan_models();
            if (!scan_res) {
                LOG_ERROR("Perf", "model scan failed: " + scan_res.error().message);
                return 1;
            }
            auto& models = scan_res.value();
            if (models.empty()) {
                LOG_ERROR("Perf", "no models found in: " + models_dir);
                return 1;
            }
            const auto* sel = &models[0];
            if (!model_name.empty()) {
                auto it = std::find_if(models.begin(), models.end(),
                    [&](const auto& m) { return m.name == model_name; });
                if (it == models.end()) {
                    LOG_ERROR("Perf", "model not found: " + model_name);
                    for (const auto& m : models) LOG_INFO("Perf", "  available: " + m.name);
                    return 1;
                }
                sel = &*it;
            }

            crossscale::perf::TestConfig cfg;
            cfg.model_param = sel->param_path.string();
            cfg.model_bin = sel->bin_path.string();
            cfg.model_name = sel->name;
            cfg.tile_width = tile_size;
            cfg.tile_height = tile_size;
            cfg.scale = sel->scale_factor;
            cfg.duration_s = duration_s;
            cfg.warmup_s = warmup_s;
            cfg.sample_ms = sample_ms;
            cfg.cpu_baseline = false;  // gpu-concurrency is GPU-only by design
            cfg.concurrency = concurrency;

            return crossscale::perf::run_test("gpu-concurrency", cfg, out_path);
        }

        if (subcommand == "gpu-reuse") {
            // A/B test: fresh VkCompute per tile (Mode A, via engine.process)
            // vs one reused VkCompute per thread with cmd.reset() (Mode B).
            // Verifies output equivalence before timing. Default concurrency 1
            // to isolate the reuse effect; pass --concurrency 2 to also see the
            // overlap impact.
            std::string model_name;
            std::string models_dir = "./models";
            std::string out_path;
            int tile_size = 256;
            int concurrency = 1;
            double duration_s = 6.0;
            double warmup_s = 2.0;
            int sample_ms = 100;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--model" && i + 1 < argc) model_name = argv[++i];
                else if (arg == "--models-dir" && i + 1 < argc) models_dir = argv[++i];
                else if (arg == "--tile" && i + 1 < argc) tile_size = safe_parse_int<int>(argv[++i], "--tile", 16, 8192);
                else if (arg == "--concurrency" && i + 1 < argc) concurrency = safe_parse_int<int>(argv[++i], "--concurrency", 1, 16);
                else if (arg == "--duration" && i + 1 < argc) {
                    std::string d = argv[++i];
                    if (!d.empty() && d.back() == 's') d.pop_back();
                    duration_s = std::stod(d);
                }
                else if (arg == "--warmup" && i + 1 < argc) {
                    std::string w = argv[++i];
                    if (!w.empty() && w.back() == 's') w.pop_back();
                    warmup_s = std::stod(w);
                }
                else if (arg == "--sample-ms" && i + 1 < argc) sample_ms = safe_parse_int<int>(argv[++i], "--sample-ms", 10, 10000);
                else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
            }

            crossscale::ModelManager mgr;
            mgr.set_models_directory(models_dir);
            auto scan_res = mgr.scan_models();
            if (!scan_res) {
                LOG_ERROR("Perf", "model scan failed: " + scan_res.error().message);
                return 1;
            }
            auto& models = scan_res.value();
            if (models.empty()) {
                LOG_ERROR("Perf", "no models found in: " + models_dir);
                return 1;
            }
            const auto* sel = &models[0];
            if (!model_name.empty()) {
                auto it = std::find_if(models.begin(), models.end(),
                    [&](const auto& m) { return m.name == model_name; });
                if (it == models.end()) {
                    LOG_ERROR("Perf", "model not found: " + model_name);
                    for (const auto& m : models) LOG_INFO("Perf", "  available: " + m.name);
                    return 1;
                }
                sel = &*it;
            }

            crossscale::perf::TestConfig cfg;
            cfg.model_param = sel->param_path.string();
            cfg.model_bin = sel->bin_path.string();
            cfg.model_name = sel->name;
            cfg.tile_width = tile_size;
            cfg.tile_height = tile_size;
            cfg.scale = sel->scale_factor;
            cfg.duration_s = duration_s;
            cfg.warmup_s = warmup_s;
            cfg.sample_ms = sample_ms;
            cfg.cpu_baseline = false;
            cfg.concurrency = concurrency;

            return crossscale::perf::run_test("gpu-reuse", cfg, out_path);
        }

        LOG_ERROR("Perf", "Unknown perf subcommand: " + subcommand);
        return 1;
    }

    if (command == "server") {
        std::string subcommand = argc > 2 ? argv[2] : "";

        if (subcommand == "start") {
            uint16_t port = 9527;
            std::string key;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-p" && i + 1 < argc) port = safe_parse_int<uint16_t>(argv[++i], "-p", 1, 65535);
                else if (arg == "-k" && i + 1 < argc) key = argv[++i];
            }

            if (key.empty()) {
                key = crossscale::crypto::generate_uuid().substr(0, 16);
            }

            LOG_INFO("Server", "Starting coordinator on port " + std::to_string(port));
            LOG_INFO("Server", "Auth key: " + key);

            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);

            auto coordinator = std::make_unique<crossscale::Coordinator>();
            coordinator->set_models_directory("./models");
            coordinator->on_device_connected() = [](const crossscale::DeviceProfile& profile) {
                LOG_INFO(
                    "Server",
                    "Worker joined: id=" + profile.device_id +
                        " name=" + profile.device_name +
                        " type=" + std::to_string(static_cast<int>(profile.type)) +
                        " tps=" + std::to_string(profile.tiles_per_second)
                );
            };
            coordinator->on_device_disconnected() = [](const std::string& worker_id) {
                LOG_WARNING("Server", "Worker disconnected: id=" + worker_id);
            };
            coordinator->on_task_progress() = [](float progress) {
                static std::atomic<int> last_pct{-1};
                const auto pct = static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f);
                const int previous = last_pct.exchange(pct);
                if (previous == pct) {
                    return;
                }
                LOG_INFO("Server", "Task progress: " + std::to_string(pct) + "%");
            };

            auto start_result = coordinator->start(port, key);
            if (!start_result.has_value()) {
                LOG_ERROR("Server", "Failed to start coordinator: " + start_result.error().message);
                return 1;
            }

            crossscale::MDNSBroadcaster mdns_broadcaster;
            const auto mdns_result = mdns_broadcaster.start("CrossScale-PC", port, key);
            if (!mdns_result.has_value()) {
                LOG_WARNING("Server", "mDNS publish unavailable: " + mdns_result.error().message);
            }

            LOG_INFO("Server", "Coordinator running. Press Ctrl+C to stop.");

            // Print the full connection card (IP + key + QR) right away so
            // the user can scan it with the phone without a second command.
            print_connection_card(port, key);

            std::optional<size_t> last_worker_count;
            while (g_running && coordinator->is_running()) {
                const size_t worker_count = coordinator->device_count();
                if (!last_worker_count.has_value() || last_worker_count.value() != worker_count) {
                    LOG_INFO("Server", "Connected workers: " + std::to_string(worker_count));
                    last_worker_count = worker_count;
                }
                // Short poll so Ctrl+C (sets g_running=false) is noticed within
                // ~200ms instead of up to 5s.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            LOG_INFO("Server", "Shutting down coordinator...");
            mdns_broadcaster.stop();
            coordinator->stop();
            return 0;
        }

        if (subcommand == "info") {
            uint16_t port = 9527;
            std::string key;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-p" && i + 1 < argc) {
                    port = safe_parse_int<uint16_t>(argv[++i], "-p", 1, 65535);
                } else if (arg == "-k" && i + 1 < argc) {
                    key = argv[++i];
                }
            }

            if (key.empty()) {
                LOG_ERROR("Server", "server info requires -k <key> from running coordinator");
                return 1;
            }

            print_connection_card(port, key);
            return 0;
        }

        LOG_ERROR("CLI", "Unknown server subcommand: " + subcommand);
        return 1;
    }
    
    if (command == "worker") {
        std::string subcommand = argc > 2 ? argv[2] : "";
        
        if (subcommand == "connect") {
            if (argc < 4) {
                LOG_ERROR("CLI", "Usage: worker connect <host>:<port> -k <key>");
                return 1;
            }
            
            std::string host_port = argv[3];
            std::string key;
            float recommended_weight = 0.0f;
            size_t queue_depth = 0;
            int recommended_tile_size = 0;
            
            for (int i = 4; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-k" && i + 1 < argc) key = argv[++i];
                else if (arg == "--recommended-weight" && i + 1 < argc) {
                    recommended_weight = safe_parse_float(argv[++i], "--recommended-weight");
                }
                else if (arg == "--queue-depth" && i + 1 < argc) {
                    queue_depth = static_cast<size_t>(safe_parse_int<int>(argv[++i], "--queue-depth", 1, 1024));
                }
                else if (arg == "--recommended-tile-size" && i + 1 < argc) {
                    recommended_tile_size = safe_parse_int<int>(argv[++i], "--recommended-tile-size", 0, 8192);
                }
            }
            
            if (key.empty()) {
                LOG_ERROR("CLI", "Auth key required (-k <key>)");
                return 1;
            }
            
            size_t colon_pos = host_port.find(':');
            if (colon_pos == std::string::npos) {
                LOG_ERROR("CLI", "Invalid host:port format. Use <host>:<port>");
                return 1;
            }
            
            std::string host = host_port.substr(0, colon_pos);
            uint16_t port = safe_parse_int<uint16_t>(host_port.substr(colon_pos + 1), "host:port", 1, 65535);
            
            LOG_INFO("Worker", "Connecting to coordinator at " + host + ":" + std::to_string(port));
            if (recommended_weight > 0.0f) {
                LOG_INFO("Worker", "Using manual recommended weight: " + std::to_string(recommended_weight));
            }
            if (queue_depth > 0) {
                LOG_INFO("Worker", "Using manual queue depth: " + std::to_string(queue_depth));
            }
            if (recommended_tile_size > 0) {
                LOG_INFO("Worker", "Using manual recommended tile size: " + std::to_string(recommended_tile_size));
            }

            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);

            int reconnect_attempt = 0;
            while (g_running) {
                auto worker = std::make_unique<crossscale::Worker>();
                worker->set_recommended_weight(recommended_weight);
                worker->set_queue_depth(queue_depth);
                worker->set_recommended_tile_size(recommended_tile_size);

                auto conn_result = worker->connect(host, port, key);
                if (!conn_result.has_value()) {
                    LOG_ERROR("Worker", "Failed to connect: " + conn_result.error().message);
                    if (!g_running) {
                        break;
                    }
                    reconnect_attempt += 1;
                    const int retry_delay_seconds = std::min(12, 1 << std::min(reconnect_attempt, 3));
                    LOG_WARNING("Worker", "Reconnect in " + std::to_string(retry_delay_seconds) + "s");
                    std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
                    continue;
                }

                auto engine = crossscale::create_ncnn_engine();
                auto init_result = engine->initialize(crossscale::DeviceType::GPU_VULKAN);
                if (!init_result.has_value()) {
                    LOG_WARNING("Worker", "GPU not available, falling back to CPU");
                    auto cpu_result = engine->initialize(crossscale::DeviceType::CPU);
                    if (!cpu_result.has_value()) {
                        LOG_ERROR("Worker", "Failed to initialize engine");
                        if (!g_running) {
                            break;
                        }
                        reconnect_attempt += 1;
                        const int retry_delay_seconds = std::min(12, 1 << std::min(reconnect_attempt, 3));
                        LOG_WARNING("Worker", "Reconnect in " + std::to_string(retry_delay_seconds) + "s");
                        std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
                        continue;
                    }
                }
                LOG_INFO(
                    "Worker",
                    "Engine initialized on " +
                        std::string(engine->current_device() == crossscale::DeviceType::GPU_VULKAN ? "GPU" : "CPU")
                );

                auto handshake_result = worker->handshake(*engine);
                if (!handshake_result.has_value()) {
                    LOG_ERROR("Worker", "Handshake failed: " + handshake_result.error().message);
                    worker->disconnect();
                    if (!g_running) {
                        break;
                    }
                    reconnect_attempt += 1;
                    const int retry_delay_seconds = std::min(12, 1 << std::min(reconnect_attempt, 3));
                    LOG_WARNING("Worker", "Reconnect in " + std::to_string(retry_delay_seconds) + "s");
                    std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
                    continue;
                }

                std::filesystem::create_directories("./models");
                worker->set_models_directory("./models");
                auto sync_result = worker->sync_models();
                if (!sync_result.has_value()) {
                    LOG_WARNING("Worker", "Model sync skipped: " + sync_result.error().message);
                }

                worker->start_heartbeat(std::chrono::seconds(5));
                reconnect_attempt = 0;
                LOG_INFO("Worker", "Handshake complete. Waiting for tasks...");

                while (g_running && worker->is_connected()) {
                    auto task_opt = worker->receive_task();
                    if (!task_opt.has_value()) {
                        LOG_WARNING("Worker", "Task receive failed or disconnected");
                        break;
                    }
                    const auto& task = task_opt.value();
                    LOG_INFO("Worker", "Received task: " + std::string(task.task_id) + " model=" + task.model_name);

                    auto result_opt = worker->execute_task(task, *engine, [](float) {});
                    if (!result_opt.has_value()) {
                        LOG_ERROR("Worker", "Task execution failed: " + result_opt.error().message);
                        continue;
                    }

                    auto submit_opt = worker->submit_results(result_opt.value());
                    if (!submit_opt.has_value()) {
                        LOG_ERROR("Worker", "Submit results failed: " + submit_opt.error().message);
                    } else {
                        LOG_INFO("Worker", "Task completed and results submitted");
                    }
                }

                worker->disconnect();
                if (!g_running) {
                    break;
                }
                reconnect_attempt += 1;
                const int retry_delay_seconds = std::min(12, 1 << std::min(reconnect_attempt, 3));
                LOG_WARNING("Worker", "Disconnected from coordinator, reconnect in " + std::to_string(retry_delay_seconds) + "s");
                std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
            }

            LOG_INFO("Worker", "Worker stopped");
            return 0;
        }
        
        LOG_ERROR("CLI", "Unknown worker subcommand: " + subcommand);
        return 1;
    }

    if (command == "client") {
        std::string subcommand = argc > 2 ? argv[2] : "";
        
        if (subcommand == "process") {
            std::string input_path, output_path, server_addr, auth_key;
            int tile_size = 256, tile_overlap = 16;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-i" && i + 1 < argc) input_path = argv[++i];
                else if (arg == "-o" && i + 1 < argc) output_path = argv[++i];
                else if (arg == "-s" && i + 1 < argc) server_addr = argv[++i];
                else if (arg == "-k" && i + 1 < argc) auth_key = argv[++i];
                else if (arg == "--tile-size" && i + 1 < argc) { tile_size = safe_parse_int<int>(argv[++i], "--tile-size", 16, 8192); }
                else if (arg == "--tile-overlap" && i + 1 < argc) tile_overlap = safe_parse_int<int>(argv[++i], "--tile-overlap", 0, 1024);
            }
            
            if (input_path.empty() || output_path.empty() || server_addr.empty() || auth_key.empty()) {
                LOG_ERROR("Client", "Missing required arguments: -i <input> -o <output> -s <host:port> -k <key>");
                return 1;
            }
            
            size_t colon_pos = server_addr.find(':');
            if (colon_pos == std::string::npos) {
                LOG_ERROR("Client", "Invalid server address format. Use <host>:<port>");
                return 1;
            }
            std::string host = server_addr.substr(0, colon_pos);
            uint16_t port = safe_parse_int<uint16_t>(server_addr.substr(colon_pos + 1), "server port", 1, 65535);
            
            LOG_INFO("Client", "Loading image: " + input_path);
            auto image_result = crossscale::Image::load(input_path);
            if (!image_result.has_value()) {
                LOG_ERROR("Client", "Failed to load image: " + image_result.error().message);
                return 1;
            }
            auto& image = image_result.value();
            
            LOG_INFO("Client", "Connecting to coordinator at " + host + ":" + std::to_string(port));
            
            crossscale::TCPSocket socket;
            auto conn_result = socket.connect(host, port);
            if (!conn_result.has_value()) {
                LOG_ERROR("Client", "Failed to connect: " + conn_result.error().message);
                return 1;
            }
            
            // Authenticate
            crossscale::AuthRequest auth_req{};
            std::copy_n(auth_key.c_str(), std::min(auth_key.size(), sizeof(auth_req.auth_key)-1), auth_req.auth_key);
            auth_req.auth_key[sizeof(auth_req.auth_key)-1] = '\0';
            
            crossscale::MessageHeader auth_header{};
            auth_header.type = crossscale::MessageType::MSG_AUTH_REQUEST;
            auth_header.payload_length = sizeof(auth_req);
            
            auto auth_data = crossscale::Protocol::serialize(auth_header, &auth_req);
            // fire-and-forget: the receive_all below detects any send-side failure.
            (void)socket.send_all(auth_data.data(), auth_data.size());
            
            std::vector<uint8_t> resp_header_buf(crossscale::Protocol::HEADER_SIZE);
            auto auth_header_recv = socket.receive_all(resp_header_buf.data(), resp_header_buf.size());
            if (!auth_header_recv.has_value()) {
                LOG_ERROR("Client", "Failed to receive auth response header: " + auth_header_recv.error().message);
                return 1;
            }
            auto resp_header = crossscale::Protocol::parse_header(resp_header_buf.data(), resp_header_buf.size());
            
            if (!resp_header || resp_header->type != crossscale::MessageType::MSG_AUTH_RESPONSE) {
                LOG_ERROR("Client", "Invalid auth response");
                return 1;
            }
            
            std::vector<uint8_t> resp_payload(resp_header->payload_length);
            if (resp_header->payload_length > MAX_NETWORK_PAYLOAD) {
                LOG_ERROR("Client", "Auth response payload too large: " + std::to_string(resp_header->payload_length));
                return 1;
            }
            auto auth_payload_recv = socket.receive_all(resp_payload.data(), resp_payload.size());
            if (!auth_payload_recv.has_value()) {
                LOG_ERROR("Client", "Failed to receive auth response payload: " + auth_payload_recv.error().message);
                return 1;
            }
            crossscale::AuthResponse* auth_resp = reinterpret_cast<crossscale::AuthResponse*>(resp_payload.data());
            
            if (!auth_resp->authenticated) {
                LOG_ERROR("Client", "Authentication failed");
                return 1;
            }
            
            LOG_INFO("Client", "Authenticated, submitting task...");
            
            // Submit task
            crossscale::ClientTaskSubmit task_submit{};
            task_submit.image_width = static_cast<uint32_t>(image.width());
            task_submit.image_height = static_cast<uint32_t>(image.height());
            task_submit.channels = static_cast<uint8_t>(image.channels());
            constexpr std::string_view default_model = "Real-ESRGANv3-anime-x2";
            std::copy_n(default_model.begin(), std::min(default_model.size(), sizeof(task_submit.model_name)-1), task_submit.model_name);
            task_submit.model_name[sizeof(task_submit.model_name)-1] = '\0';
            task_submit.tile_size = static_cast<uint32_t>(tile_size);
            task_submit.tile_overlap = static_cast<uint32_t>(tile_overlap);
            task_submit.image_data_length = static_cast<uint32_t>(image.size());
            
            crossscale::MessageHeader submit_header{};
            submit_header.type = crossscale::MessageType::MSG_CLIENT_SUBMIT;
            submit_header.payload_length = sizeof(task_submit);
            
            auto submit_header_data = crossscale::Protocol::serialize(submit_header, &task_submit);
            // fire-and-forget: the receive_all below detects any send-side failure.
            (void)socket.send_all(submit_header_data.data(), submit_header_data.size());
            (void)socket.send_all(image.data(), image.size());
            
            // Receive result
            std::vector<uint8_t> result_header_buf(crossscale::Protocol::HEADER_SIZE);
            auto result_header_recv = socket.receive_all(result_header_buf.data(), result_header_buf.size());
            if (!result_header_recv.has_value()) {
                LOG_ERROR("Client", "Failed to receive task response header: " + result_header_recv.error().message);
                return 1;
            }
            auto result_header = crossscale::Protocol::parse_header(result_header_buf.data(), result_header_buf.size());
            
            if (!result_header || result_header->type != crossscale::MessageType::MSG_CLIENT_RESPONSE) {
                LOG_ERROR("Client", "Invalid task response header");
                return 1;
            }
            
            std::vector<uint8_t> result_resp_payload(result_header->payload_length);
            if (result_header->payload_length > MAX_NETWORK_PAYLOAD) {
                LOG_ERROR("Client", "Task response payload too large: " + std::to_string(result_header->payload_length));
                return 1;
            }
            auto result_payload_recv = socket.receive_all(result_resp_payload.data(), result_resp_payload.size());
            if (!result_payload_recv.has_value()) {
                LOG_ERROR("Client", "Failed to receive task response payload: " + result_payload_recv.error().message);
                return 1;
            }
            crossscale::ClientTaskResponse* task_resp = reinterpret_cast<crossscale::ClientTaskResponse*>(result_resp_payload.data());
            
            if (!task_resp->success) {
                LOG_ERROR("Client", "Task failed: " + std::string(task_resp->error_message));
                return 1;
            }
            
            LOG_INFO("Client", "Task completed, receiving result (" + std::to_string(task_resp->result_data_length) + " bytes)...");
            
            if (task_resp->result_data_length > MAX_NETWORK_PAYLOAD) {
                LOG_ERROR("Client", "Result data too large: " + std::to_string(task_resp->result_data_length));
                return 1;
            }
            std::vector<uint8_t> result_data(task_resp->result_data_length);
            auto result_data_recv = socket.receive_all(result_data.data(), result_data.size());
            if (!result_data_recv.has_value()) {
                LOG_ERROR("Client", "Failed to receive task result bytes: " + result_data_recv.error().message);
                return 1;
            }
            
            std::ofstream out_file(output_path, std::ios::binary);
            if (!out_file.is_open()) {
                LOG_ERROR("Client", "Failed to open output file: " + output_path);
                return 1;
            }
            out_file.write(reinterpret_cast<const char*>(result_data.data()), result_data.size());
            out_file.close();
            
            if (!out_file) {
                LOG_ERROR("Client", "Failed to write output file: " + output_path);
                return 1;
            }
            
            LOG_INFO("Client", "Result saved to: " + output_path);
            return 0;
        }
        
        LOG_ERROR("CLI", "Unknown client subcommand: " + subcommand);
        return 1;
    }

    if (command == "serve") {
        std::string subcommand = argc > 2 ? argv[2] : "start";
        std::string model_name;
        std::string models_dir = "./models";
        std::string socket_path = crossscale::default_serve_socket_path();
        bool force_cpu = false;
        bool foreground = false;

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--model" && i + 1 < argc) model_name = argv[++i];
            else if (arg == "--models-dir" && i + 1 < argc) models_dir = argv[++i];
            else if (arg == "--socket" && i + 1 < argc) socket_path = argv[++i];
            else if (arg == "--cpu") force_cpu = true;
            else if (arg == "--fg") foreground = true;
            else if (arg == "--help" || arg == "-h") {
                std::cout << "CrossScale serve - local warm-engine daemon\n\n"
                          << "Usage:\n"
                          << "  crossscale serve start [--model <name>] [--socket <path>] [--cpu] [--fg]\n"
                          << "  crossscale serve stop  [--socket <path>]\n\n"
                          << "Holds a warm ncnn engine so that `crossscale process` single-image calls\n"
                          << "reuse it and skip the ~490ms init+load fixed cost (~4x faster).\n\n"
                          << "Options:\n"
                          << "  --model <name>   Initial model (default: first available)\n"
                          << "  --socket <path>  Unix socket path (default: " << crossscale::default_serve_socket_path() << ")\n"
                          << "  --cpu            Force CPU (default: GPU with CPU fallback)\n"
                          << "  --fg             Run in foreground (default: background)\n\n"
                          << "Status: `pgrep -af 'crossscale serve'`\n";
                return 0;
            }
        }

        if (socket_path.empty()) {
            LOG_ERROR("Serve", "no default socket path available (set XDG_CACHE_HOME or HOME)");
            return 1;
        }

        std::string pid_path = socket_path + ".pid";

        if (subcommand == "stop") {
            // Read pid, send SIGTERM. The daemon's run() loop observes the
            // resulting g_running=false and exits; its destructor unlinks the
            // socket, and the start path unlinks the pid file.
            std::ifstream pf(pid_path);
            if (!pf) {
                LOG_ERROR("Serve", "no pid file at " + pid_path + " (daemon not running?)");
                return 1;
            }
            pid_t pid = 0;
            pf >> pid;
            if (pid <= 0) {
                LOG_ERROR("Serve", "corrupt pid file at " + pid_path);
                return 1;
            }
            if (::kill(pid, SIGTERM) < 0) {
                LOG_ERROR("Serve", std::string("kill pid ") + std::to_string(pid) +
                                   " failed: " + std::strerror(errno));
                // Stale pid file: clean it up so next start isn't confused.
                if (errno == ESRCH) std::filesystem::remove(pid_path);
                return 1;
            }
            LOG_INFO("Serve", "stopped pid " + std::to_string(pid));
            return 0;
        }

        if (subcommand != "start") {
            LOG_ERROR("Serve", "unknown subcommand: " + subcommand + " (try start|stop)");
            return 1;
        }

        // ── start ───────────────────────────────────────────────────────────
        // Guard: if a pid file exists and the pid is alive, refuse (already running).
        {
            std::ifstream pf(pid_path);
            if (pf) {
                pid_t existing = 0;
                pf >> existing;
                if (existing > 0 && ::kill(existing, 0) == 0) {
                    LOG_ERROR("Serve", "daemon already running, pid " + std::to_string(existing));
                    return 1;
                }
                // pid file present but process dead → stale, overwrite below.
            }
        }

        // Background via single fork + setsid (detaches controlling terminal).
        if (!foreground) {
            pid_t pid = ::fork();
            if (pid < 0) { LOG_ERROR("Serve", "fork failed"); return 1; }
            if (pid > 0) return 0;  // parent exits
            ::setsid();
            ::close(STDIN_FILENO);
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
        }

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        crossscale::ServeDaemon daemon;
        int rc = daemon.start(socket_path, models_dir, model_name,
                              force_cpu ? crossscale::ServeDevice::CPU
                                        : crossscale::ServeDevice::GPU);
        if (rc != 0) {
            std::filesystem::remove(pid_path);
            return rc;
        }

        // Write pid file after successful start (so stop can find us).
        std::ofstream(pid_path) << ::getpid();

        daemon.run();  // returns when stop_requested_ is set (signal)

        // Cleanup on shutdown: daemon destructor unlinks the socket; we unlink pid.
        std::filesystem::remove(pid_path);
        LOG_INFO("Serve", "daemon shutting down");
        return 0;
    }

    LOG_ERROR("CLI", "Unknown command: " + command);
    print_usage();
    return 1;
}
