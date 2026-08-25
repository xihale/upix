#include "perf/reporter.hpp"
#include "perf/probes.hpp"

#include <cstdio>
#include <string>

namespace upix::perf {

Reporter::Reporter(std::string path) : path_(std::move(path)) {}

Reporter::~Reporter() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
    }
}

auto Reporter::open() -> bool {
    if (path_.empty()) {
        file_ = stdout;
        return true;
    }
    file_ = std::fopen(path_.c_str(), "w");
    return file_ != nullptr;
}

void Reporter::write(std::string_view line) {
    if (!file_) return;
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::fwrite(line.data(), 1, line.size(), static_cast<FILE*>(file_));
    std::fputc('\n', static_cast<FILE*>(file_));
}

void Reporter::emit_meta(std::string_view json) {
    // Caller passes pre-serialized inner fields, we wrap.
    std::string line = "{\"type\":\"meta\",";
    line.append(json.data(), json.size());
    line += "}";
    write(line);
}

void Reporter::emit_sample(const Sample& s, std::string_view device) {
    std::string line;
    line.reserve(160);
    line += "{\"type\":\"sample\",\"t_ms\":";
    line += std::to_string(s.t_ms);
    line += ",\"device\":\"";
    line.append(device.data(), device.size());
    line += "\"";
    if (s.gpu_util_pct) { line += ",\"gpu_util\":"; line += std::to_string(*s.gpu_util_pct); }
    if (s.mem_used_mb) { line += ",\"mem_used_mb\":"; line += std::to_string(*s.mem_used_mb); }
    if (s.mem_total_mb) { line += ",\"mem_total_mb\":"; line += std::to_string(*s.mem_total_mb); }
    if (s.power_w) { line += ",\"power_w\":"; line += std::to_string(*s.power_w); }
    if (s.sm_clock_mhz) { line += ",\"sm_clock\":"; line += std::to_string(*s.sm_clock_mhz); }
    if (s.graphics_clock_mhz) { line += ",\"graphics_clock\":"; line += std::to_string(*s.graphics_clock_mhz); }
    if (s.cpu_util_pct) { line += ",\"cpu_util\":"; line += std::to_string(*s.cpu_util_pct); }
    line += "}";
    write(line);
}

void Reporter::emit_tile(int index, double elapsed_ms) {
    char buf[96];
    int n = std::snprintf(buf, sizeof(buf),
                          "{\"type\":\"tile\",\"i\":%d,\"elapsed_ms\":%.4f}", index, elapsed_ms);
    write(std::string_view(buf, n));
}

void Reporter::emit_summary(std::string_view json) {
    std::string line = "{\"type\":\"summary\",";
    line.append(json.data(), json.size());
    line += "}";
    write(line);
}

void Reporter::emit_comparison(double gpu_tps, double cpu_tps) {
    double speedup = cpu_tps > 0.0 ? gpu_tps / cpu_tps : 0.0;
    char buf[128];
    int n = std::snprintf(buf, sizeof(buf),
                          "{\"type\":\"comparison\",\"gpu_tiles_per_s\":%.4f,\"cpu_tiles_per_s\":%.4f,\"speedup\":%.3f}",
                          gpu_tps, cpu_tps, speedup);
    write(std::string_view(buf, n));
}

} // namespace upix::perf
