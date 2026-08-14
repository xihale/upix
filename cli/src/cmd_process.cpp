// `crossscale process` — the main upscale path.
//
// Single file: try to delegate to a warm serve daemon first (skips the ~490ms
// init+load), else init an inline engine and process. Directory: batch over
// all images, with optional GPU submit-queue pipeline or CPU EnginePool.
//
// Migrated verbatim from the legacy monolithic main.cpp; only the argv parsing
// (now done by CLI11 in cli_app.cpp) was removed. Business logic is unchanged.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/image.hpp"
#include "utils/logger.hpp"
#include "utils/unix_socket.hpp"
#include "core/config.hpp"
#include "core/serve/daemon.hpp"  // default_serve_socket_path, ServeRequest/Response
#include "core/engine.hpp"
#include "core/engine_pool.hpp"
#include "core/engine_setup.hpp"
#include "core/model_manager.hpp"
#include "core/distributed/protocol.hpp"
#include "core/tile_optimizer.hpp"
#include "core/tile_processor.hpp"
#include "process_batch.hpp"  // run_batch (directory path, Layer 2 Task 4)

namespace fs = std::filesystem;

int cmd_process(const args::Process& a) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const std::string& input_path = a.input_path;
    const std::string& output_path = a.output_path;
    const std::string& model_name = a.model_name;
    const std::string& models_dir = a.models_dir;
    const std::string& config_file = a.config_file;
    int tile_size = a.tile_size;
    int tile_overlap = a.tile_overlap;
    bool auto_tile = a.auto_tile;
    bool tile_size_explicit = a.tile_size_explicit;  // set by cli_app.cpp
    int min_tile = a.min_tile;
    int max_tile = a.max_tile;
    int batch_size = a.batch_size;
    bool no_daemon = a.no_daemon;
    int cli_output_quality = a.output_quality;
    const std::string& cli_output_format = a.output_format;

    // Check if input is a directory for batch processing
    bool is_dir = fs::is_directory(input_path);
    LOG_INFO("Debug", "Input path: " + input_path + " is_dir: " + std::to_string(is_dir));

    // ── Serve-daemon delegation (single-file only) ──────────────────────────
    // If a local serve daemon is running, send it the image and let the warm
    // engine do inference — skipping the ~490 ms init+load fixed cost. Falls
    // back silently (returns nullopt) if no daemon or the connection fails;
    // on a real daemon-side error returns 1.
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
        return run_batch(a);
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

    // Engine init + model load via the shared recipe. ENGINE_DEFAULT mirrors the
    // original `engine->initialize()` (no arg): try GPU, fall back to CPU.
    auto setup = crossscale::setup_engine(
        crossscale::SetupDevice::ENGINE_DEFAULT,
        models_dir,
        model_name
    );
    if (!setup) {
        LOG_ERROR("Engine", setup.error());
        return 1;
    }
    const auto* selected_model = setup->selected;  // capture pointer before moves
    auto engine = std::move(setup->engine);
    // `manager` owns the scanned model vector that `selected_model` points into;
    // keep it alive for the whole function (used again at line ~289 below).
    crossscale::ModelManager manager = std::move(setup->models);
    if (model_name.empty()) {
        LOG_INFO("Engine", "No model specified, using first available: " + selected_model->name);
    }
    std::string model_name_resolved = selected_model->name;
    int model_input_channels = std::max(1, selected_model->input_channels);
    LOG_INFO("Engine", "Using model: " + model_name_resolved + " (" + std::to_string(selected_model->scale_factor) + "x)");
    // Override engine's heuristic scale detection with the authoritative value
    // parsed from the model filename.
    engine->set_scale_factor(selected_model->scale_factor);

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
        model_constraints.model_name = model_name_resolved;
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
