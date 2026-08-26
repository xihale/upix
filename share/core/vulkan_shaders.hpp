#pragma once

// Vulkan compute shader sources + the VkAllocGuard RAII helper, shared by
// ncnn_engine.cpp, ncnn_gpu_init.cpp and ncnn_gpu_pipeline.cpp.
//
// ncnn/platform.h defines NCNN_VULKAN; include it first so the guard below is
// honored regardless of include order in the including .cpp. The VkAllocGuard
// destructor calls into ncnn::VulkanDevice, so <gpu.h> (complete type) is
// required when Vulkan is enabled. Inside the shaders, NCNN_fp16_storage /
// NCNN_int8_storage are ncnn-provided macros substituted at VkComp pipeline
// creation time; they are intentionally left untouched here.

#include <platform.h>
#include <cstdint>
#include <mutex>

#if NCNN_VULKAN
#include <gpu.h>
#endif

namespace upix::detail {

#if NCNN_VULKAN

// RAII wrapper that returns blob/staging allocators to their device on scope
// exit. Used by every Vulkan code path that acquires allocators for a tile pass.
struct VkAllocGuard {
    ncnn::VulkanDevice* dev = nullptr;
    ncnn::VkAllocator* blob = nullptr;
    ncnn::VkAllocator* staging = nullptr;
    ~VkAllocGuard() {
        if (dev != nullptr) {
            // Staging buffers are full-frame host-mapped coherent memory that
            // ncnn caches by size bucket and never shrinks; without this the
            // largest frame ever staged stays resident (and mapped) for the
            // process lifetime. Clearing here — submits are done and the
            // allocator is exclusively ours until reclaim — keeps per-image
            // RSS flat. Blob buffers are VRAM-backed on dGPU and worth
            // reusing across images, so they are kept.
            if (staging) staging->clear();
            if (blob) dev->reclaim_blob_allocator(blob);
            if (staging) dev->reclaim_staging_allocator(staging);
        }
    }
};

// Small-type SSBO policy (Adreno workaround):
// Some mobile drivers (Adreno 7xx class) silently fail to execute compute
// shaders whose storage-buffer blocks declare SCALAR float16_t / uint8_t
// members, even though the device advertises the corresponding extensions
// and features. Packed vectors (sfpvec4) and plain ints/floats work. All
// custom kernels therefore treat sub-32-bit payloads as plain uint words
// with explicit lane conversion — core GLSL only (unpackHalf2x16 /
// packHalf2x16 / byte shifts), no extensions, no alignment assumptions.
//
// Write side ownership rule: every invocation stores WHOLE words, partitioned
// over absolute buffer elements (not rows/channels), so two invocations never
// split one word. Padding holes (cstep slack, odd tails) are written zero /
// replicated and are never consumed by the net or the download path.
//
// The fp16/u8 payload formats below are fixed by the host pipeline on BOTH
// frozen precision profiles (both enable fp16+int8 storage), so the former
// NCNN_fp16_storage / NCNN_int8_storage source variants collapsed into one.

inline constexpr const char* kHwcToChwComp = R"(
#version 450

// HWC fp32 -> CHW fp16-as-u32-words. One invocation owns one output word =
// two consecutive CHW halves, mapped over absolute elements so padding and
// odd sizes stay race-free. Dispatch: 1D over (channels*cstep+1)/2.
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int q = int(gl_GlobalInvocationID.x);
    const int plane = p.w * p.h;
    const int total = p.channels * p.cstep;

    uint bits0 = 0u;
    uint bits1 = 0u;
    const int e0 = q * 2;
    const int e1 = e0 + 1;
    if (e0 < total) {
        // Within a CHW plane the flat offset IS the HWC pixel index.
        const int c0 = e0 / p.cstep;
        const int off0 = e0 - c0 * p.cstep;
        if (off0 < plane) {
            bits0 = packHalf2x16(vec2(src_data[off0 * p.channels + c0], 0.0));
        }
        if (e1 < total) {
            const int c1 = e1 / p.cstep;
            const int off1 = e1 - c1 * p.cstep;
            if (off1 < plane) {
                bits1 = packHalf2x16(vec2(src_data[off1 * p.channels + c1], 0.0));
            }
        }
        dst_data[q] = bits0 | (bits1 << 16);
    }
}
)";

