#include "perf/probes.hpp"

#include <dlfcn.h>

namespace crossscale::perf {

// ---- TimerProbe ----

auto TimerProbe::start() -> bool {
    start_time_ = std::chrono::high_resolution_clock::now();
    tic_time_ = start_time_;
    started_ = true;
    return true;
}

auto TimerProbe::sample() -> Sample {
    Sample s;
    if (started_) {
        s.t_ms = std::chrono::duration<double, std::milli>(
                     std::chrono::high_resolution_clock::now() - start_time_)
                     .count();
    }
    return s;
}

void TimerProbe::tic() {
    tic_time_ = std::chrono::high_resolution_clock::now();
}

auto TimerProbe::toc() -> double {
    return std::chrono::duration<double, std::milli>(
               std::chrono::high_resolution_clock::now() - tic_time_)
        .count();
}

auto TimerProbe::toc_reset() -> double {
    auto now = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - tic_time_).count();
    tic_time_ = now;
    return ms;
}

// ---- NvmlProbe (dlopen) ----
// NVML types and function pointer signatures. We define our own opaque types to
// avoid depending on nvidia-ml dev headers (which aren't installed on this box).
namespace {

using nvmlDevice_t = struct nvmlDevice_st*;  // opaque

// Minimal NVML C API subset we use, declared as function-pointer typedefs so we
// can resolve them via dlsym at runtime.
using nvmlInit_t = int (*)(void);
using nvmlShutdown_t = int (*)(void);
using nvmlDeviceGetCount_t = int (*)(unsigned*);
using nvmlDeviceGetHandleByIndex_t = int (*)(unsigned, nvmlDevice_t*);
using nvmlDeviceGetUtilizationRates_t = int (*)(nvmlDevice_t, struct nvmlUtilization_st*);
using nvmlDeviceGetMemoryInfo_t = int (*)(nvmlDevice_t, struct nvmlMemory_st*);
using nvmlDeviceGetPowerUsage_t = int (*)(nvmlDevice_t, unsigned*);
using nvmlDeviceGetClockInfo_t = int (*)(nvmlDevice_t, unsigned, unsigned*);
using nvmlDeviceGetName_t = int (*)(nvmlDevice_t, char*, unsigned);

// NVML struct layouts (must match the library's ABI; these are stable).
struct nvmlUtilization_st { unsigned gpu, memory; };
struct nvmlMemory_st { unsigned long long total, used, free; };

// NVML clock type enum values (from nvml.h).
constexpr unsigned NVML_CLOCK_SM = 0;
constexpr unsigned NVML_CLOCK_GRAPHICS = 1;

struct NvmlApi {
    void* lib = nullptr;
    nvmlInit_t init = nullptr;
    nvmlShutdown_t shutdown = nullptr;
    nvmlDeviceGetCount_t get_count = nullptr;
    nvmlDeviceGetHandleByIndex_t get_handle = nullptr;
    nvmlDeviceGetUtilizationRates_t get_util = nullptr;
    nvmlDeviceGetMemoryInfo_t get_mem = nullptr;
    nvmlDeviceGetPowerUsage_t get_power = nullptr;
    nvmlDeviceGetClockInfo_t get_clock = nullptr;
    nvmlDeviceGetName_t get_name = nullptr;
};

auto load_nvml() -> NvmlApi {
    NvmlApi api;
    // Try the SONAME first (always present with the driver), then the dev symlink.
    api.lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api.lib) api.lib = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
    if (!api.lib) return api;

    api.init = reinterpret_cast<nvmlInit_t>(dlsym(api.lib, "nvmlInit_v2"));
    if (!api.init) api.init = reinterpret_cast<nvmlInit_t>(dlsym(api.lib, "nvmlInit"));
    api.shutdown = reinterpret_cast<nvmlShutdown_t>(dlsym(api.lib, "nvmlShutdown"));
    api.get_count = reinterpret_cast<nvmlDeviceGetCount_t>(dlsym(api.lib, "nvmlDeviceGetCount_v2"));
    if (!api.get_count) api.get_count = reinterpret_cast<nvmlDeviceGetCount_t>(dlsym(api.lib, "nvmlDeviceGetCount"));
    api.get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(dlsym(api.lib, "nvmlDeviceGetHandleByIndex_v2"));
    if (!api.get_handle) api.get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(dlsym(api.lib, "nvmlDeviceGetHandleByIndex"));
    api.get_util = reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(dlsym(api.lib, "nvmlDeviceGetUtilizationRates"));
    api.get_mem = reinterpret_cast<nvmlDeviceGetMemoryInfo_t>(dlsym(api.lib, "nvmlDeviceGetMemoryInfo"));
    api.get_power = reinterpret_cast<nvmlDeviceGetPowerUsage_t>(dlsym(api.lib, "nvmlDeviceGetPowerUsage"));
    api.get_clock = reinterpret_cast<nvmlDeviceGetClockInfo_t>(dlsym(api.lib, "nvmlDeviceGetClockInfo"));
    api.get_name = reinterpret_cast<nvmlDeviceGetName_t>(dlsym(api.lib, "nvmlDeviceGetName"));

