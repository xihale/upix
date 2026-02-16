#include "core/distributed/coordinator.hpp"
#include "core/config.hpp"
#include "utils/image.hpp"
#include "utils/logger.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    using namespace crossscale;

    // Connect to coordinator
    Coordinator coordinator;

    // Start coordinator (this is the server)
    auto start_result = coordinator.start(9527, "test123");
    if (!start_result.has_value()) {
        LOG_ERROR("Test", "Failed to start coordinator: " + start_result.error().message);
        return 1;
    }

    LOG_INFO("Test", "Coordinator started, waiting for workers...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Load test image
    auto img_result = Image::load("/home/xihale/Desktop/learning/android/upscaler/CrossScale/cli/tests/imgs/0.webp");
    if (!img_result.has_value()) {
        LOG_ERROR("Test", "Failed to load image: " + img_result.error().message);
        coordinator.stop();
        return 1;
    }
    auto& image = img_result.value();
    LOG_INFO("Test", "Image loaded: " + std::to_string(image.width()) + "x" + std::to_string(image.height()));

    // Wait for at least one worker
    int wait_seconds = 0;
    while (coordinator.device_count() == 0 && wait_seconds < 10) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wait_seconds++;
    }

    if (coordinator.device_count() == 0) {
        LOG_ERROR("Test", "No workers connected after 10 seconds");
        coordinator.stop();
        return 1;
    }

    LOG_INFO("Test", "Workers connected: " + std::to_string(coordinator.device_count()));

    // Prepare config
    ProcessingConfig config;
    config.tile_size = 256;
    config.tile_overlap = 16;

    // Distribute task
    LOG_INFO("Test", "Distributing task...");
    auto start_time = std::chrono::steady_clock::now();

    auto result = coordinator.distribute_task(
        std::vector<uint8_t>(image.data(), image.data() + image.size()),
        image.width(), image.height(), image.channels(),
        "Real-ESRGANv3-anime-x2",
        2,
        config
    );

    auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    if (result.has_value()) {
        LOG_INFO("Test", "Task completed in " + std::to_string(elapsed.count()) + " seconds");
        LOG_INFO("Test", "Output size: " + std::to_string(result->size()) + " bytes");

        // Save output
        Image output_img(result->data(),
                         image.width() * 2, image.height() * 2, image.channels());
        auto save_result = output_img.save("/tmp/distributed_output.webp");
        if (save_result.has_value()) {
            LOG_INFO("Test", "Saved to /tmp/distributed_output.webp");
        }
    } else {
        LOG_ERROR("Test", "Task failed: " + result.error().message);
    }

    coordinator.stop();
    LOG_INFO("Test", "Coordinator stopped");
    return 0;
}