inline constexpr const char* kHwcU8ToChwComp = R"(
#version 450

// HWC u8-as-u32-words -> CHW fp16-as-u32-words. Same absolute-word ownership
// as kHwcToChwComp. Dispatch: 1D over (channels*cstep+1)/2.
layout (binding = 0) readonly buffer src_blob { uint src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int q = int(gl_GlobalInvocationID.x);
    const int plane = p.w * p.h;
    const int total = p.channels * p.cstep;

    uint bits0 = 0u;
    uint bits1 = 0u;
    const int e0 = q * 2;
    const int e1 = e0 + 1;
    if (e0 < total) {
        const int c0 = e0 / p.cstep;
        const int off0 = e0 - c0 * p.cstep;
        if (off0 < plane) {
            const int s0 = off0 * p.channels + c0;
            const uint b0 = (src_data[s0 >> 2] >> ((s0 & 3) * 8)) & 0xFFu;
            bits0 = packHalf2x16(vec2(float(b0) * (1.0 / 255.0), 0.0));
        }
        if (e1 < total) {
            const int c1 = e1 / p.cstep;
            const int off1 = e1 - c1 * p.cstep;
            if (off1 < plane) {
                const int s1 = off1 * p.channels + c1;
                const uint b1 = (src_data[s1 >> 2] >> ((s1 & 3) * 8)) & 0xFFu;
                bits1 = packHalf2x16(vec2(float(b1) * (1.0 / 255.0), 0.0));
            }
        }
        dst_data[q] = bits0 | (bits1 << 16);
    }
}
)";

inline constexpr const char* kHwcCropToChwComp = R"(
#version 450

// HWC fp32 crop (+replicate/mirror border) -> CHW fp16-as-u32-words.
// Absolute-word ownership over the padded tile planes; padding holes are
// zeroed. Dispatch: 1D over (channels*dst_cstep+1)/2.
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int src_w;
    int src_h;
    int channels;
    int tile_x;
    int tile_y;
    int tile_w;
    int tile_h;
    int dst_cstep;
    int border_replicate;
} p;

int gather(int c, int off)
{
    const int plane = p.tile_w * p.tile_h;
    if (off >= plane) {
        return -1;
    }
    int src_x = p.tile_x + off % p.tile_w;
    int src_y = p.tile_y + off / p.tile_w;
    if (p.border_replicate != 0)
    {
        // Pre-padding models: out-of-image context must replicate the edge
        // pixel (BORDER_REPLICATE), matching copy_make_border / the
        // whole-image path, not mirror it.
        src_x = clamp(src_x, 0, p.src_w - 1);
        src_y = clamp(src_y, 0, p.src_h - 1);
    }
    else
    {
        src_x = abs(src_x);
        src_y = abs(src_y);
        src_x = (p.src_w - 1) - abs(src_x - (p.src_w - 1));
        src_y = (p.src_h - 1) - abs(src_y - (p.src_h - 1));
    }
    return (src_y * p.src_w + src_x) * p.channels + c;
}

void main()
{
    const int q = int(gl_GlobalInvocationID.x);
    const int total = p.channels * p.dst_cstep;

    uint bits0 = 0u;
    uint bits1 = 0u;
    const int e0 = q * 2;
    if (e0 < total) {
        const int c0 = e0 / p.dst_cstep;
        const int s0 = gather(c0, e0 - c0 * p.dst_cstep);
        if (s0 >= 0) {
            bits0 = packHalf2x16(vec2(src_data[s0], 0.0));
        }
        const int e1 = e0 + 1;
        if (e1 < total) {
            const int c1 = e1 / p.dst_cstep;
            const int s1 = gather(c1, e1 - c1 * p.dst_cstep);
            if (s1 >= 0) {
                bits1 = packHalf2x16(vec2(src_data[s1], 0.0));
            }
        }
        dst_data[q] = bits0 | (bits1 << 16);
    }
}
)";

