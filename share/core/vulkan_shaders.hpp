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
#include <mutex>

#if NCNN_VULKAN
#include <gpu.h>
#endif

namespace crossscale::detail {

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

inline constexpr const char* kHwcToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = (gy * p.w + gx) * p.channels + gz;
    const int dst_idx = gz * p.cstep + gy * p.w + gx;
    dst_data[dst_idx] = sfp(src_data[src_idx]);
}
)";

inline constexpr const char* kHwcU8ToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

#if NCNN_int8_storage
layout (binding = 0) readonly buffer src_blob { uint8_t src_data[]; };
#else
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
#endif
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = (gy * p.w + gx) * p.channels + gz;
    const int dst_idx = gz * p.cstep + gy * p.w + gx;
#if NCNN_int8_storage
    dst_data[dst_idx] = sfp(float(uint(src_data[src_idx])) * (1.0 / 255.0));
#else
    dst_data[dst_idx] = sfp(src_data[src_idx] * (1.0 / 255.0));
#endif
}
)";

inline constexpr const char* kHwcCropToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

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
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.tile_w || gy >= p.tile_h || gz >= p.channels)
        return;

    int src_x = p.tile_x + gx;
    int src_y = p.tile_y + gy;
    src_x = abs(src_x);
    src_y = abs(src_y);
    src_x = (p.src_w - 1) - abs(src_x - (p.src_w - 1));
    src_y = (p.src_h - 1) - abs(src_y - (p.src_h - 1));

    const int src_idx = (src_y * p.src_w + src_x) * p.channels + gz;
    const int dst_idx = gz * p.dst_cstep + gy * p.tile_w + gx;
    dst_data[dst_idx] = sfp(src_data[src_idx]);
}
)";

inline constexpr const char* kHwcCropU8ToChwComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

#if NCNN_int8_storage
layout (binding = 0) readonly buffer src_blob { uint8_t src_data[]; };
#else
layout (binding = 0) readonly buffer src_blob { float src_data[]; };
#endif
layout (binding = 1) writeonly buffer dst_blob { sfp dst_data[]; };

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
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.tile_w || gy >= p.tile_h || gz >= p.channels)
        return;

    int src_x = p.tile_x + gx;
    int src_y = p.tile_y + gy;
    src_x = abs(src_x);
    src_y = abs(src_y);
    src_x = (p.src_w - 1) - abs(src_x - (p.src_w - 1));
    src_y = (p.src_h - 1) - abs(src_y - (p.src_h - 1));

    const int src_idx = (src_y * p.src_w + src_x) * p.channels + gz;
    const int dst_idx = gz * p.dst_cstep + gy * p.tile_w + gx;
#if NCNN_int8_storage
    dst_data[dst_idx] = sfp(float(uint(src_data[src_idx])) * (1.0 / 255.0));
#else
    dst_data[dst_idx] = sfp(src_data[src_idx] * (1.0 / 255.0));
#endif
}
)";

inline constexpr const char* kChwToHwcComp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = gz * p.cstep + gy * p.w + gx;
    const int dst_idx = (gy * p.w + gx) * p.channels + gz;
    dst_data[dst_idx] = float(src_data[src_idx]);
}
)";

inline constexpr const char* kChwToHwcU8Comp = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
#if NCNN_int8_storage
layout (binding = 1) writeonly buffer dst_blob { uint8_t dst_data[]; };
#else
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };
#endif

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int channels;
    int cstep;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    const int gy = int(gl_GlobalInvocationID.y);
    const int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.channels)
        return;

    const int src_idx = gz * p.cstep + gy * p.w + gx;
    const int dst_idx = (gy * p.w + gx) * p.channels + gz;
    const float v = clamp(float(src_data[src_idx]) * 255.0, 0.0, 255.0);
#if NCNN_int8_storage
    const uint v32 = clamp(uint(floor(v + 0.5)), 0u, 255u);
    dst_data[dst_idx] = uint8_t(v32);
#else
    dst_data[dst_idx] = v;
#endif
}
)";

inline constexpr const char* kHwcFloatToU8Comp = R"(
#version 450

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
#if NCNN_int8_storage
layout (binding = 1) writeonly buffer dst_blob { uint8_t dst_data[]; };
#else
layout (binding = 1) writeonly buffer dst_blob { float dst_data[]; };
#endif

layout (push_constant) uniform parameter
{
    int total;
} p;

void main()
{
    const int gx = int(gl_GlobalInvocationID.x);
    if (gx >= p.total)
        return;

    const float v = clamp(src_data[gx] * 255.0, 0.0, 255.0);
#if NCNN_int8_storage
    const uint v32 = clamp(uint(floor(v + 0.5)), 0u, 255u);
    dst_data[gx] = uint8_t(v32);
#else
    dst_data[gx] = v;
#endif
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

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#define sfp float16_t
#else
#define sfp float
#endif

layout (binding = 0) readonly buffer src_blob { sfp src_data[]; };
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
    dst_data[dst_idx] += float(src_data[src_idx]) * w;
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

#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif

layout (binding = 0) readonly buffer src_blob { float src_data[]; };
layout (binding = 1) readonly buffer weight_blob { float weight_data[]; };
#if NCNN_int8_storage
layout (binding = 2) writeonly buffer dst_blob { uint8_t dst_data[]; };
#else
layout (binding = 2) writeonly buffer dst_blob { float dst_data[]; };
#endif

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
#if NCNN_int8_storage
        dst_data[dst_idx] = uint8_t(0u);
#else
        dst_data[dst_idx] = 0.0;
#endif
        return;
    }

    const float inv_w = 1.0 / w;
    const float normalized = src_data[dst_idx] * inv_w;
    const float v = clamp(normalized * 255.0, 0.0, 255.0);
#if NCNN_int8_storage
    const uint v32 = clamp(uint(floor(v + 0.5)), 0u, 255u);
    dst_data[dst_idx] = uint8_t(v32);
#else
    dst_data[dst_idx] = v;
#endif
}
)";

#endif // NCNN_VULKAN

} // namespace crossscale::detail
