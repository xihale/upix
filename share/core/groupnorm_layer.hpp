#pragma once

// Vulkan-capable GroupNorm registered as an overwrite of ncnn's builtin
// CPU-only GroupNorm type. Upstream ncnn ships no groupnorm_vulkan: a GPU
// graph containing GroupNorm drains the whole queue (download, CPU pass,
// upload) once per layer — 28 times per tile on RealPLKSR — which is why
// that family idles at ~30% GPU utilization. With this override the layer
// stays on the device end to end (share/core/vulkan_shaders.hpp kGroupNormComp).
//
// The class must implement the CPU path too: the overwrite registry replaces
// the builtin in every net this process creates, including CPU-only engines.
// The CPU forward mirrors ncnn's builtin groupnorm.cpp math (two-pass
// mean/variance per group) so CPU output stays identical to the builtin.
//
// Concurrency: one ncnn::Net is driven by one worker at a time (engine_pool
// gives every worker its own engine), so the per-layer stats workspace can be
// a plain member reused across forwards. It must outlive the recorded
// command, which may still be executing when forward() returns under the
// engine's pipelined submit — a local would return its block to the
// workspace pool with the buffer bindings untracked (ncnn keeps only
// images/clones/uploads alive until submission).

#include "core/spirv_pack.hpp"
#include "core/vulkan_shaders.hpp"

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <layer.h>
#include <mat.h>
#include <net.h>
#include <option.h>
#include <paramdict.h>

#if NCNN_VULKAN
#include <command.h>
#include <gpu.h>
#include <pipeline.h>
#endif

namespace upix::detail {

class GroupNormLayer : public ncnn::Layer {
public:
    GroupNormLayer() {
        one_blob_only = true;
        support_inplace = true;
        // Globally replacing a builtin layer means CPU-mode engines run this
        // class as well, so Vulkan stays opt-in per net option.
#if NCNN_VULKAN
        support_vulkan = true;
        // Kernel owns the Adreno-safe elempack-1 word format; ncnn inserts
        // pack/unpack conversions on both sides.
        support_vulkan_packing = false;
#endif
    }

    int load_param(const ncnn::ParamDict& pd) override {
        group_ = pd.get(0, 1);
        channels_ = pd.get(1, 0);
        eps_ = pd.get(2, 0.001f);
        affine_ = pd.get(3, 1);
        return 0;
    }

    int load_model(const ncnn::ModelBin& mb) override {
        if (affine_ == 0) return 0;
        gamma_ = mb.load(channels_, 1);
        if (gamma_.empty()) return -100;
        beta_ = mb.load(channels_, 1);
        if (beta_.empty()) return -100;
        return 0;
    }

    // CPU path — two-pass mean/variance per group, same as the builtin. The
    // engine TU builds without OpenMP (ncnn's simpleomp lives inside the
    // ncnn package), so this stays serial; the CPU engine path for RealPLKSR
    // is dominated by the convs around the norm anyway.
    int forward_inplace(ncnn::Mat& bottom_top_blob, const ncnn::Option& /*opt*/) const override {
        const int channels_g = channels_ / group_;

        if (bottom_top_blob.dims == 1) {
            for (int g = 0; g < group_; g++) {
                groupnorm(static_cast<float*>(bottom_top_blob.range(g * channels_g, channels_g).data),
                          affine_ ? (const float*)gamma_ + g * channels_g : nullptr,
                          affine_ ? (const float*)beta_ + g * channels_g : nullptr,
                          eps_, channels_g, 1, 1);
            }
            return 0;
        }
        if (bottom_top_blob.dims == 2) {
            const int w = bottom_top_blob.w;
            for (int g = 0; g < group_; g++) {
                groupnorm(static_cast<float*>(bottom_top_blob.row_range(g * channels_g, channels_g).data),
                          affine_ ? (const float*)gamma_ + g * channels_g : nullptr,
                          affine_ ? (const float*)beta_ + g * channels_g : nullptr,
                          eps_, channels_g, w, w);
            }
            return 0;
        }

        const int size = bottom_top_blob.w * bottom_top_blob.h * bottom_top_blob.d;
        const size_t cstep = bottom_top_blob.cstep;
        for (int g = 0; g < group_; g++) {
            groupnorm(static_cast<float*>(bottom_top_blob.channel_range(g * channels_g, channels_g).data),
                      affine_ ? (const float*)gamma_ + g * channels_g : nullptr,
                      affine_ ? (const float*)beta_ + g * channels_g : nullptr,
                      eps_, channels_g, size, cstep);
        }
        return 0;
    }

#if NCNN_VULKAN
    int create_pipeline(const ncnn::Option& opt) override {
        if (!opt.use_vulkan_compute) return 0;

        static std::mutex spv_mutex;
        std::vector<uint32_t> spv;
        {
            std::lock_guard<std::mutex> lock(spv_mutex);
            if (!spirv_pack::load_or_compile_custom(CustomShader::GroupNorm, kGroupNormComp,
                                                    opt, spv)) {
                return -1;
            }
        }
        pipeline_ = new ncnn::Pipeline(vkdev);
        pipeline_->set_optimal_local_size_xyz(64, 1, 1);
        if (pipeline_->create(spv.data(), spv.size() * 4, {}) != 0) {
            delete pipeline_;
            pipeline_ = nullptr;
            return -1;
        }
        return 0;
    }

    int destroy_pipeline(const ncnn::Option& /*opt*/) override {
        delete pipeline_;
        pipeline_ = nullptr;
        return 0;
    }

