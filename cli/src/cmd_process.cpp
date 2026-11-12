// `upix <image>` — the main upscale path (the old explicit `process`
// subcommand is gone; the same options live on the root verb).
//
// Single file: try to delegate to a warm daemon first (skips the ~490ms
// init+load), spawning one if none is running, else init an inline engine and
// process. Directory: batch over all images, with optional GPU submit-queue
// pipeline or CPU EnginePool.
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
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/image.hpp"
#include "utils/exif.hpp"
#include "utils/logger.hpp"
#include "utils/sr_marker.hpp"
#include "core/config.hpp"
#include "core/serve/daemon.hpp"  // default_serve_socket_path
#include "core/serve/daemon_control.hpp"
#include "core/engine.hpp"
#include "core/engine_setup.hpp"
#include "core/model_manager.hpp"
#include "core/tile_optimizer.hpp"
#include "core/tile_processor.hpp"
#include "process_batch.hpp"  // run_batch (directory path, Layer 2 Task 4)

namespace fs = std::filesystem;

namespace {

// Model used to tag a derived default output when the user named none: the
// first model the manager finds in models_dir — the same "first available"
// rule setup_engine and run_batch use.
auto first_model_name(const std::string& models_dir) -> std::string {
    upix::ModelManager manager(models_dir);
    auto scan = manager.scan_models();
    if (scan && !scan->empty() && !manager.available_models().empty()) {
        return manager.available_models().front().name;
    }
    return "model";
}

void log_missing_model_hint() {
    LOG_ERROR("Engine",
              "Install a model with `upix model download <name>` "
              "(see `upix model list-remote` or `upix model list`)");
}

}  // namespace

