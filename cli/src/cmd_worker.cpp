// `crossscale worker connect <host>:<port>`.
//
// Connects to a coordinator, handshakes, syncs models, then loops receiving
// tasks. On disconnect it reconnects with exponential backoff (capped at 12s).
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <thread>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/logger.hpp"
#include "core/engine.hpp"
#include "core/distributed/worker.hpp"

namespace {

// Parse "<host>:<port>" → (host, port). Returns false and logs on bad format.
auto split_host_port(const std::string& host_port, std::string& host, uint16_t& port)
    -> bool {
    auto colon = host_port.find(':');
    if (colon == std::string::npos) {
        LOG_ERROR("CLI", "Invalid host:port format. Use <host>:<port>");
        return false;
    }
    host = host_port.substr(0, colon);
    try {
        long p = std::stol(host_port.substr(colon + 1));
        if (p < 1 || p > 65535) throw std::out_of_range("port");
        port = static_cast<uint16_t>(p);
    } catch (const std::exception&) {
        LOG_ERROR("CLI", "Invalid port in host:port: " + host_port);
        return false;
    }
    return true;
}

}  // namespace

int cmd_worker(const args::Worker& a) {
    std::string host;
    uint16_t port = 0;
    if (!split_host_port(a.host_port, host, port)) {
        return 1;
    }

    LOG_INFO("Worker", "Connecting to coordinator at " + host + ":" + std::to_string(port));
    if (a.recommended_weight > 0.0f) {
        LOG_INFO("Worker", "Using manual recommended weight: " + std::to_string(a.recommended_weight));
    }
    if (a.queue_depth > 0) {
        LOG_INFO("Worker", "Using manual queue depth: " + std::to_string(a.queue_depth));
    }
    if (a.recommended_tile_size > 0) {
        LOG_INFO("Worker", "Using manual recommended tile size: " + std::to_string(a.recommended_tile_size));
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    int reconnect_attempt = 0;
    while (g_running) {
        auto worker = std::make_unique<crossscale::Worker>();
        worker->set_recommended_weight(a.recommended_weight);
        worker->set_queue_depth(a.queue_depth);
        worker->set_recommended_tile_size(a.recommended_tile_size);

        auto conn_result = worker->connect(host, port, a.key);
        if (!conn_result.has_value()) {
            LOG_ERROR("Worker", "Failed to connect: " + conn_result.error().message);
            if (!g_running) break;
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
                if (!g_running) break;
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
            if (!g_running) break;
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
        if (!g_running) break;
        reconnect_attempt += 1;
        const int retry_delay_seconds = std::min(12, 1 << std::min(reconnect_attempt, 3));
        LOG_WARNING("Worker", "Disconnected from coordinator, reconnect in " + std::to_string(retry_delay_seconds) + "s");
        std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
    }

    LOG_INFO("Worker", "Worker stopped");
    return 0;
}
