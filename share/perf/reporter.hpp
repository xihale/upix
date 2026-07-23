// JSONL event-stream reporter. Emits one JSON object per line. Each event has a
// "type" discriminator (meta/sample/tile/summary/comparison). Downstream tooling
// parses line-by-line with any JSON library.
#pragma once

#include <string>
#include <string_view>

namespace crossscale::perf {

// Overwrite-safe line writer. Writes to a path (or stdout if empty).
class Reporter {
public:
    explicit Reporter(std::string path);
    ~Reporter();

    Reporter(const Reporter&) = delete;
    auto operator=(const Reporter&) -> Reporter& = delete;

    // Open the destination. Returns false on failure.
    auto open() -> bool;

    void emit_meta(std::string_view json);          // raw JSON object body (no braces needed)
    void emit_sample(const struct Sample& s, std::string_view device);
    void emit_tile(int index, double elapsed_ms);
    void emit_summary(std::string_view json);
    void emit_comparison(double gpu_tps, double cpu_tps);

private:
    void write(std::string_view line);

    std::string path_;
    void* file_ = nullptr;  // FILE*
};

} // namespace crossscale::perf
