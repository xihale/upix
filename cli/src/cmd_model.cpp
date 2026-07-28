// `crossscale model list` — scan ./models and print each model's scale + size.
#include <filesystem>
#include <iostream>

#include "args.hpp"
#include "cli_app.hpp"
#include "utils/logger.hpp"
#include "core/model_manager.hpp"

namespace fs = std::filesystem;

int cmd_model(const args::Model&) {
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