inline constexpr const char* kHwcCropU8ToChwComp = R"(
#version 450

// HWC u8-as-u32-words crop -> CHW fp16-as-u32-words.
// Dispatch: 1D over (channels*dst_cstep+1)/2.
layout (binding = 0) readonly buffer src_blob { uint src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int src_w;
    int src_h;
    int channels;
    int tile_x;
    int tile_y;
    int tile_w;
    int tile_h;
    int dst_cstep;
    int border_replicate;
} p;

int gather_byte(int c, int off)
{
    const int plane = p.tile_w * p.tile_h;
    if (off >= plane) {
        return -1;
    }
    int src_x = p.tile_x + off % p.tile_w;
    int src_y = p.tile_y + off / p.tile_w;
    if (p.border_replicate != 0)
    {
        src_x = clamp(src_x, 0, p.src_w - 1);
        src_y = clamp(src_y, 0, p.src_h - 1);
    }
    else
    {
        src_x = abs(src_x);
        src_y = abs(src_y);
        src_x = (p.src_w - 1) - abs(src_x - (p.src_w - 1));
        src_y = (p.src_h - 1) - abs(src_y - (p.src_h - 1));
    }
    return (src_y * p.src_w + src_x) * p.channels + c;
}

void main()
{
    const int q = int(gl_GlobalInvocationID.x);
    const int total = p.channels * p.dst_cstep;

    uint bits0 = 0u;
    uint bits1 = 0u;
    const int e0 = q * 2;
    if (e0 < total) {
        const int c0 = e0 / p.dst_cstep;
        const int s0 = gather_byte(c0, e0 - c0 * p.dst_cstep);
        if (s0 >= 0) {
            const uint b0 = (src_data[s0 >> 2] >> ((s0 & 3) * 8)) & 0xFFu;
            bits0 = packHalf2x16(vec2(float(b0) * (1.0 / 255.0), 0.0));
        }
        const int e1 = e0 + 1;
        if (e1 < total) {
            const int c1 = e1 / p.dst_cstep;
            const int s1 = gather_byte(c1, e1 - c1 * p.dst_cstep);
            if (s1 >= 0) {
                const uint b1 = (src_data[s1 >> 2] >> ((s1 & 3) * 8)) & 0xFFu;
                bits1 = packHalf2x16(vec2(float(b1) * (1.0 / 255.0), 0.0));
            }
        }
        dst_data[q] = bits0 | (bits1 << 16);
    }
}
)";

inline constexpr const char* kChwToHwcComp = R"(
#version 450

// CHW fp16-as-u32-words -> HWC fp32. Read-only lane extraction; dispatch
// grid unchanged (w, h, channels).
layout (binding = 0) readonly buffer src_blob { uint src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

float load_half(int e)
{
    const uint bits = (e & 1) == 0 ? (src_data[e >> 1] & 0xFFFFu)
                                   : (src_data[e >> 1] >> 16);
    return unpackHalf2x16(bits).x;
}

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = gz * p.cstep + gy * p.w + gx;
    const int dst_idx = (gy * p.w + gx) * p.channels + gz;
    dst_data[dst_idx] = load_half(src_idx);
}
)";

inline constexpr const char* kChwToHwcU8Comp = R"(
#version 450

