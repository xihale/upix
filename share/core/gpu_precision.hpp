#pragma once

// Frozen Vulkan precision profiles. Online glslang is replaced by a precompiled
// SPIR-V pack, so Option bits that change shader macros must match the dump
// bit-for-bit. Two profiles cover the devices this app actually runs on:
//
//   Integrated — phones / laptop iGPUs: fp16 packed+storage, fp32 math
//                (Adreno/Mali residual blocks overflow fp16's 65504 range).
//   Discrete   — PC dGPUs: fp16 packed+storage+arith.
//
// use_shader_local_memory stays OFF in BOTH profiles: ncnn's local-memory
// winograd/deconvolution pack4 shaders fail to compile under the frozen dump
// when combined with fp16 arithmetic (16 layer shaders), which leaves holes in
// the pack and crashes the runtime with "binding_count not match". Without
// local memory those convolutions select their gemm variants, which do
// compile. fp16 arithmetic alone only drops convolution1d pack4 variants,
// which no SR model uses.
//
// Cooperative matrix and subgroup ops stay off in both. int8 storage stays on
// (u8 tile queue). Uniform-as-fp16/int8 stays off so a missing VkFeature does
// not desync the pack key.

#include <platform.h>
#include <option.h>
#include <optional>
#include <string_view>
#if NCNN_VULKAN
#include <gpu.h>
#endif

namespace crossscale {

enum class GpuPrecisionProfile {
    Integrated = 0,
    Discrete = 1,
};

struct GpuFeatureCaps {
    bool discrete = false;
    bool mobile_vendor = false;
    bool fp16_packed = false;
    bool fp16_storage = false;
    bool fp16_arithmetic = false;
    bool int8_packed = false;
    bool int8_storage = false;
};

inline uint32_t option_key(const ncnn::Option& opt) {
    uint32_t k = 0;
    k |= static_cast<uint32_t>(opt.use_fp16_packed) << 0;
    k |= static_cast<uint32_t>(opt.use_fp16_storage) << 1;
    k |= static_cast<uint32_t>(opt.use_fp16_arithmetic) << 2;
    k |= static_cast<uint32_t>(opt.use_fp16_uniform) << 3;
    k |= static_cast<uint32_t>(opt.use_int8_packed) << 4;
    k |= static_cast<uint32_t>(opt.use_int8_storage) << 5;
    k |= static_cast<uint32_t>(opt.use_int8_arithmetic) << 6;
    k |= static_cast<uint32_t>(opt.use_int8_uniform) << 7;
    k |= static_cast<uint32_t>(opt.use_shader_local_memory) << 8;
    k |= static_cast<uint32_t>(opt.use_cooperative_matrix) << 9;
    k |= static_cast<uint32_t>(opt.use_subgroup_ops) << 10;
    return k;
}

inline const char* profile_name(GpuPrecisionProfile profile) {
    switch (profile) {
        case GpuPrecisionProfile::Integrated:
            return "integrated";
        case GpuPrecisionProfile::Discrete:
            return "discrete";
    }
    return "unknown";
}

inline void apply_profile(ncnn::Option& opt, GpuPrecisionProfile profile) {
    const bool discrete = profile == GpuPrecisionProfile::Discrete;
    opt.use_fp16_packed = true;
    opt.use_fp16_storage = true;
    opt.use_fp16_arithmetic = discrete;
    opt.use_fp16_uniform = false;
    opt.use_int8_packed = true;
    opt.use_int8_storage = true;
    opt.use_int8_arithmetic = false;
    opt.use_int8_uniform = false;
    opt.use_shader_local_memory = false;
    opt.use_cooperative_matrix = false;
    opt.use_subgroup_ops = false;
}

#if NCNN_VULKAN
inline GpuFeatureCaps gpu_feature_caps(const ncnn::GpuInfo& info) {
    GpuFeatureCaps caps;
    caps.discrete = info.type() == 0;
    const char* name_raw = info.device_name();
    const auto name = name_raw ? std::string_view(name_raw) : std::string_view{};
    caps.mobile_vendor = name.find("Adreno") != std::string_view::npos ||
                         name.find("Mali") != std::string_view::npos ||
                         name.find("PowerVR") != std::string_view::npos ||
                         name.find("Imagination") != std::string_view::npos;
    caps.fp16_packed = info.support_fp16_packed();
    caps.fp16_storage = info.support_fp16_storage();
    caps.fp16_arithmetic = info.support_fp16_arithmetic();
    caps.int8_packed = info.support_int8_packed();
    caps.int8_storage = info.support_int8_storage();
    return caps;
}
#endif

inline bool profile_supported(const GpuFeatureCaps& caps, GpuPrecisionProfile profile) {
    if (!caps.fp16_packed || !caps.fp16_storage || !caps.int8_packed || !caps.int8_storage) {
        return false;
    }
    if (profile == GpuPrecisionProfile::Discrete && !caps.fp16_arithmetic) {
        return false;
    }
    return true;
}

inline std::optional<GpuPrecisionProfile> select_profile(const GpuFeatureCaps& caps,
                                                         bool force_fp16_arith) {
    const bool want_discrete =
        force_fp16_arith || (caps.discrete && !caps.mobile_vendor);
    if (want_discrete && profile_supported(caps, GpuPrecisionProfile::Discrete)) {
        return GpuPrecisionProfile::Discrete;
    }
    if (profile_supported(caps, GpuPrecisionProfile::Integrated)) {
        return GpuPrecisionProfile::Integrated;
    }
    return std::nullopt;
}

// ncnn VulkanDevice packing uops construct a fresh Option with these bits
// (see gpu.cpp get_utility_operator). Dumped as extra pack keys so those
// shaders resolve without glslang.
inline ncnn::Option packing_option(bool use_fp16, bool use_int8) {
    ncnn::Option opt;
    opt.use_vulkan_compute = true;
    opt.use_fp16_packed = use_fp16;
    opt.use_fp16_storage = use_fp16;
    opt.use_fp16_arithmetic = false;
    opt.use_int8_packed = use_int8;
    opt.use_int8_storage = use_int8;
    opt.use_int8_arithmetic = false;
    opt.use_cooperative_matrix = false;
    return opt;
}

} // namespace crossscale
