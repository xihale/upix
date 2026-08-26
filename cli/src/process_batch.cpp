// Directory batch path, split out of cmd_process (Layer 2 Task 4).
//
// Verbatim move of the former `if (is_dir) { ... }` block into run_batch().
// The four helper lambdas (prepare/infer/save/process_one) stay intact
// inside run_batch; their [&] captures resolve to run_batch's locals.
// No logic change — A/B md5 verified bit-identical.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/image.hpp"
#include "utils/image_avif.hpp"
#include "utils/exif.hpp"
#include "utils/logger.hpp"
#include "utils/sr_marker.hpp"
#include "utils/unix_socket.hpp"
#include "core/config.hpp"
#include "core/serve/daemon.hpp"  // default_serve_socket_path, ServeRequest/Response
#include "core/engine.hpp"
#include "core/engine_pool.hpp"
#include "core/model_manager.hpp"
#include "core/distributed/protocol.hpp"
#include "core/tile_optimizer.hpp"
#include "core/tile_processor.hpp"

namespace fs = std::filesystem;

namespace {

// Header-only dimension sniff (PNG IHDR / JPEG SOF) for the VRAM admission
// control in the GPU directory path. Returns {0,0} when the format is
// unknown — callers treat that as "no estimate" and rely on the runtime OOM
// ladder instead.
std::pair<uint32_t, uint32_t> sniff_image_dimensions(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {0, 0};
    std::array<unsigned char, 24> sig{};
    f.read(reinterpret_cast<char*>(sig.data()), static_cast<std::streamsize>(sig.size()));
    const auto got = static_cast<size_t>(f.gcount());
    const auto sig_bytes = std::span(sig).first(got);

    static constexpr std::array<unsigned char, 8> kPngSig = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (got >= 24 && std::ranges::equal(sig_bytes.first(8), kPngSig)) {
        const auto be32 = [&](int o) {
            return (static_cast<uint32_t>(sig[o]) << 24) |
                   (static_cast<uint32_t>(sig[o + 1]) << 16) |
                   (static_cast<uint32_t>(sig[o + 2]) << 8) | sig[o + 3];
        };
        return {be32(16), be32(20)};
    }

    if (got >= 2 && sig[0] == 0xFF && sig[1] == 0xD8) {
        f.seekg(2);
        while (f) {
            unsigned char m[4];
            f.read(reinterpret_cast<char*>(m), 4);  // FF marker len_hi len_lo
            if (f.gcount() < 4 || m[0] != 0xFF) break;
            const bool is_sof = m[1] >= 0xC0 && m[1] <= 0xCF &&
                                m[1] != 0xC4 && m[1] != 0xC8 && m[1] != 0xCC;
            const size_t seg_len = (static_cast<size_t>(m[2]) << 8) | m[3];
            if (is_sof) {
                unsigned char sof[5];  // precision + height + width
                f.read(reinterpret_cast<char*>(sof), 5);
                if (f.gcount() < 5) break;
                const uint32_t h = (static_cast<uint32_t>(sof[1]) << 8) | sof[2];
                const uint32_t w = (static_cast<uint32_t>(sof[3]) << 8) | sof[4];
                return {w, h};
            }
            f.seekg(static_cast<std::streamoff>(seg_len) - 2, std::ios::cur);
        }
    }

    if (got >= 12 && sig[4] == 'f' && sig[5] == 't' && sig[6] == 'y' && sig[7] == 'p') {
        f.seekg(0);
        std::vector<unsigned char> header(256 * 1024);
        f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        const auto n = static_cast<size_t>(f.gcount());
        return upix::avif_codec::sniff_dimensions(header.data(), n);
    }
    return {0, 0};
}

} // namespace

#include "process_batch.hpp"

