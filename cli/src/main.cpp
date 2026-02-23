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
#include "utils/network.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <optional>
#include <cstdlib>

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_cancel_requested{false};

void signal_handler(int) {
    g_running = false;
    g_cancel_requested = true;
}

void print_usage() {
    std::cout << "CrossScale - Distributed Image Super-Resolution System\n\n"
              << "Usage:\n"
              << "  crossscale process -i <input> -o <output> -m <model>\n"
              << "  crossscale server start -p <port> [-k <key>]\n"
              << "  crossscale server info\n"
              << "  crossscale worker connect <host>:<port> -k <key>\n"
              << "  crossscale model list\n"
              << "  crossscale benchmark\n"
              << "  crossscale --help\n";
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

    if (command == "process") {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::string input_path, output_path, model_name;
        std::string models_dir = "./models";
        std::string config_file;

        // 瓦片配置参数
        int tile_size = 256;
        int tile_overlap = 16;
        bool auto_tile = false;
        int min_tile = 64;
        int max_tile = 512;
        int batch_size = 1;
        int jobs = 1;
        bool use_gpu = false;  // for directory mode with --jobs > 1

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-i" && i + 1 < argc) input_path = argv[++i];
            else if (arg == "-o" && i + 1 < argc) output_path = argv[++i];
            else if (arg == "-m" && i + 1 < argc) model_name = argv[++i];
            else if (arg == "--models" && i + 1 < argc) models_dir = argv[++i];
            else if (arg == "--tile-size" && i + 1 < argc) tile_size = std::stoi(argv[++i]);
            else if (arg == "--tile-overlap" && i + 1 < argc) tile_overlap = std::stoi(argv[++i]);
            else if (arg == "--batch-size" && i + 1 < argc) batch_size = std::stoi(argv[++i]);
            else if (arg == "--auto-tile") auto_tile = true;
            else if (arg == "--min-tile" && i + 1 < argc) min_tile = std::stoi(argv[++i]);
            else if (arg == "--max-tile" && i + 1 < argc) max_tile = std::stoi(argv[++i]);
            else if (arg == "--jobs" && i + 1 < argc) jobs = std::stoi(argv[++i]);
            else if (arg == "-j" && i + 1 < argc) jobs = std::stoi(argv[++i]);
            else if (arg == "--gpu") use_gpu = true;
            else if (arg == "--config" && i + 1 < argc) config_file = argv[++i];
        }

        if (input_path.empty() || output_path.empty()) {
            LOG_ERROR("CLI", "Missing input or output path");
            return 1;
        }

        // Check if input is a directory for batch processing
        bool is_dir = fs::is_directory(input_path);
        LOG_INFO("Debug", "Input path: " + input_path + " is_dir: " + std::to_string(is_dir));
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
                prepared.width = img_res->width();
                prepared.height = img_res->height();
                prepared.channels = img_res->channels();
                prepared.input_image = std::move(img_res.value());

                return prepared;
            };

            bool enable_progress_log = false;
            if (const char* progress_env = std::getenv("CROSSSCALE_PROGRESS")) {
                enable_progress_log = std::string(progress_env) == "1";
            }
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
                std::filesystem::create_directories(out_path_obj.parent_path());

                auto save_res = processed.output_image.save(processed.out_path);
                if (!save_res.has_value()) {
                    LOG_ERROR("Task", "Failed to save output: " + save_res.error().message);
                    return false;
                }

                if (enable_progress_log) {
                    LOG_INFO("Task", "Completed: " + processed.out_path);
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
                            bool ok = false;
                            try {
                                ok = process_one(task.first, task.second, *eng);
                            } catch (...) {
                                ok = false;
                            }
                            pool.release(eng);
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
        LOG_INFO("Engine", "Using model: " + model_name + " (" + std::to_string(selected_model->scale_factor) + "x)");

        auto load_result = engine->load_model(param_path, bin_path);
        if (!load_result.has_value()) {
            LOG_ERROR("Engine", "Failed to load model: " + load_result.error().message);
            return 1;
        }

        LOG_INFO("Engine", "Model loaded successfully");

        int scale = engine->get_scale_factor();
        LOG_INFO("Engine", "Scale factor: " + std::to_string(scale) + "x");

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

        auto process_result = processor.process_image_u8(
            image.data(), output_image.data(),
            image.width(), image.height(), image.channels(),
            *engine, tile_config,
            [&](const crossscale::ProcessingProgress& progress) {
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

        if (!process_result.has_value()) {
            LOG_ERROR("Engine", "Processing failed: " + process_result.error().message);
            return 1;
        }

        auto save_result = output_image.save(output_path);
        if (!save_result.has_value()) {
            LOG_ERROR("Engine", "Failed to save output: " + save_result.error().message);
            return 1;
        }

        LOG_INFO("Engine", "Output saved: " + output_path);
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
        }, 10, 2);
        
        std::cout << "Benchmark: " << result.name << "\n"
                  << "  Iterations: " << result.iterations << "\n"
                  << "  Avg time: " << result.avg_time_ms << " ms\n"
                  << "  Ops/sec: " << result.ops_per_second << "\n";
        return 0;
    }

    if (command == "server") {
        std::string subcommand = argc > 2 ? argv[2] : "";

        if (subcommand == "start") {
            uint16_t port = 9527;
            std::string key;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-p" && i + 1 < argc) port = std::stoi(argv[++i]);
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

            auto start_result = coordinator->start(port, key);
            if (!start_result.has_value()) {
                LOG_ERROR("Server", "Failed to start coordinator: " + start_result.error().message);
                return 1;
            }

            LOG_INFO("Server", "Coordinator running. Press Ctrl+C to stop.");

            while (g_running && coordinator->is_running()) {
                size_t worker_count = coordinator->device_count();
                LOG_INFO("Server", "Connected workers: " + std::to_string(worker_count));
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }

            LOG_INFO("Server", "Shutting down coordinator...");
            coordinator->stop();
            return 0;
        }

        if (subcommand == "info") {
            auto coordinator = std::make_unique<crossscale::Coordinator>();
            auto conn_info = coordinator->generate_connection_info();

            std::string json = "{\"v\":1,\"ip\":\"" + conn_info.ip +
                              "\",\"port\":" + std::to_string(conn_info.port) +
                              ",\"key\":\"" + conn_info.auth_key + "\"}";

            std::cout << "\nConnection Information:\n"
                      << "  IP: " << conn_info.ip << "\n"
                      << "  Port: " << conn_info.port << "\n"
                      << "  Key: " << conn_info.auth_key << "\n\n"
                      << "QR Code (scan with Android app):\n\n"
                      << generate_qr_ascii(json) << "\n";
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
            
            for (int i = 4; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-k" && i + 1 < argc) key = argv[++i];
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
            uint16_t port = std::stoi(host_port.substr(colon_pos + 1));
            
            LOG_INFO("Worker", "Connecting to coordinator at " + host + ":" + std::to_string(port));

            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);

            auto worker = std::make_unique<crossscale::Worker>();
            auto conn_result = worker->connect(host, port, key);
            if (!conn_result.has_value()) {
                LOG_ERROR("Worker", "Failed to connect: " + conn_result.error().message);
                return 1;
            }

            auto engine = crossscale::create_ncnn_engine();
            auto init_result = engine->initialize(crossscale::DeviceType::GPU_VULKAN);
            if (!init_result.has_value()) {
                LOG_WARNING("Worker", "GPU not available, falling back to CPU");
                auto cpu_result = engine->initialize(crossscale::DeviceType::CPU);
                if (!cpu_result.has_value()) {
                    LOG_ERROR("Worker", "Failed to initialize engine");
                    return 1;
                }
            }
            LOG_INFO("Worker", "Engine initialized on " +
                     std::string(engine->current_device() == crossscale::DeviceType::GPU_VULKAN ? "GPU" : "CPU"));

            auto handshake_result = worker->handshake(*engine);
            if (!handshake_result.has_value()) {
                LOG_ERROR("Worker", "Handshake failed: " + handshake_result.error().message);
                return 1;
            }
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

            LOG_INFO("Worker", "Disconnecting...");
            worker->disconnect();
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
                else if (arg == "--tile-size" && i + 1 < argc) tile_size = std::stoi(argv[++i]);
                else if (arg == "--tile-overlap" && i + 1 < argc) tile_overlap = std::stoi(argv[++i]);
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
            uint16_t port = std::stoi(server_addr.substr(colon_pos + 1));
            
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
            
            crossscale::MessageHeader auth_header;
            auth_header.type = crossscale::MessageType::MSG_AUTH_REQUEST;
            auth_header.payload_length = sizeof(auth_req);
            
            auto auth_data = crossscale::Protocol::serialize(auth_header, &auth_req);
            socket.send_all(auth_data.data(), auth_data.size());
            
            std::vector<uint8_t> resp_header_buf(crossscale::Protocol::HEADER_SIZE);
            socket.receive_all(resp_header_buf.data(), resp_header_buf.size());
            auto resp_header = crossscale::Protocol::parse_header(resp_header_buf.data(), resp_header_buf.size());
            
            if (!resp_header || resp_header->type != crossscale::MessageType::MSG_AUTH_RESPONSE) {
                LOG_ERROR("Client", "Invalid auth response");
                return 1;
            }
            
            std::vector<uint8_t> resp_payload(resp_header->payload_length);
            socket.receive_all(resp_payload.data(), resp_payload.size());
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
            
            crossscale::MessageHeader submit_header;
            submit_header.type = crossscale::MessageType::MSG_CLIENT_SUBMIT;
            submit_header.payload_length = sizeof(task_submit);
            
            auto submit_header_data = crossscale::Protocol::serialize(submit_header, &task_submit);
            socket.send_all(submit_header_data.data(), submit_header_data.size());
            socket.send_all(image.data(), image.size());
            
            // Receive result
            std::vector<uint8_t> result_header_buf(crossscale::Protocol::HEADER_SIZE);
            socket.receive_all(result_header_buf.data(), result_header_buf.size());
            auto result_header = crossscale::Protocol::parse_header(result_header_buf.data(), result_header_buf.size());
            
            if (!result_header || result_header->type != crossscale::MessageType::MSG_CLIENT_RESPONSE) {
                LOG_ERROR("Client", "Invalid task response header");
                return 1;
            }
            
            std::vector<uint8_t> result_resp_payload(result_header->payload_length);
            socket.receive_all(result_resp_payload.data(), result_resp_payload.size());
            crossscale::ClientTaskResponse* task_resp = reinterpret_cast<crossscale::ClientTaskResponse*>(result_resp_payload.data());
            
            if (!task_resp->success) {
                LOG_ERROR("Client", "Task failed: " + std::string(task_resp->error_message));
                return 1;
            }
            
            LOG_INFO("Client", "Task completed, receiving result (" + std::to_string(task_resp->result_data_length) + " bytes)...");
            
            std::vector<uint8_t> result_data(task_resp->result_data_length);
            socket.receive_all(result_data.data(), result_data.size());
            
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

    LOG_ERROR("CLI", "Unknown command: " + command);
    print_usage();
    return 1;
}
