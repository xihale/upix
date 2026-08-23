#pragma once

// Precompiled SPIR-V pack for frozen GPU Option profiles. Lives next to the
// engine, not next to .param/.bin models — kernels are engine-wide.

#include "gpu_precision.hpp"
#include "vulkan_shaders.hpp"
#include <cstdint>
#include <span>
#include <vector>

#if NCNN_VULKAN

namespace crossscale::spirv_pack {

constexpr uint32_t kKindLayer = 0;
constexpr uint32_t kKindCustom = 1;

bool load_embedded();
std::span<const uint32_t> find(uint32_t kind, uint32_t id, uint32_t opt_key);
bool copy(uint32_t kind, uint32_t id, uint32_t opt_key, std::vector<uint32_t>& out);

inline std::span<const uint32_t> find_custom(detail::CustomShader id, uint32_t opt_key) {
    return find(kKindCustom, static_cast<uint32_t>(id), opt_key);
}

inline bool copy_custom(detail::CustomShader id, uint32_t opt_key, std::vector<uint32_t>& out) {
    return copy(kKindCustom, static_cast<uint32_t>(id), opt_key, out);
}

// Try pack first, then ncnn::compile_spirv_module (no-op when glslang is unlinked).
bool load_or_compile_custom(detail::CustomShader id, const char* src, const ncnn::Option& opt,
                            std::vector<uint32_t>& out);

void register_ncnn_lookup();

} // namespace crossscale::spirv_pack

#endif // NCNN_VULKAN
