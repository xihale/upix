#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <csignal>

#include "qrcodegen.hpp"

#include "utils/logger.hpp"
#include "utils/image.hpp"
#include "utils/benchmark.hpp"
#include "utils/crypto.hpp"
#include "core/engine.hpp"
#include "core/model_manager.hpp"
#include "core/tile_processor.hpp"
#include "core/distributed/coordinator.hpp"
#include "core/distributed/worker.hpp"

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
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

    std::string command = argv[1];

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    if (command == "process") {
        std::string input_path, output_path, model_name;
        std::string models_dir = "./models";

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-i" && i + 1 < argc) input_path = argv[++i];
            else if (arg == "-o" && i + 1 < argc) output_path = argv[++i];
            else if (arg == "-m" && i + 1 < argc) model_name = argv[++i];
            else if (arg == "--models" && i + 1 < argc) models_dir = argv[++i];
        }

        if (input_path.empty() || output_path.empty()) {
            LOG_ERROR("CLI", "Missing input or output path");
            return 1;
        }

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
        auto init_result = engine->initialize(crossscale::DeviceType::GPU_VULKAN);
        if (!init_result.has_value()) {
            LOG_WARNING("Engine", "GPU not available, falling back to CPU");
            auto cpu_result = engine->initialize(crossscale::DeviceType::CPU);
            if (!cpu_result.has_value()) {
                LOG_ERROR("Engine", "Failed to initialize engine");
                return 1;
            }
        }

        LOG_INFO("Engine", std::string("Engine initialized on ") +
                 (engine->current_device() == crossscale::DeviceType::GPU_VULKAN ? "GPU" : "CPU"));

        std::string param_path, bin_path;

        if (!model_name.empty()) {
            param_path = models_dir + "/" + model_name + ".param";
            bin_path = models_dir + "/" + model_name + ".bin";
        } else {
            crossscale::ModelManager manager(models_dir);
            auto models = manager.scan_models();
            if (models.has_value() && !models.value().empty()) {
                auto& first_model = models.value()[0];
                param_path = first_model.param_path;
                bin_path = first_model.bin_path;
                model_name = first_model.name;
                LOG_INFO("Engine", "Using model: " + model_name);
            } else {
                LOG_ERROR("Engine", "No models found in " + models_dir);
                return 1;
            }
        }

        auto load_result = engine->load_model(param_path, bin_path);
        if (!load_result.has_value()) {
            LOG_ERROR("Engine", "Failed to load model: " + load_result.error().message);
            return 1;
        }

        LOG_INFO("Engine", "Model loaded successfully");

        int scale = engine->get_scale_factor();
        LOG_INFO("Engine", "Scale factor: " + std::to_string(scale) + "x");

        int out_width = image.width() * scale;
        int out_height = image.height() * scale;
        crossscale::Image output_image(out_width, out_height, image.channels());

        LOG_INFO("Engine", "Processing image...");

        const int tile_size = 256;
        const int overlap = 16;

        int tiles_x = (image.width() + tile_size - 1) / tile_size;
        int tiles_y = (image.height() + tile_size - 1) / tile_size;
        int total_tiles = tiles_x * tiles_y;
        int processed_tiles = 0;

        std::atomic<bool> cancel_flag{false};

        for (int ty = 0; ty < tiles_y && !cancel_flag; ++ty) {
            for (int tx = 0; tx < tiles_x && !cancel_flag; ++tx) {
                int x = tx * tile_size;
                int y = ty * tile_size;
                int w = std::min(tile_size, image.width() - x);
                int h = std::min(tile_size, image.height() - y);

                auto tile_image = image.extract_region(x, y, w, h);
                if (!tile_image.has_value()) {
                    LOG_ERROR("Engine", "Failed to extract tile at (" + std::to_string(x) + "," + std::to_string(y) + ")");
                    continue;
                }

                std::vector<float> input_tile(w * h * image.channels());
                for (int c = 0; c < image.channels(); ++c) {
                    for (int row = 0; row < h; ++row) {
                        for (int col = 0; col < w; ++col) {
                            input_tile[(row * w + col) * image.channels() + c] =
                                tile_image.value().at(col, row, c) / 255.0f;
                        }
                    }
                }

                int out_tile_w = w * scale;
                int out_tile_h = h * scale;
                std::vector<float> output_tile(out_tile_w * out_tile_h * image.channels());

                auto process_result = engine->process(
                    input_tile.data(), output_tile.data(),
                    w, h, image.channels(), cancel_flag
                );

                if (!process_result.has_value()) {
                    LOG_ERROR("Engine", "Failed to process tile: " + process_result.error().message);
                    continue;
                }

                int out_x = x * scale;
                int out_y = y * scale;

                for (int c = 0; c < image.channels(); ++c) {
                    for (int row = 0; row < out_tile_h; ++row) {
                        for (int col = 0; col < out_tile_w; ++col) {
                            float val = output_tile[(row * out_tile_w + col) * image.channels() + c];
                            val = std::clamp(val * 255.0f, 0.0f, 255.0f);
                            output_image.at(out_x + col, out_y + row, c) = static_cast<uint8_t>(val);
                        }
                    }
                }

                ++processed_tiles;
                if (processed_tiles % 10 == 0 || processed_tiles == total_tiles) {
                    LOG_INFO("Engine", "Progress: " + std::to_string(processed_tiles) + "/" +
                             std::to_string(total_tiles) + " tiles");
                }
            }
        }

        if (cancel_flag) {
            LOG_WARNING("Engine", "Processing cancelled");
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

            auto connect_result = worker->connect(host, port, key);
            if (!connect_result.has_value()) {
                LOG_ERROR("Worker", "Failed to connect: " + connect_result.error().message);
                return 1;
            }

            LOG_INFO("Worker", "Connected successfully. Waiting for tasks...");

            while (g_running && worker->is_connected()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            LOG_INFO("Worker", "Disconnecting...");
            worker->disconnect();
            return 0;
        }
        
        LOG_ERROR("CLI", "Unknown worker subcommand: " + subcommand);
        return 1;
    }

    LOG_ERROR("CLI", "Unknown command: " + command);
    print_usage();
    return 1;
}
