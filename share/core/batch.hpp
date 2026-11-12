// The directory/single-file batch pipeline (docs/SPEC-frontends.md S1: the
// processing pipeline lives in share, frontends only drive it). This is the
// former cli/src/process_batch.cpp body with three seams:
//   * progress/events  → BatchCallbacks (CLI maps to JSON lines/stderr,
//                        desktop maps to machine-event lines for the job card)
//   * cancellation     → a caller-owned atomic (CLI's signal flag, desktop's
//                        job-cancel atomic)
//   * output naming    → pure functions in core/config.hpp
// The CLI keeps a thin adapter (cli/src/process_batch.cpp); desktop runs this
// directly for dirs and for single files the warm daemon cannot serve
// (CPU, auto-tile, daemon down).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.hpp"

namespace upix {

// Collect <input, output> pairs for a directory batch: image walk
// (only extensions Image::load ships), lexically-relative outputs preserving
// the tree shape, unreadable entries and `upix_*` output/backup directories
// skipped. Delays creating `output_dir` until at least one input is found.
// On failure returns nullopt with a human message in `error`.
auto gather_directory_tasks(const std::string& input_dir,
                            const std::string& output_dir,
                            std::string& error,
                            bool recursive = true)
    -> std::optional<std::vector<std::pair<std::string, std::string>>>;

// An input item together with its optional folder-batch root (empty for single
// files). Non-empty `batch_root` preserves relative tree structure under
// `<batch_root>/upix_<model>/` (or `custom_out_dir` if provided).
struct BatchInput {
    std::string path;
    std::string batch_root;
};

auto batch_out_dir(const BatchInput& in,
                   const std::string& model_name,
                   const std::string& custom_out_dir = {})
    -> std::optional<std::filesystem::path>;

auto compute_batch_output_path(const BatchInput& in,
                               const std::string& model_name,
                               const ProcessingConfig& cfg,
                               const std::string& custom_out_dir,
                               bool overwrite_original) -> std::filesystem::path;

// Copies `src` to `<parent>/upix_orig/<filename>` via an atomic sibling temp
// file if that backup does not already exist (never clobbers an existing backup).
auto backup_original_once(const std::filesystem::path& src,
                          std::string& err) -> bool;

struct BatchRequest {
    // Pre-gathered tasks (gather_directory_tasks, or a single file the
    // frontend named explicitly). Outputs are resolved through
    // resolve_output_path with `config` at save time.
    std::vector<std::pair<std::string, std::string>> tasks;
    std::string models_dir;
    // Empty = first scanned model (same rule as the CLI).
    std::string model_name;
    // Caller-resolved effective config (tile/format/quality/encoder...). A
    // missing output format is inferred from the first task's extension.
    ProcessingConfig config;
    bool recommend_tile_size = false;  // engine-recommended tile when no config pinned one
    bool force = false;                // re-process SR-marked inputs
    bool overwrite_original = false;   // write results back onto inputs (locks format to Follow)
    bool backup_original = true;       // when overwrite_original, save once into upix_orig/
    bool use_gpu = true;
    int jobs = 1;
    // When false, GPU directory runs may size the worker count from the VRAM
    // budget (and CPU runs clamp to hardware concurrency), matching `upix
    // imgs` without -j.
    bool jobs_explicit = false;
};

struct BatchCallbacks {
    // A file entered the engine ("t":"current" for machine consumers).
    std::function<void(const std::string& file)> on_current;
    // An output was written ("t":"saved").
    std::function<void(const std::string& path)> on_saved;
    // Tile progress for the current image. `eta_sec` is the batch-level ETA
    // (finished images / total images), the number the CLI's stderr line shows.
    std::function<void(const ProcessingProgress& prog, int eta_sec)> on_progress;
    // A marked input was skipped (per-file note; the CLI batch only logs it).
    std::function<void(const std::string& path)> on_skipped;
    // A per-file error occurred (prepare/infer/backup/save failure).
    std::function<void(const std::string& path, const std::string& reason)> on_error;
};

struct BatchStats {
    size_t total = 0;
    size_t succeeded = 0;
    size_t skipped = 0;
    bool cancelled = false;
    // failed = total - succeeded - skipped (kept derived to make the
    // invariant unfalsifiable).
    auto failed() const -> size_t { return total - succeeded - skipped; }
};

// Runs the batch to completion (or until `cancel` flips). Fatal setup
// problems (no models, invalid config, output collision) return nullopt;
// the reason is logged (utils/logger). Per-file failures count into
// BatchStats and carry on.
auto run_batch(const BatchRequest& request, const BatchCallbacks& callbacks,
               const std::atomic<bool>& cancel) -> std::optional<BatchStats>;

}  // namespace upix