    int upload_model(ncnn::VkTransfer& cmd, const ncnn::Option& opt) override {
        // The pipeline layout always binds weights; affine=0 binds a
        // one-float placeholder that the shader never reads.
        ncnn::Mat host(affine_ ? 2 * channels_ : 1, static_cast<size_t>(4));
        if (affine_) {
            std::memcpy(host.data, gamma_.data, static_cast<size_t>(channels_) * sizeof(float));
            std::memcpy(static_cast<float*>(host.data) + channels_, beta_.data,
                        static_cast<size_t>(channels_) * sizeof(float));
        } else {
            static_cast<float*>(host.data)[0] = 0.f;
        }
        cmd.record_upload(host, weights_gpu_, opt);
        return 0;
    }

    int forward_inplace(ncnn::VkMat& bottom_top_blob, ncnn::VkCompute& cmd,
                        const ncnn::Option& opt) const override {
        if (pipeline_ == nullptr) return -1;

        // Flatten every blob rank onto the shader's (w, h, cstep) row model.
        uint32_t w, h, cstep, channels;
        if (bottom_top_blob.dims == 1) {
            w = 1;
            h = 1;
            cstep = 1;
            channels = bottom_top_blob.w;
        } else if (bottom_top_blob.dims == 2) {
            w = bottom_top_blob.w;
            h = 1;
            cstep = bottom_top_blob.w;
            channels = bottom_top_blob.h;
        } else {
            w = bottom_top_blob.w;
            h = bottom_top_blob.h;
            cstep = static_cast<uint32_t>(bottom_top_blob.cstep);
            channels = bottom_top_blob.c;
        }
        if (group_ <= 0 || channels % static_cast<uint32_t>(group_) != 0) return -100;
        const uint32_t channels_g = channels / static_cast<uint32_t>(group_);

        // Partials live on the layer so the recorded command can still be
        // in flight when forward returns (see the file comment).
        const size_t stats_elems = 2u * channels_g * group_ * h;
        if (stats_.w != static_cast<int>(stats_elems)) {
            stats_.create(static_cast<int>(stats_elems), 4u, 1, opt.workspace_vkallocator);
        }

        std::vector<ncnn::vk_constant_type> constants(8);
        constants[0].i = static_cast<int>(w);
        constants[1].i = static_cast<int>(h);
        constants[2].i = static_cast<int>(cstep);
        constants[3].i = static_cast<int>(channels_g);
        constants[4].i = 0;  // mode: reduce
        constants[5].i = affine_;
        constants[6].f = eps_;
        constants[7].f = 1.0f / static_cast<float>(channels_g * w * h);

        // Grid: one 64-wide workgroup per (group,row) for both modes.
        ncnn::VkMat dispatcher;
        dispatcher.dims = 3;
        dispatcher.w = 2;
        dispatcher.h = static_cast<int>(h);
        dispatcher.c = static_cast<int>(group_);
        dispatcher.elemsize = 4u;
        dispatcher.elempack = 1;

        std::vector<ncnn::VkMat> bindings(4);
        bindings[0] = bottom_top_blob;
        bindings[1] = bottom_top_blob;
        bindings[2] = stats_;
        bindings[3] = weights_gpu_;
        cmd.record_pipeline(pipeline_, bindings, constants, dispatcher);

        constants[4].i = 1;  // mode: normalize
        cmd.record_pipeline(pipeline_, bindings, constants, dispatcher);
        return 0;
    }
#endif  // NCNN_VULKAN

private:
    // Operates on one group's channel range, same math as ncnn groupnorm.cpp.
    static void groupnorm(float* x, const float* gamma, const float* beta, float eps,
                          int channels, int size, size_t cstep) {
        float sum = 0.f;
        for (int q = 0; q < channels; q++) {
            const float* ptr0 = x + cstep * q;
            for (int i = 0; i < size; i++) {
                sum += ptr0[i];
            }
        }
        const float mean = sum / (channels * size);
        float sqsum = 0.f;
        for (int q = 0; q < channels; q++) {
            const float* ptr0 = x + cstep * q;
            for (int i = 0; i < size; i++) {
                const float v = ptr0[i] - mean;
                sqsum += v * v;
            }
        }
        const float var = sqsum / (channels * size);
        const float a = 1.f / std::sqrt(var + eps);
        const float b = -mean * a;
        if (gamma && beta) {
            for (int q = 0; q < channels; q++) {
                float* ptr0 = x + cstep * q;
                const float gam = gamma[q];
                const float bet = beta[q];
                for (int i = 0; i < size; i++) {
                    ptr0[i] = (ptr0[i] * a + b) * gam + bet;
                }
            }
        } else {
            for (int q = 0; q < channels; q++) {
                float* ptr0 = x + cstep * q;
                for (int i = 0; i < size; i++) {
                    ptr0[i] = ptr0[i] * a + b;
                }
            }
        }
    }

#if NCNN_VULKAN
    ncnn::Pipeline* pipeline_ = nullptr;
    mutable ncnn::VkMat stats_;
    ncnn::VkMat weights_gpu_;
#endif

    int group_ = 1;
    int channels_ = 0;
    float eps_ = 0.001f;
    int affine_ = 1;
    ncnn::Mat gamma_;
    ncnn::Mat beta_;
};

inline void register_groupnorm_override(ncnn::Net& net) {
    net.register_custom_layer(
        "GroupNorm",
        [](void*) -> ncnn::Layer* { return new GroupNormLayer(); },
        [](ncnn::Layer* layer, void*) { delete layer; },
        nullptr);
}

} // namespace upix::detail