    // Core symbols must resolve or we treat NVML as unusable. get_name is optional.
    if (!api.init || !api.shutdown || !api.get_count || !api.get_handle ||
        !api.get_util || !api.get_mem || !api.get_power || !api.get_clock) {
        dlclose(api.lib);
        api.lib = nullptr;
    }
    return api;
}

} // namespace

auto NvmlProbe::start() -> bool {
    NvmlApi* api = new NvmlApi(load_nvml());
    if (!api->lib) { delete api; return false; }

    if (api->init() != 0) {  // NVML_ERROR_*
        dlclose(api->lib);
        delete api;
        return false;
    }

    unsigned count = 0;
    if (api->get_count(&count) != 0 || count == 0) {
        api->shutdown();
        dlclose(api->lib);
        delete api;
        return false;
    }

    nvmlDevice_t dev = nullptr;
    if (api->get_handle(0, &dev) != 0) {
        api->shutdown();
        dlclose(api->lib);
        delete api;
        return false;
    }

    lib_handle_ = api->lib;
    nvml_handle_ = dev;
    api_handle_ = api;  // owned by this probe until stop()
    start_time_ = std::chrono::high_resolution_clock::now();
    initialized_ = true;
    return true;
}

auto NvmlProbe::sample() -> Sample {
    Sample s;
    s.t_ms = std::chrono::duration<double, std::milli>(
                 std::chrono::high_resolution_clock::now() - start_time_)
                 .count();
    if (!initialized_ || !api_handle_) return s;

    auto* api = static_cast<NvmlApi*>(api_handle_);
    auto* dev = static_cast<nvmlDevice_t>(nvml_handle_);

    nvmlUtilization_st util{};
    if (api->get_util(dev, &util) == 0) s.gpu_util_pct = util.gpu;

    nvmlMemory_st mem{};
    if (api->get_mem(dev, &mem) == 0) {
        s.mem_used_mb = static_cast<unsigned>(mem.used >> 20);
        s.mem_total_mb = static_cast<unsigned>(mem.total >> 20);
    }

    unsigned power_mw = 0;
    if (api->get_power(dev, &power_mw) == 0) s.power_w = power_mw / 1000;

    unsigned sm_clk = 0;
    if (api->get_clock(dev, NVML_CLOCK_SM, &sm_clk) == 0) s.sm_clock_mhz = sm_clk;

    unsigned gr_clk = 0;
    if (api->get_clock(dev, NVML_CLOCK_GRAPHICS, &gr_clk) == 0) s.graphics_clock_mhz = gr_clk;

    return s;
}

void NvmlProbe::stop() {
    if (initialized_ && api_handle_) {
        auto* api = static_cast<NvmlApi*>(api_handle_);
        api->shutdown();
        if (api->lib) dlclose(api->lib);  // NvmlApi is POD; no dtor, close manually
    }
    delete static_cast<NvmlApi*>(api_handle_);
    api_handle_ = nullptr;
    lib_handle_ = nullptr;
    nvml_handle_ = nullptr;
    initialized_ = false;
}

auto NvmlProbe::device_name() const -> std::string {
    if (!initialized_ || !api_handle_) return {};
    auto* api = static_cast<NvmlApi*>(api_handle_);
    if (!api->get_name) return {};
    auto* dev = static_cast<nvmlDevice_t>(nvml_handle_);
    char buf[96] = {0};
    if (api->get_name(dev, buf, sizeof(buf)) == 0) return std::string(buf);
    return {};
}

} // namespace crossscale::perf