// CHW fp16-as-u32-words -> HWC u8-as-u32-words. One invocation owns one
// output word = four consecutive HWC bytes. Dispatch: 1D over
// (w*h*channels+3)/4.
layout (binding = 0) readonly buffer src_blob { uint src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int q = int(gl_GlobalInvocationID.x);
    const int total_bytes = p.w * p.h * p.channels;

    uint word = 0u;
    for (int j = 3; j >= 0; --j) {
        const int e = q * 4 + j;
        if (e >= total_bytes) {
            continue;
        }
        const int px = e / p.channels;
        const int c = e - px * p.channels;
        const int src_idx = c * p.cstep + px;
        const uint bits = (src_idx & 1) == 0 ? (src_data[src_idx >> 1] & 0xFFFFu)
                                             : (src_data[src_idx >> 1] >> 16);
        const float v = clamp(unpackHalf2x16(bits).x * 255.0, 0.0, 255.0);
        word |= clamp(uint(floor(v + 0.5)), 0u, 255u) << (j * 8);
    }
    dst_data[q] = word;
}
)";

inline constexpr const char* kHwcFloatToU8Comp = R"(
#version 450

// HWC fp32 -> HWC u8-as-u32-words. One invocation owns four consecutive
// output bytes. Dispatch: 1D over (total+3)/4.
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int total;
} p;

void main()
{
    const int q = int(gl_GlobalInvocationID.x);

    uint word = 0u;
    for (int j = 3; j >= 0; --j) {
        const int e = q * 4 + j;
        if (e >= p.total) {
            continue;
        }
        const float v = clamp(src_data[e] * 255.0, 0.0, 255.0);
        word |= clamp(uint(floor(v + 0.5)), 0u, 255u) << (j * 8);
    }
    dst_data[q] = word;
}
)";

inline constexpr const char* kClearComp = R"(
#version 450

layout (binding = 0) writeonly buffer dst_blob { float dst_data[]; };

layout (push_constant) uniform parameter
{
    int total;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    if (gx >= p.total)
        return;
    dst_data[gx] = 0.0;
}
)";

inline constexpr const char* kBlendTileComp = R"(
#version 450

// Net tile output CHW fp16-as-u32-words -> fp32 accumulation. Read-only lane
// extraction; dispatch grid unchanged.
layout (binding = 0) readonly buffer src_blob { uint src_data[]; };
layout (binding = 1) buffer dst_blob { float dst_data[]; };
layout (binding = 2) buffer weight_blob { float weight_data[]; };

layout (push_constant) uniform parameter
{
    int tile_w;
    int tile_h;
    int channels;
    int out_w;
    int out_h;
    int out_x;
    int out_y;
    int overlap;
    int src_w;
    int src_h;
    int src_offset_x;
    int src_offset_y;
    int src_cstep;
} p;

float blend_weight(int pos, int size, int overlap, int tile_start, int full_size)
{
    const int effective_overlap = min(overlap, size);
    if (effective_overlap <= 0)
        return 1.0;
    float w = 1.0;
    if (tile_start > 0 && pos < effective_overlap)
        w = float(pos + 1) / float(effective_overlap);
    else if (tile_start + size < full_size && pos >= size - effective_overlap)
        w = float(size - pos) / float(effective_overlap);
    return w * w;
}

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.tile_w || gy >= p.tile_h || gz >= p.channels)
        return;

    const int out_px = p.out_x + gx;
    const int out_py = p.out_y + gy;
    if (out_px < 0 || out_px >= p.out_w || out_py < 0 || out_py >= p.out_h)
        return;

    const float wx = blend_weight(gx, p.tile_w, p.overlap, p.out_x, p.out_w);
    const float wy = blend_weight(gy, p.tile_h, p.overlap, p.out_y, p.out_h);
    const float w = wx * wy;

    const int src_x = p.src_offset_x + gx;
    const int src_y = p.src_offset_y + gy;
    if (src_x < 0 || src_x >= p.src_w || src_y < 0 || src_y >= p.src_h)
        return;

    const int src_xy_idx = src_y * p.src_w + src_x;
    const int src_idx = gz * p.src_cstep + src_xy_idx;
    const int dst_idx = (out_py * p.out_w + out_px) * p.channels + gz;
    const uint bits = (src_idx & 1) == 0 ? (src_data[src_idx >> 1] & 0xFFFFu)
                                         : (src_data[src_idx >> 1] >> 16);
    dst_data[dst_idx] += unpackHalf2x16(bits).x * w;
    if (gz == 0) {
        const int weight_idx = out_py * p.out_w + out_px;
        weight_data[weight_idx] += w;
    }
}
)";

