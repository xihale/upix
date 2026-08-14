// Directory batch path, split out of cmd_process (Layer 2 Task 4).
//
// Verbatim move of the former `if (is_dir) { ... }` block into run_batch().
// The four helper lambdas (prepare/infer/save/process_one) stay intact
// inside run_batch; their [&] captures resolve to run_batch's locals.
// No logic change — A/B md5 verified bit-identical.
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
#include "core/model_manager.hpp"
#include "core/distributed/protocol.hpp"
#include "core/tile_optimizer.hpp"
#include "core/tile_processor.hpp"

namespace fs = std::filesystem;

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
        crossscale::ModelManager manager(models_dir);
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
        // GPU submit-queue tuning was driven by 6 CROSSSCALE_GPU_SUBMIT_QUEUE*
        // env vars, but the queue path's payoff (multi-task pipeline parallelism)
        // is disabled by the submit_budget=1 pin in ncnn_gpu_pipeline.cpp (blend
        // RAW/WAR hazard — see commit d5c8605). Hardcode the former defaults;
        // dynamic sizing is gone.
        const size_t gpu_submit_queue_max = 3;
        const size_t gpu_submit_queue_min = 1;  // kept for the failure backoff below

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
            // Override engine's heuristic scale detection with the
            // authoritative value parsed from the model filename.
            engine->set_scale_factor(selected_model->scale_factor);
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
                    const bool use_gpu_submit_queue = false;
                    size_t runtime_queue_depth_cap = 1;

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

                            // Dynamic batch sizing + batch-fill wait were tied to
                            // the (now-removed) CROSSSCALE_GPU_SUBMIT_QUEUE_DYNAMIC
                            // / _MIN_SUBMIT_BATCH / _BATCH_WAIT_MS env vars. With
                            // submit_budget pinned to 1 inside the engine, batching
                            // gains nothing — drain whatever is ready, up to cap.
                            const size_t batch_target = use_gpu_submit_queue
                                ? runtime_queue_depth_cap
                                : static_cast<size_t>(1);

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

                // Same auto tile-size selection as the jobs<=1 path: without
                // this, jobs>1 silently used the CLI default (256) while
                // jobs=1 used the engine recommendation, so the tile grid —
                // and therefore the output — depended on --jobs.
                if (!tile_size_explicit && !auto_tile) {
                    tile_size = gpu_engines[0]->get_recommended_tile_size();
                    config.tile_size = tile_size;
                    LOG_INFO("Engine", "Auto-selected tile size: " + std::to_string(tile_size));
                }

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
                // CPU EnginePool: fixed worker pool pulling tasks via an atomic
                // index (same shape as the GPU jobs>1 branch). One std::async
                // per file would race acquire() with more tasks than engines
                // and stack N engines' default ncnn thread armies.
                size_t requested_workers = static_cast<size_t>(std::max(1, jobs));
                size_t worker_count = std::min(requested_workers, tasks.size());
                crossscale::EnginePool pool(worker_count, param_path, bin_path, crossscale::DeviceType::CPU);
                LOG_INFO("Engine", "Created engine pool with " + std::to_string(worker_count) + " CPU engines");

                // Same auto tile-size selection as the jobs<=1 path: without
                // this, jobs>1 silently used the CLI default (256) while
                // jobs=1 used the engine recommendation (128 on CPU), so the
                // tile grid — and therefore the output — depended on --jobs.
                if (!tile_size_explicit && !auto_tile) {
                    crossscale::Engine* probe = pool.acquire();
                    tile_size = probe->get_recommended_tile_size();
                    pool.release(probe);
                    config.tile_size = tile_size;
                    LOG_INFO("Engine", "Auto-selected tile size: " + std::to_string(tile_size));
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
                            crossscale::Engine* eng = pool.acquire();
                            bool ok = false;
                            try {
                                ok = process_one(tasks[idx].first, tasks[idx].second, *eng);
                            } catch (const std::exception& e) {
                                LOG_ERROR("Engine", std::string("Task processing exception: ") + e.what());
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

                LOG_INFO("CLI", "Processing summary: " + std::to_string(success_count.load()) + "/" + std::to_string(tasks.size()) + " succeeded");
                return success_count.load() == tasks.size() ? 0 : 1;
            }
        }
}