int cmd_process(const args::Process& a) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Machine event mode (UPIX_PROGRESS=json): must be on before the first
    // LOG_* so stdout stays a clean event stream. `=1` keeps the human path.
    if (const char* progress_env = std::getenv("UPIX_PROGRESS");
        progress_env && std::string_view(progress_env) == "json") {
        enable_machine_output();
    }

    if (a.input_path.empty()) {
        std::println(stderr, "upix: no input given — try `upix <image>` "
                             "or `upix -i <image>`");
        return 1;
    }

    const std::string& input_path = a.input_path;
    const std::string& output_path = a.output_path;
    const std::string& model_name = a.model_name;
    const std::string& models_dir = a.models_dir;
    auto effective = resolve_processing_config(a);
    auto& config = effective.config;
    // Reject invalid fixed settings before delegating: the daemon must receive
    // the same valid settings that inline processing would use.
    if (!effective.recommend_tile_size && !config.auto_tile_size) {
        const auto errors = config.validate();
        if (!errors.empty()) {
            for (const auto& err : errors) LOG_ERROR("Config", "{}", err);
            return 1;
        }
    }

    // ── Device selection ────────────────────────────────────────────────────
    // Explicit CLI wins over the config file: --cpu (mutually exclusive with
    // --gpu) forces a CPU-only engine, --gpu prefers GPU, and a config file's
    // `use_gpu: false` is honored when neither flag was given — it used to be
    // parsed and then silently ignored on the single-image path. CPU-only also
    // skips daemon delegation below: the daemon is a warm GPU engine and
    // ServeRequest carries no device field, so delegating would run the work
    // on the GPU anyway.
    const bool force_cpu = a.use_cpu || (!a.use_gpu && !config.use_gpu);

    // Check if input is a directory for batch processing
    bool is_dir = fs::is_directory(input_path);

    // ── Daemon delegation (single-file only) ────────────────────────────────
    // Try the warm daemon BEFORE reading input_bytes into CLI memory: the
    // daemon reads the file, checks the SR marker (`force`), runs inference,
    // and writes the output itself, keeping the CLI's memory near zero.
    const bool daemon_supports_config =
        !config.auto_tile_size && config.batch_size == upix::kDefaultBatchSize &&
        (!effective.recommend_tile_size ||
         (config.min_tile_size == upix::kDefaultMinTileSize &&
          config.max_tile_size == upix::kDefaultMaxTileSize));
    if (!is_dir && !a.no_daemon && !force_cpu && daemon_supports_config) {
        std::error_code ex_ec;
        if (fs::exists(input_path, ex_ec)) {
            const std::string out_fmt = !config.output_format.empty()
                ? config.output_format
                : (!output_path.empty() ? infer_format_from_extension(output_path)
                                        : std::string());

            upix::ServeRequestArgs req_args;
            req_args.input_path = input_path;
            req_args.output_path = output_path;
            req_args.model_name = model_name;
            req_args.output_format = out_fmt;
            req_args.quality = config.output_quality;
            req_args.png_level = config.encoder.png_level;
            req_args.webp_method = config.encoder.webp_method;
            req_args.avif_speed = config.encoder.avif_speed;
            req_args.chroma = config.encoder.jpeg_subsampling;
            req_args.avif_subsampling = config.encoder.avif_subsampling;
            req_args.avif_threads = config.encoder.avif_threads;
            req_args.webp_lossless = config.encoder.webp_lossless;
            req_args.use_gpu = !force_cpu;
            req_args.force = a.force;
            req_args.tile_size = effective.recommend_tile_size
                ? 0u
                : static_cast<uint32_t>(config.tile_size);
            req_args.tile_overlap = config.tile_overlap;

            const auto try_delegate = [&]() -> std::optional<int> {
                const auto delegate_start = std::chrono::steady_clock::now();
                auto progress_cb = [&](uint32_t completed, uint32_t total,
                                       uint32_t stage) {
                    if (stage == 0) {
                        upix::ProcessingProgress prog;
                        prog.total_tiles = static_cast<int>(total);
                        prog.completed_tiles = static_cast<int>(completed);
                        report_progress(prog, delegate_start);
                    } else if (!machine_output_enabled()) {
                        std::print(stderr, "\n");
                        std::fflush(stderr);
                        LOG_INFO("Engine", "Inference done, encoding output...");
                    }
                };
                auto outcome = upix::serve_request(
                    req_args, {}, &g_cancel_requested, progress_cb);
                if (outcome.transport_fallback) {
                    return std::nullopt;
                }
                if (outcome.cancelled) {
                    LOG_WARNING("Engine", "Processing cancelled");
                    return 1;
                }
                if (!outcome.ok) {
                    LOG_ERROR("Engine", "daemon error: {}", outcome.message);
                    if (outcome.message.find("model") != std::string::npos) {
                        log_missing_model_hint();
                    }
                    return 1;
                }
                if (outcome.skipped) {
                    LOG_INFO("Engine",
                             "Skipped (already super-resolved by this tool): {}; "
                             "re-run with --force to process anyway",
                             input_path);
                    return 0;
                }
                if (machine_output_enabled()) {
                    emit_json_line(std::format(
                        R"({{"t":"saved","path":"{}"}})",
                        json_escape(outcome.out_path)));
                    emit_json_line(std::format(
                        R"({{"t":"note","message":"{}x{}"}})",
                        outcome.width, outcome.height));
                }
                LOG_INFO("Engine", "Output saved (via warm daemon): {}",
                         outcome.out_path);
                LOG_INFO("Engine", "Processing complete: {}x{}",
                         outcome.width, outcome.height);
                return 0;
            };

            if (upix::daemon_endpoint_present()) {
                LOG_INFO("Engine", "delegating to warm engine daemon...");
                if (auto rc = try_delegate()) {
                    return *rc;
                }
            }
            std::string spawn_err;
            LOG_INFO("Engine", "starting warm engine daemon...");
            if (upix::ensure_serve_daemon_running(
                    models_dir, args::Daemon{}.idle_timeout_s, spawn_err, {},
                    model_name, false)) {
                LOG_INFO("Engine", "delegating to warm engine daemon...");
                if (auto rc = try_delegate()) {
                    return *rc;
                }
            }
        }
    }

    if (is_dir) {
        // No explicit output → derive a sibling `dir_<model>/`. Copy the args
        // so the const contract holds; the tag model mirrors what run_batch
        // will load (-m when given, else first available).
        args::Process batch = a;
        if (batch.output_path.empty()) {
            const std::string model = model_name.empty()
                ? first_model_name(models_dir)
                : model_name;
            batch.output_path = derive_default_output(input_path, model, true);
            LOG_INFO("Engine", "Output directory: {}", batch.output_path);
        }
        return run_batch(batch, std::move(effective));
    }

    // ── Inline single-file fallback: read bytes + SR-marker check ───────────
    std::vector<uint8_t> input_bytes;
    {
        auto raw = upix::Image::load_raw(input_path);
        if (!raw.has_value()) {
            LOG_ERROR("Engine", "Failed to load image {}: {}",
                      input_path, raw.error().message);
            return 1;
        }
        input_bytes = std::move(raw.value());
        if (!a.force) {
            if (auto marker = upix::sr_marker::probe(input_bytes.data(),
                                                     input_bytes.size())) {
                LOG_INFO("Engine",
                         "Skipped (already super-resolved by this tool): {} "
                         "[model={} scale={}x ts={}]; re-run with --force to process anyway",
                         input_path, marker->model, marker->scale, marker->ts);
                return 0;
            }
        }
    }

    // Single file processing continues...
    LOG_INFO("Engine", "Loading image: {}", input_path);

    // Bytes were already read for the marker check above; decode from them
    // (same extension-based dispatch as Image::load).
    auto image_result = upix::Image::load_from_memory(
        fs::path(input_path).extension().string(), input_bytes);
    if (!image_result.has_value()) {
        LOG_ERROR("Engine", "Failed to load image: {}", image_result.error().message);
        return 1;
    }

    auto& image = image_result.value();

    // Portrait JPEGs: bake the EXIF orientation transform into the decoded
    // pixels so the upscale runs on the upright frame (mirrors the remote
    // path and the batch path). Non-JPEG / no-EXIF inputs are untouched.
    const int exif_orientation = upix::exif::parse_jpeg_exif_orientation(
        input_bytes.data(), input_bytes.size());
    if (exif_orientation != 1 &&
        upix::exif::autorotate_image(image, exif_orientation)) {
        LOG_INFO("Engine", "applied EXIF orientation {}", exif_orientation);
    }

    LOG_INFO("Engine", "Image loaded: {}x{} ({} channels)",
             image.width(), image.height(), image.channels());

    // Engine init + model load via the shared recipe. ENGINE_DEFAULT mirrors the
    // original `engine->initialize()` (no arg): try GPU, fall back to CPU.
    // --cpu / config use_gpu=false select the CPU-only strategy; --gpu makes
    // the GPU-first strategy explicit (identical to the default, but it must
    // not be silently ignored).
    auto setup = upix::setup_engine(
        force_cpu ? upix::SetupDevice::CPU
                  : (a.use_gpu ? upix::SetupDevice::GPU_THEN_CPU
                               : upix::SetupDevice::ENGINE_DEFAULT),
        models_dir,
        model_name
    );
    if (!setup) {
        LOG_ERROR("Engine", "{}", setup.error());
        log_missing_model_hint();
        return 1;
    }
    const auto* selected_model = setup->selected;  // capture pointer before moves
    auto engine = std::move(setup->engine);
    // `manager` owns the scanned model vector that `selected_model` points into;
    // keep it alive for the whole function (used again at line ~289 below).
    upix::ModelManager manager = std::move(setup->models);
    if (model_name.empty()) {
        LOG_INFO("Engine", "No model specified, using first available: {}", selected_model->name);
    }
    std::string model_name_resolved = selected_model->name;
    int model_input_channels = std::max(1, selected_model->input_channels);
    LOG_INFO("Engine", "Using model: {} ({}x)", model_name_resolved,
             selected_model->scale_factor);
    // Fall back to the filename-parsed scale only when the engine could not
    // detect it from the param graph — a detected value describes what the
    // net actually outputs; the filename is just a convention.
    if (!engine->scale_factor_detected()) {
        engine->set_scale_factor(selected_model->scale_factor);
    }

    // No explicit output → derive `img_<model>.png` next to the input.
    const std::string eff_output =
        output_path.empty()
            ? derive_default_output(input_path, model_name_resolved, false)
            : output_path;

    LOG_INFO("Engine", "Model loaded successfully");

    int scale = engine->get_scale_factor();
    LOG_INFO("Engine", "Scale factor: {}x", scale);

    if (effective.recommend_tile_size) {
        config.tile_size = std::clamp(engine->get_recommended_tile_size(),
                                      config.min_tile_size, config.max_tile_size);
        LOG_INFO("Engine", "Auto-selected tile size: {}", config.tile_size);
    }

    int input_channels_before = image.channels();
    if (!normalize_channels_for_model(image, model_input_channels, input_path)) {
        return 1;
    }
    if (image.channels() != input_channels_before) {
        LOG_INFO("Engine", "Adjusted input channels: {} -> {}",
                 input_channels_before, image.channels());
    }

    if (config.output_format.empty()) {
        // Auto-infer output format from output file extension
        std::string inferred = infer_format_from_extension(eff_output);
        if (!inferred.empty()) {
            config.output_format = upix::normalize_output_format(inferred);
            LOG_INFO("Config", "Auto-detected output format: {}", inferred);
        }
    }

    // 如果启用自动调优，运行基准测试选择最优 tile size
    if (config.auto_tile_size) {
        LOG_INFO("Config", "Auto-tile enabled, benchmarking...");

        upix::TileSizeOptimizer optimizer;
        upix::ModelConstraints model_constraints;
        model_constraints.model_name = model_name_resolved;
        model_constraints.model_size_mb = selected_model->total_size / (1024 * 1024);
        model_constraints.min_tile_size = config.min_tile_size;
        model_constraints.max_tile_size = config.max_tile_size;

        config.tile_size = optimizer.find_optimal_tile_size(
            *engine, model_constraints, config,
            image.width(), image.height()
        );
        config.auto_tile_size = true;

        LOG_INFO("Config", "Optimal tile size determined: {}", config.tile_size);
    }

    // 验证配置
    auto errors = config.validate();
    if (!errors.empty()) {
        LOG_ERROR("Config", "Configuration errors:");
        for (const auto& err : errors) {
            LOG_ERROR("Config", "  - {}", err);
        }
        return 1;
    }

    // 使用 TileProcessor 进行瓦片处理
    upix::TileProcessor processor;
    upix::TileConfig tile_config;
    tile_config.tile_size = config.tile_size;
    tile_config.overlap = config.tile_overlap;
    tile_config.batch_size = config.batch_size;

    // 64-bit math, then range-check BEFORE the narrowing cast (same rule as the
    // batch path): an overflowed int would size the output Image from a bogus
    // value instead of failing loudly.
    const int64_t out_w64 = static_cast<int64_t>(image.width()) * scale;
    const int64_t out_h64 = static_cast<int64_t>(image.height()) * scale;
    const uint64_t bytes_per_pixel =
        static_cast<uint64_t>(std::max(1, image.channels()));
    const bool size_overflow =
        out_w64 > 0 && out_h64 > 0 &&
        static_cast<uint64_t>(out_w64) >
            std::numeric_limits<size_t>::max() /
                (static_cast<uint64_t>(out_h64) * bytes_per_pixel);
    if (out_w64 <= 0 || out_h64 <= 0 ||
        out_w64 > std::numeric_limits<int>::max() ||
        out_h64 > std::numeric_limits<int>::max() || size_overflow) {
        LOG_ERROR("Engine", "Upscaled output out of range: {}x{} at {}x",
                  image.width(), image.height(), scale);
        return 1;
    }
    const int out_width = static_cast<int>(out_w64);
    const int out_height = static_cast<int>(out_h64);
    upix::Image output_image(out_width, out_height, image.channels());

    LOG_INFO("Engine", "Processing image with tile_size={} overlap={} batch_size={}...",
             config.tile_size, config.tile_overlap, config.batch_size);

    bool enable_progress_log_single = false;
    if (const char* progress_env = std::getenv("UPIX_PROGRESS")) {
        enable_progress_log_single = std::string(progress_env) == "1";
    }

    auto process_start_time = std::chrono::steady_clock::now();
    auto process_result = processor.process_image_u8(
        image.data(), output_image.data(),
        image.width(), image.height(), image.channels(),
        *engine, tile_config,
        [&](const upix::ProcessingProgress& progress) {
            // Human: always print progress to stderr with ETA.
            // Machine: one JSON event per update on stdout.
            report_progress(progress, process_start_time);
            if (enable_progress_log_single && progress.total_tiles > 0) {
                float pct = progress.percentage();
                if (progress.completed_tiles % 10 == 0 ||
                    progress.completed_tiles == progress.total_tiles) {
                    LOG_INFO("Engine", "Progress: {}/{} tiles ({:.0f}%)",
                             progress.completed_tiles, progress.total_tiles, pct);
                }
            }
        },
        g_cancel_requested
    );

    if (!process_result.has_value()) {
        LOG_ERROR("Engine", "Processing failed: {}", process_result.error().message);
        return 1;
    }

    const auto final_output_path = upix::resolve_output_path(eff_output, config);
    // Match the daemon path: an explicit -o into a not-yet-existing directory
    // creates it instead of failing at open() time.
    if (!final_output_path.parent_path().empty()) {
        std::error_code mk_ec;
        std::filesystem::create_directories(final_output_path.parent_path(), mk_ec);
    }
    LOG_INFO("Engine", "Inference done, encoding and saving output...");
    const auto sr_xmp = upix::sr_marker::build_xmp(
        model_name_resolved, scale, static_cast<int64_t>(std::time(nullptr)));
    auto save_result = output_image.save(final_output_path, config.output_quality, sr_xmp, config.encoder);
    if (!save_result.has_value()) {
        LOG_ERROR("Engine", "Failed to save output {}: {}",
                  final_output_path.string(), save_result.error().message);
        return 1;
    }

    if (machine_output_enabled()) {
        emit_json_line(std::format(
            R"({{"t":"saved","path":"{}"}})",
            json_escape(final_output_path.string())));
        emit_json_line(std::format(
            R"({{"t":"note","message":"{}x{}"}})", out_width, out_height));
    }
    LOG_INFO("Engine", "Output saved: {}", final_output_path.string());
    LOG_INFO("Engine", "Processing complete: {}x{}", out_width, out_height);
    return 0;
}