inline constexpr const char* kNormalizeByWeightComp = R"(
#version 450

layout (binding = 0) buffer dst_blob { float dst_data[]; };
layout (binding = 1) readonly buffer weight_blob { float weight_data[]; };

layout (push_constant) uniform parameter
{
    int total_pixels;
    int channels;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.total_pixels || gz >= p.channels)
        return;

    const float w = weight_data[gx];
    const int dst_idx = gx * p.channels + gz;
    if (w <= 1e-6) {
        dst_data[dst_idx] = 0.0;
        return;
    }

    const float inv_w = 1.0 / w;
    dst_data[dst_idx] = dst_data[dst_idx] * inv_w;
}
)";

inline constexpr const char* kNormalizeQuantizeByWeightComp = R"(
#version 450

// HWC fp32 accumulation + per-pixel weight -> HWC u8-as-u32-words.
// One invocation owns four consecutive output bytes. Dispatch: 1D over
// (total_pixels*channels+3)/4.
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) readonly buffer weight_blob { float weight_data[]; };
layout (binding = 2) writeonly buffer dst_blob { uint dst_data[]; };

layout (push_constant) uniform parameter
{
    int total_pixels;
    int channels;
} p;

void main()
{
    const int q = int(gl_GlobalInvocationID.x);
    const int total_bytes = p.total_pixels * p.channels;

    uint word = 0u;
    for (int j = 3; j >= 0; --j) {
        const int e = q * 4 + j;
        if (e >= total_bytes) {
            continue;
        }
        const int px = e / p.channels;
        const float w = weight_data[px];
        uint b = 0u;
        if (w > 1e-6) {
            const float normalized = src_data[e] / w;
            const float v = clamp(normalized * 255.0, 0.0, 255.0);
            b = clamp(uint(floor(v + 0.5)), 0u, 255u);
        }
        word |= b << (j * 8);
    }
    dst_data[q] = word;
}
)";

enum class CustomShader : uint32_t {
    HwcToChw = 1,
    HwcU8ToChw = 2,
    HwcCropToChw = 3,
    HwcCropU8ToChw = 4,
    ChwToHwc = 5,
    ChwToHwcU8 = 6,
    HwcFloatToU8 = 7,
    Clear = 8,
    BlendTile = 9,
    NormalizeByWeight = 10,
    NormalizeQuantizeByWeight = 11,
};

// Diagnostic micro-kernels (scalar float16_t SSBO bisect).
struct CustomShaderSrc {
    CustomShader id;
    const char* name;
    const char* source;
};

inline constexpr CustomShaderSrc kCustomShaders[] = {
    {CustomShader::HwcToChw, "hwc_to_chw", kHwcToChwComp},
    {CustomShader::HwcU8ToChw, "hwc_u8_to_chw", kHwcU8ToChwComp},
    {CustomShader::HwcCropToChw, "hwc_crop_to_chw", kHwcCropToChwComp},
    {CustomShader::HwcCropU8ToChw, "hwc_crop_u8_to_chw", kHwcCropU8ToChwComp},
    {CustomShader::ChwToHwc, "chw_to_hwc", kChwToHwcComp},
    {CustomShader::ChwToHwcU8, "chw_to_hwc_u8", kChwToHwcU8Comp},
    {CustomShader::HwcFloatToU8, "hwc_float_to_u8", kHwcFloatToU8Comp},
    {CustomShader::Clear, "clear", kClearComp},
    {CustomShader::BlendTile, "blend_tile", kBlendTileComp},
    {CustomShader::NormalizeByWeight, "normalize_by_weight", kNormalizeByWeightComp},
    {CustomShader::NormalizeQuantizeByWeight, "normalize_quantize_by_weight",
     kNormalizeQuantizeByWeightComp},
};

#endif // NCNN_VULKAN

} // namespace upix::detail