int run_batch(const args::Process& a) {
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
    int jobs = a.jobs;
    bool jobs_explicit = a.jobs_explicit;  // set by cli_app.cpp
    bool use_gpu = a.use_gpu;
    int cli_output_quality = a.output_quality;
    const std::string& cli_output_format = a.output_format;

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

        // Gather image files recursively. Must mirror the decoders Image::load
        // actually ships (share/utils/image.cpp): .bmp/.tiff used to be
        // enumerated here but always failed at load with "Unsupported image
        // format", so they are not tasks at all.
        std::vector<std::string> extensions = {
            ".png", ".jpg", ".jpeg", ".webp", ".avif"
        };
        std::vector<std::pair<std::string, std::string>> tasks;
        for (auto& p : fs::recursive_directory_iterator(input_path)) {
            if (!p.is_regular_file()) continue;
            std::string ext = p.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (!std::ranges::contains(extensions, ext)) continue;
            std::string rel = fs::relative(p.path(), input_path).string();
            std::string out_file = fs::path(output_path) / rel;
            tasks.emplace_back(p.path().string(), out_file);
        }
        if (tasks.empty()) {
            LOG_WARNING("CLI", "No image files found in input directory");
            return 0;
        }
        LOG_INFO("CLI", "Found {} images to process", tasks.size());

        // Largest-first (LPT) scheduling: the biggest image dominates the
        // critical path, so it must start first when workers run in
        // parallel; for a single worker the order does not change the total.
        std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) {
            std::error_code ea, eb;
            const auto sa = fs::file_size(a.first, ea);
            const auto sb = fs::file_size(b.first, eb);
            return std::make_pair(sa, a.first) > std::make_pair(sb, b.first);
        });

        // Model selection
        upix::ModelManager manager(models_dir);
        auto models_opt = manager.scan_models();
        if (!models_opt.has_value() || models_opt->empty()) {
            LOG_ERROR("Engine", "No models found in {}", models_dir);
            return 1;
        }
        auto& models = models_opt.value();

        const upix::ModelInfo* selected_model = nullptr;
        if (model_name.empty()) {
            selected_model = &models[0];
            LOG_INFO("Engine", "No model specified, using first available: {}",
                     selected_model->name);
        } else {
            const auto it = std::ranges::find(models, model_name,
                                              &upix::ModelInfo::name);
            if (it != models.end()) selected_model = &*it;
            if (!selected_model) {
                LOG_ERROR("Engine", "Model not found: {}", model_name);
                LOG_ERROR("Engine", "Available models:");
                for (const auto& m : models) {
                    LOG_ERROR("Engine", "  - {} ({}x)", m.name, m.scale_factor);
                }
                return 1;
            }
        }

        std::string param_path = selected_model->param_path.string();
        std::string bin_path = selected_model->bin_path.string();
        std::string model_name_used = selected_model->name;
        size_t model_total_size = selected_model->total_size;
        int model_input_channels = std::max(1, selected_model->input_channels);
        LOG_INFO("Engine", "Using model: {} ({}x)", model_name_used,
                 selected_model->scale_factor);

        // Load configuration if specified
        upix::ProcessingConfig config;
        if (!config_file.empty()) {
            auto cfg_result = upix::ProcessingConfig::load(config_file);
            if (cfg_result.has_value()) {
                config = cfg_result.value();
                LOG_INFO("Config", "Loaded config from {}", config_file);
            } else {
                LOG_WARNING("Config", "Failed to load config: {}", cfg_result.error());
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
        apply_encoder_overrides(a, config);
        if (!cli_output_format.empty()) {
            config.output_format = upix::normalize_output_format(cli_output_format);
        } else {
            // Auto-infer output format from first output file extension
            if (!tasks.empty()) {
                std::string inferred = infer_format_from_extension(tasks[0].second);
                if (!inferred.empty()) {
                    config.output_format = upix::normalize_output_format(inferred);
                    LOG_INFO("Config", "Auto-detected output format: {}", inferred);
                }
            }
        }

        // Validate configuration
        auto errors = config.validate();
        if (!errors.empty()) {
            LOG_ERROR("Config", "Configuration errors:");
            for (const auto& err : errors) {
                LOG_ERROR("Config", "  - {}", err);
            }
            return 1;
        }

        struct PreparedTask {
            std::string in_path;
            std::string out_path;
            int width = 0;
            int height = 0;
            int channels = 0;
            size_t pixel_bytes = 0;  // decoded frame bytes, for queue byte caps
            upix::Image input_image;
        };

        // Tri-state outcome: a marker skip is neither a success nor a
        // failure, so plain optional<PreparedTask> cannot represent it.
        enum class PrepareStatus { Ok, Skipped, Failed };
        struct PrepareOutcome {
            PrepareStatus status = PrepareStatus::Failed;
            std::optional<PreparedTask> task;
        };
        std::atomic<size_t> skip_count{0};

        auto prepare_task = [&](const std::string& in_path, const std::string& out_path) -> PrepareOutcome {
            if (g_cancel_requested.load()) {
                return {PrepareStatus::Failed, std::nullopt};
            }
            // Read once: the SR-marker check needs the raw bytes and the
            // decode below reuses them (same extension-based dispatch as
            // Image::load).
            auto raw = upix::Image::load_raw(in_path);
            if (!raw.has_value()) {
                LOG_ERROR("Task", "Failed to load image: {}", in_path);
                return {PrepareStatus::Failed, std::nullopt};
            }
            if (!a.force) {
                if (auto marker = upix::sr_marker::probe(raw.value().data(),
                                                         raw.value().size())) {
                    skip_count.fetch_add(1);
                    LOG_INFO("Task",
                             "Skipped (already super-resolved by this tool): {} "
                             "[model={} scale={}x ts={}]; --force re-processes",
                             in_path, marker->model, marker->scale, marker->ts);
                    return {PrepareStatus::Skipped, std::nullopt};
                }
            }
            auto img_res = upix::Image::load_from_memory(
                fs::path(in_path).extension().string(), raw.value());
            if (!img_res.has_value()) {
                LOG_ERROR("Task", "Failed to load image: {}", in_path);
                return {PrepareStatus::Failed, std::nullopt};
            }

            PreparedTask prepared;
            prepared.in_path = in_path;
            prepared.out_path = out_path;
            prepared.input_image = std::move(img_res.value());

            // Portrait JPEGs: bake the EXIF orientation in right after
            // decode (same rule as the single-image and remote paths).
            const int exif_orientation = upix::exif::parse_jpeg_exif_orientation(
                raw.value().data(), raw.value().size());
            if (exif_orientation != 1 &&
                upix::exif::autorotate_image(prepared.input_image, exif_orientation)) {
                LOG_INFO("Task", "applied EXIF orientation {} for {}",
                         exif_orientation, in_path);
            }

            if (!normalize_channels_for_model(prepared.input_image, model_input_channels, in_path)) {
                return {PrepareStatus::Failed, std::nullopt};
            }

            prepared.width = prepared.input_image.width();
            prepared.height = prepared.input_image.height();
            prepared.channels = prepared.input_image.channels();
            prepared.pixel_bytes = prepared.input_image.size();

            return {PrepareStatus::Ok, std::move(prepared)};
        };

        bool enable_progress_log = false;
        if (const char* progress_env = std::getenv("UPIX_PROGRESS")) {
            enable_progress_log = std::string(progress_env) == "1";
        }
        // Track processing start time for ETA
        auto batch_start_time = std::chrono::steady_clock::now();

        struct ProcessedTask {
            std::string out_path;
            int out_width = 0;
            int out_height = 0;
            int channels = 0;
            int scale = 0;           // engine scale factor, for the SR marker
            size_t pixel_bytes = 0;  // encoded-ready frame bytes, for queue byte caps
            upix::Image output_image;
        };

        auto infer_prepared_task = [&](PreparedTask&& prepared, upix::Engine& eng) -> std::optional<ProcessedTask> {
            if (enable_progress_log) {
                LOG_INFO("Engine", "Processing image: {}", prepared.in_path);
            }

            upix::ProcessingConfig local_config = config;

            if (auto_tile || local_config.auto_tile_size) {
                LOG_INFO("Task", "Auto-tile for {}", prepared.in_path);
                upix::TileSizeOptimizer optimizer;
                upix::ModelConstraints mc;
                mc.model_name = model_name_used;
                mc.model_size_mb = static_cast<float>(model_total_size) / (1024 * 1024);
                mc.min_tile_size = local_config.min_tile_size;
                mc.max_tile_size = local_config.max_tile_size;

                local_config.tile_size = optimizer.find_optimal_tile_size(
                    eng, mc, local_config,
                    prepared.width, prepared.height
                );
                local_config.auto_tile_size = true;
                LOG_INFO("Task", "Optimal tile size: {}", local_config.tile_size);
            }

            upix::TileConfig tile_config;
            tile_config.tile_size = local_config.tile_size;
            tile_config.overlap = local_config.tile_overlap;
            tile_config.batch_size = local_config.batch_size;

            int out_width = prepared.width * eng.get_scale_factor();
            int out_height = prepared.height * eng.get_scale_factor();
            upix::Image output_image(out_width, out_height, prepared.channels);

            upix::TileProcessor processor;
            auto result = processor.process_image_u8(
                prepared.input_image.data(), output_image.data(),
                prepared.width, prepared.height, prepared.channels,
                eng, tile_config,
                [&](const upix::ProcessingProgress& prog) {
                    // Always print progress to stderr
                    print_progress_stderr(prog, batch_start_time);
                    if (enable_progress_log &&
                        prog.total_tiles > 0 &&
                        (prog.completed_tiles % 10 == 0 || prog.completed_tiles == prog.total_tiles)) {
                        float pct = prog.percentage();
                        LOG_INFO("Engine", "Progress: {}/{} tiles ({:.0f}%)",
                                 prog.completed_tiles, prog.total_tiles, pct);
                    }
                },
                g_cancel_requested
            );

            if (!result.has_value()) {
                if (g_cancel_requested.load()) {
                    LOG_WARNING("Task", "Processing cancelled by signal");
                }
                LOG_ERROR("Task", "Processing failed: {}", result.error().message);
                return std::nullopt;
            }

            ProcessedTask processed;
            processed.out_path = prepared.out_path;
            processed.out_width = out_width;
            processed.out_height = out_height;
            processed.channels = prepared.channels;
            processed.scale = eng.get_scale_factor();
            processed.pixel_bytes = output_image.size();
            processed.output_image = std::move(output_image);
            return processed;
        };

        auto save_processed_task = [&](ProcessedTask&& processed) -> bool {
            std::filesystem::path out_path_obj(processed.out_path);
            const std::filesystem::path final_out_path = upix::resolve_output_path(out_path_obj, config);
            if (!final_out_path.parent_path().empty()) {
                std::filesystem::create_directories(final_out_path.parent_path());
            }

            const auto sr_xmp = upix::sr_marker::build_xmp(
                model_name_used, processed.scale,
                static_cast<int64_t>(std::time(nullptr)));
            auto save_res = processed.output_image.save(final_out_path, config.output_quality, sr_xmp, config.encoder);
            if (!save_res.has_value()) {
                LOG_ERROR("Task", "Failed to save output: {} -> {}",
                          save_res.error().message, final_out_path.string());
                return false;
            }

            if (enable_progress_log) {
                LOG_INFO("Task", "Completed: {}", final_out_path.string());
            }
            return true;
        };

        enum class TaskResult { Success, Failure, Skipped };
        auto process_one = [&](const std::string& in_path, const std::string& out_path, upix::Engine& eng) -> TaskResult {
            auto prepared = prepare_task(in_path, out_path);
            if (prepared.status != PrepareStatus::Ok) {
                return prepared.status == PrepareStatus::Skipped
                           ? TaskResult::Skipped
                           : TaskResult::Failure;
            }
            auto processed = infer_prepared_task(std::move(prepared.task.value()), eng);
            if (!processed.has_value()) {
                return TaskResult::Failure;
            }
            return save_processed_task(std::move(processed.value()))
                       ? TaskResult::Success
                       : TaskResult::Failure;
        };

        // GPU directory mode defaults to VRAM-sized multi-engine concurrency
        // (decided inside that branch, where engine #0 reports the budget)
        // with the same prepare/SR/save pipeline stages as -j 1, just N
        // engines in the SR stage; an explicit -j 1 still pins the
        // single-engine variant of that pipeline, and CPU jobs<=1 keeps the
        // plain serial loop.
        const bool single_engine_mode =
            jobs <= 1 && !(use_gpu && !jobs_explicit && tasks.size() > 1);
        if (single_engine_mode) {
            auto engine = upix::create_ncnn_engine();
            auto device_type = use_gpu ? upix::DeviceType::GPU_VULKAN : upix::DeviceType::CPU;
            auto init_result = engine->initialize(device_type);
            if (!init_result.has_value()) {
                LOG_ERROR("Engine", "Failed to initialize engine");
                return 1;
            }
            const bool initialized_on_gpu = engine->current_device() == upix::DeviceType::GPU_VULKAN;
            LOG_INFO("Engine", "Engine initialized on {}",
                     initialized_on_gpu ? "GPU" : "CPU");

            // Load model once
            auto load_result = engine->load_model(param_path, bin_path);
            if (!load_result.has_value()) {
                LOG_ERROR("Engine", "Failed to load model: {}", load_result.error().message);
                return 1;
            }
            // Override engine's heuristic scale detection with the
            // authoritative value parsed from the model filename.
            engine->set_scale_factor(selected_model->scale_factor);
            LOG_INFO("Engine", "Model loaded successfully");

            int scale = engine->get_scale_factor();
            LOG_INFO("Engine", "Scale factor: {}x", scale);

            // Auto tile size: use engine's VRAM-aware recommendation if not explicitly set
            if (!tile_size_explicit && !auto_tile) {
                tile_size = engine->get_recommended_tile_size();
                config.tile_size = tile_size;
                LOG_INFO("Engine", "Auto-selected tile size: {}", tile_size);
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
                // Byte caps on top of the count limits: a directory of huge
                // images makes each queued frame worth hundreds of MB, so a
                // count-only bound can pin several GB of decoded inputs /
                // full outputs ahead of the GPU and the encoders. A queue
                // always accepts its first item, so one oversized image that
                // alone exceeds the cap still flows.
                constexpr size_t kPreparedQueueByteCap = 768u << 20;
                constexpr size_t kProcessedQueueByteCap = 1024u << 20;
                size_t prepared_queue_bytes = 0;
                size_t processed_queue_bytes = 0;
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
                            if (prepared.status != PrepareStatus::Ok) {
                                // Skip (already logged + counted) or load
                                // failure (error logged) — never enqueued.
                                continue;
                            }
                            const size_t task_bytes = prepared.task.value().pixel_bytes;
                            std::unique_lock<std::mutex> lock(prepared_mutex);
                            while ((prepared_queue.size() >= prepared_queue_limit ||
                                    (prepared_queue_bytes > 0 &&
                                     prepared_queue_bytes + task_bytes > kPreparedQueueByteCap)) &&
                                   !g_cancel_requested.load()) {
                                prepared_cv.wait_for(lock, kCancelPoll);
                            }
                            if (g_cancel_requested.load()) {
                                break;
                            }
                            prepared_queue_bytes += task_bytes;
                            prepared_queue.emplace_back(std::move(prepared.task.value()));
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
                    auto enqueue_processed = [&](ProcessedTask&& processed) {
                        const size_t task_bytes = processed.pixel_bytes;
                        std::unique_lock<std::mutex> lock(processed_mutex);
                        while ((processed_queue.size() >= processed_queue_limit ||
                                (processed_queue_bytes > 0 &&
                                 processed_queue_bytes + task_bytes > kProcessedQueueByteCap)) &&
                               !g_cancel_requested.load()) {
                            processed_cv.wait_for(lock, kCancelPoll);
                        }
                        if (g_cancel_requested.load()) {
                            return false;
                        }
                        processed_queue_bytes += task_bytes;
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

                            // Batch sizing + fill wait were tied to the (now-removed)
                            // UPIX_GPU_SUBMIT_QUEUE* env vars. With submit_budget
                            // pinned to 1 inside the engine, batching gains nothing —
                            // take one prepared task at a time.
                            prepared_queue_bytes -= prepared_queue.front().pixel_bytes;
                            prepared_batch.emplace_back(std::move(prepared_queue.front()));
                            prepared_queue.pop_front();
                        }
                        prepared_cv.notify_all();

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
                                processed_queue_bytes -= processed_queue.front().pixel_bytes;
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
                    if (process_one(task.first, task.second, *engine) == TaskResult::Success) {
                        success_count++;
                    }
                }
            }

            const size_t skipped = skip_count.load();
            LOG_INFO("CLI", "Processing summary: {}/{} succeeded{}",
                     success_count, tasks.size(),
                     skipped > 0 ? std::format(", {} skipped (already super-resolved)",
                                               skipped)
                                 : std::string{});
            return success_count + skipped == tasks.size() ? 0 : 1;
        } else {
            if (use_gpu) {
                struct EngineInitResult {
                    std::unique_ptr<upix::Engine> engine;
                    std::string error;
                };
                auto init_engine = [&](size_t i) -> EngineInitResult {
                    auto engine = upix::create_ncnn_engine();
                    auto init_result = engine->initialize(upix::DeviceType::GPU_VULKAN);
                    if (!init_result.has_value()) {
                        return {nullptr, std::format("Failed to initialize GPU engine #{}", i)};
                    }
                    auto load_result = engine->load_model(param_path, bin_path);
                    if (!load_result.has_value()) {
                        return {nullptr, std::format(
                            "Failed to load model into GPU engine #{}: {}",
                            i, load_result.error().message)};
                    }
                    return {std::move(engine), ""};
                };

                // Engine #0 first: it reports the VRAM budget that sizes the
                // default worker count (when -j was not given).
                auto primary = init_engine(0);
                if (!primary.engine) {
                    LOG_ERROR("Engine", primary.error);
                    return 1;
                }

                size_t requested_workers = static_cast<size_t>(std::max(1, jobs));
                // Admission control (applies to explicit -j too — an
                // overcommitted worker count fails mid-run when concurrent
                // full-output buffers exhaust VRAM, which is far worse than
                // running with fewer workers): with W workers the peak
                // device memory is roughly the sum of the W largest images'
                // full-output buffers (LPT order starts the biggest first).
                // Pick the largest W ≤ 4 whose projected peak fits 90% of
                // the heap budget; the per-image OOM ladder covers estimate
                // errors.
                const size_t budget_mb = primary.engine->get_device_memory_budget_mb();
                const int scale = primary.engine->get_scale_factor();
                if (budget_mb > 0) {
                    std::vector<size_t> needs_mb;
                    needs_mb.reserve(tasks.size());
                    for (const auto& t : tasks) {
                        const auto [w, h] = sniff_image_dimensions(t.first);
                        if (w == 0 || h == 0) continue;
                        // float accumulator + weight map + u8 staging:
                        // ~20 B per output pixel (C=3), 5% headroom.
                        const double out_px = static_cast<double>(w) * h * scale * scale;
                        needs_mb.push_back(static_cast<size_t>(out_px * 21.0 / (1024.0 * 1024.0)));
                    }
                    std::sort(needs_mb.rbegin(), needs_mb.rend());
                    size_t peak_mb = 0;
                    size_t fit = 1;
                    for (size_t w = 0; w < needs_mb.size() && w < 4; ++w) {
                        peak_mb += needs_mb[w];
                        if (peak_mb > budget_mb * 9 / 10) break;
                        fit = w + 1;
                    }
                    if (!jobs_explicit) {
                        requested_workers = fit;
                        LOG_INFO("Engine",
                                 "Auto-selected jobs={} from VRAM budget {}MB (projected peak {}MB with 4 workers; use -j to override)",
                                 fit, budget_mb, peak_mb);
                    } else if (requested_workers > fit) {
                        LOG_WARNING("Engine",
                                    "jobs={} would overcommit VRAM (projected peak {}MB vs budget {}MB), capping to {} workers",
                                    requested_workers, peak_mb, budget_mb, fit);
                        requested_workers = fit;
                    }
                }
                size_t worker_count = std::min(requested_workers, tasks.size());

                std::vector<std::unique_ptr<upix::Engine>> gpu_engines;
                gpu_engines.reserve(worker_count);
                gpu_engines.push_back(std::move(primary.engine));

                if (worker_count > 1) {
                    std::vector<std::future<EngineInitResult>> init_futures;
                    init_futures.reserve(worker_count - 1);
                    for (size_t i = 1; i < worker_count; ++i) {
                        init_futures.emplace_back(std::async(std::launch::async,
                                                             [&, i]() { return init_engine(i); }));
                    }
                    // Degrade to the engines that did come up instead of
                    // failing the whole run: VRAM exhaustion on init of the
                    // Nth engine leaves 1..N-1 perfectly usable.
                    for (auto& init_future : init_futures) {
                        auto init_result = init_future.get();
                        if (!init_result.engine) {
                            LOG_WARNING("Engine", "{}, continuing with {} workers",
                                        init_result.error, gpu_engines.size());
                            break;
                        }
                        gpu_engines.push_back(std::move(init_result.engine));
                    }
                }
                worker_count = gpu_engines.size();
                LOG_INFO("Engine", "GPU engine workers ready: {}", worker_count);

                // Same auto tile-size selection as the jobs<=1 path: without
                // this, jobs>1 silently used the CLI default (256) while
                // jobs=1 used the engine recommendation, so the tile grid —
                // and therefore the output — depended on --jobs.
                if (!tile_size_explicit && !auto_tile) {
                    tile_size = gpu_engines[0]->get_recommended_tile_size();
                    config.tile_size = tile_size;
                    LOG_INFO("Engine", "Auto-selected tile size: {}", tile_size);
                }

                // Pipelined stage structure — the same prepare/SR/save shape
                // as the -j1 path above, with the SR stage widened to N
                // engines. Decode and encode run on their own thread pools
                // so they no longer stall an engine's GPU between images:
                // prepare workers read+decode into the prepared queue, each
                // engine thread only runs SR, save workers encode+write.
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
                // Byte caps on top of the count limits (same rationale and
                // same values as the -j1 pipeline): a directory of huge
                // images makes each queued frame worth hundreds of MB, so a
                // count-only bound can pin several GB of decoded inputs /
                // full outputs ahead of the engines and the encoders. A
                // queue always accepts its first item, so one oversized
                // image that alone exceeds the cap still flows.
                constexpr size_t kPreparedQueueByteCap = 768u << 20;
                constexpr size_t kProcessedQueueByteCap = 1024u << 20;
                size_t prepared_queue_bytes = 0;
                size_t processed_queue_bytes = 0;
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
                std::atomic<size_t> active_engines{gpu_engines.size()};

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
                            if (prepared.status != PrepareStatus::Ok) {
                                // Skip (already logged + counted) or load
                                // failure (error logged) — never enqueued.
                                continue;
                            }
                            const size_t task_bytes = prepared.task.value().pixel_bytes;
                            std::unique_lock<std::mutex> lock(prepared_mutex);
                            while ((prepared_queue.size() >= prepared_queue_limit ||
                                    (prepared_queue_bytes > 0 &&
                                     prepared_queue_bytes + task_bytes > kPreparedQueueByteCap)) &&
                                   !g_cancel_requested.load()) {
                                prepared_cv.wait_for(lock, kCancelPoll);
                            }
                            if (g_cancel_requested.load()) {
                                break;
                            }
                            prepared_queue_bytes += task_bytes;
                            prepared_queue.emplace_back(std::move(prepared.task.value()));
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

                std::vector<std::thread> engine_threads;
                engine_threads.reserve(worker_count);
                for (size_t i = 0; i < worker_count; ++i) {
                    engine_threads.emplace_back([&, i]() {
                        upix::Engine& eng = *gpu_engines[i];
                        while (true) {
                            std::optional<PreparedTask> prepared;
                            {
                                std::unique_lock<std::mutex> lock(prepared_mutex);
                                while (prepared_queue.empty() && !prepare_done && !g_cancel_requested.load()) {
                                    prepared_cv.wait_for(lock, kCancelPoll);
                                }
                                if ((prepared_queue.empty() && prepare_done) || g_cancel_requested.load()) {
                                    break;
                                }
                                prepared_queue_bytes -= prepared_queue.front().pixel_bytes;
                                prepared = std::move(prepared_queue.front());
                                prepared_queue.pop_front();
                            }
                            prepared_cv.notify_all();

                            auto processed = infer_prepared_task(std::move(prepared.value()), eng);
                            if (!processed.has_value()) {
                                continue;
                            }

                            const size_t task_bytes = processed.value().pixel_bytes;
                            {
                                std::unique_lock<std::mutex> lock(processed_mutex);
                                while ((processed_queue.size() >= processed_queue_limit ||
                                        (processed_queue_bytes > 0 &&
                                         processed_queue_bytes + task_bytes > kProcessedQueueByteCap)) &&
                                       !g_cancel_requested.load()) {
                                    processed_cv.wait_for(lock, kCancelPoll);
                                }
                                if (g_cancel_requested.load()) {
                                    break;
                                }
                                processed_queue_bytes += task_bytes;
                                processed_queue.emplace_back(std::move(processed.value()));
                                lock.unlock();
                                processed_cv.notify_one();
                            }
                        }
                        if (active_engines.fetch_sub(1) == 1) {
                            std::lock_guard<std::mutex> lock(processed_mutex);
                            process_done = true;
                            processed_cv.notify_all();
                            prepared_cv.notify_all();
                        }
                    });
                }

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
                                processed_queue_bytes -= processed_queue.front().pixel_bytes;
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
                for (auto& engine_thread : engine_threads) {
                    engine_thread.join();
                }
                for (auto& save_thread : save_threads) {
                    save_thread.join();
                }

                const size_t skipped = skip_count.load();
                LOG_INFO("CLI", "Processing summary: {}/{} succeeded{}",
                         save_success_count.load(), tasks.size(),
                         skipped > 0 ? std::format(", {} skipped (already super-resolved)",
                                                   skipped)
                                     : std::string{});
                return save_success_count.load() + skipped == tasks.size() ? 0 : 1;
            } else {
                // CPU EnginePool: fixed worker pool pulling tasks via an atomic
                // index (same shape as the GPU jobs>1 branch). One std::async
                // per file would race acquire() with more tasks than engines
                // and stack N engines' default ncnn thread armies.
                size_t requested_workers = static_cast<size_t>(std::max(1, jobs));
                size_t worker_count = std::min(requested_workers, tasks.size());
                upix::EnginePool pool(worker_count, param_path, bin_path, upix::DeviceType::CPU);
                LOG_INFO("Engine", "Created engine pool with {} CPU engines", worker_count);

                // Same auto tile-size selection as the jobs<=1 path: without
                // this, jobs>1 silently used the CLI default (256) while
                // jobs=1 used the engine recommendation (128 on CPU), so the
                // tile grid — and therefore the output — depended on --jobs.
                if (!tile_size_explicit && !auto_tile) {
                    upix::Engine* probe = pool.acquire();
                    tile_size = probe->get_recommended_tile_size();
                    pool.release(probe);
                    config.tile_size = tile_size;
                    LOG_INFO("Engine", "Auto-selected tile size: {}", tile_size);
                }

                std::atomic<size_t> next_task{0};
                std::atomic<size_t> success_count{0};
                std::vector<std::thread> workers;
                workers.reserve(worker_count);

                for (size_t i = 0; i < worker_count; ++i) {
                    workers.emplace_back([&]() {
                        while (!g_cancel_requested.load()) {
                            size_t idx = next_task.fetch_add(1);
                            if (idx >= tasks.size()) {
                                break;
                            }
                            upix::Engine* eng = pool.acquire();
                            bool ok = false;
                            try {
                                ok = process_one(tasks[idx].first, tasks[idx].second, *eng) ==
                                     TaskResult::Success;
                            } catch (const std::exception& e) {
                                LOG_ERROR("Engine", "Task processing exception: {}", e.what());
                                ok = false;
                            } catch (...) {
                                LOG_ERROR("Engine", "Task processing unknown exception");
                                ok = false;
                            }
                            pool.release(eng);
                            if (ok) {
                                success_count.fetch_add(1);
                            }
                        }
                    });
                }

                for (auto& w : workers) {
                    w.join();
                }

                const size_t skipped = skip_count.load();
                LOG_INFO("CLI", "Processing summary: {}/{} succeeded{}",
                         success_count.load(), tasks.size(),
                         skipped > 0 ? std::format(", {} skipped (already super-resolved)",
                                                   skipped)
                                     : std::string{});
                return success_count.load() + skipped == tasks.size() ? 0 : 1;
            }
        }
}
