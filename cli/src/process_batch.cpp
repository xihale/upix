// CLI adapter over share/core/batch.cpp — see process_batch.hpp. Every
// scheduling/validation/encode decision lives in the core; this file maps
// callbacks to the CLI's machine-event stream and stderr lines.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "process_batch.hpp"

#include "core/batch.hpp"
#include "utils/logger.hpp"

namespace fs = std::filesystem;

namespace {
// GCC 16 immediate-escalation: std::format may not appear lexically inside a
// lambda stored in std::function (the invocation gets promoted to an
// immediate function and fails to compile). Route every JSON line through a
// plain function instead — same bytes either way.
auto current_event(const std::string& file) -> std::string {
    return std::format(R"({{"t":"current","file":"{}}}")",
                       json_escape(fs::path(file).filename().string()));
}
auto saved_event(const std::string& path) -> std::string {
    return std::format(R"({{"t":"saved","path":"{}"}})", json_escape(path));
}
}  // namespace

int run_batch(const args::Process& a, EffectiveProcessingConfig effective) {
    const std::string& input_path = a.input_path;
    const std::string& output_path = a.output_path;
    auto& config = effective.config;

    if (output_path.empty()) {
        LOG_ERROR("CLI", "Output path required when input is a directory");
        return 1;
    }

    std::string error;
    auto tasks = upix::gather_directory_tasks(input_path, output_path, error);
    if (!tasks.has_value()) {
        LOG_ERROR("CLI", "{}", error);
        return 1;
    }
    if (tasks->empty()) {
        LOG_WARNING("CLI", "No image files found in input directory");
        return 0;
    }

    upix::BatchRequest request;
    request.tasks = std::move(*tasks);
    request.models_dir = a.models_dir;
    request.model_name = a.model_name;
    request.config = config;
    request.recommend_tile_size = effective.recommend_tile_size;
    request.force = a.force;
    request.use_gpu = a.use_gpu;
    request.jobs = a.jobs;
    request.jobs_explicit = a.jobs_explicit;

    // Machine events key the GUI/script consumers; human mode renders the
    // stderr progress line (identical numbers, identical wording).
    const bool machine = machine_output_enabled();
    upix::BatchCallbacks callbacks;
    callbacks.on_current = [machine](const std::string& file) {
        if (machine) emit_json_line(current_event(file));
    };
    callbacks.on_saved = [machine](const std::string& path) {
        if (machine) emit_json_line(saved_event(path));
    };
    callbacks.on_skipped = {};  // CLI keeps the historical log-only summary
    // The original batch path passed ONE batch-start clock to every progress
    // report; keep that basis so the stderr ETA drifts exactly as before.
    const auto batch_start = std::chrono::steady_clock::now();
    callbacks.on_progress = [&batch_start](const upix::ProcessingProgress& prog, int) {
        report_progress(prog, batch_start);
    };

    auto stats = upix::run_batch(request, callbacks, g_cancel_requested);
    if (!stats.has_value()) return 1;  // fatal setup problem, already logged
    return stats->succeeded + stats->skipped == stats->total ? 0 : 1;
}
