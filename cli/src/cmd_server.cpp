// `crossscale server {start, info}`.
//
// start: launch a Coordinator on `port`, broadcast mDNS, print the connection
//        card (IP/QR/key), and poll until Ctrl+C. Auto-generates a key if none.
// info:  just print the connection card for the given port/key (no server).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <thread>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/logger.hpp"
#include "utils/mdns.hpp"
#include "core/distributed/coordinator.hpp"
#include "core/distributed/identity.hpp"

int cmd_server(const args::Server& a) {
    if (a.subcommand == "info") {
        auto identity = crossscale::IdentityKey::load_or_create(
            crossscale::default_identity_path());
        print_connection_card(a.port, identity.public_key());
        return 0;
    }

    // ── start ───────────────────────────────────────────────────────────────
    auto identity = crossscale::IdentityKey::load_or_create(
        crossscale::default_identity_path());
    LOG_INFO("Server", "PC identity fingerprint: " +
              crossscale::fingerprint(identity.public_key()));

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto coordinator = std::make_unique<crossscale::Coordinator>();
    coordinator->set_models_directory("./models");
    coordinator->set_trust_store_path(crossscale::default_trust_path());
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

    auto start_result = coordinator->start(a.port, identity);
    if (!start_result.has_value()) {
        LOG_ERROR("Server", "Failed to start coordinator: " + start_result.error().message);
        return 1;
    }

    crossscale::MDNSBroadcaster mdns_broadcaster;
    const auto mdns_result = mdns_broadcaster.start("CrossScale-PC", a.port, identity.public_key());
    if (!mdns_result.has_value()) {
        LOG_WARNING("Server", "mDNS publish unavailable: " + mdns_result.error().message);
    }

    LOG_INFO("Server", "Coordinator running. Press Ctrl+C to stop.");

    print_connection_card(a.port, identity.public_key());

    std::optional<size_t> last_worker_count;
    while (g_running && coordinator->is_running()) {
        const size_t worker_count = coordinator->device_count();
        if (!last_worker_count.has_value() || last_worker_count.value() != worker_count) {
            LOG_INFO("Server", "Connected workers: " + std::to_string(worker_count));
            last_worker_count = worker_count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("Server", "Shutting down coordinator...");
    mdns_broadcaster.stop();
    coordinator->stop();
    return 0;
}
