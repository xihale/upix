#include "core/gpu_precision.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    using upix::GpuPrecisionProfile;
    using upix::GpuFeatureCaps;

    ncnn::Option igpu;
    upix::apply_profile(igpu, GpuPrecisionProfile::Integrated);
    if (!igpu.use_fp16_packed || !igpu.use_fp16_storage || igpu.use_fp16_arithmetic ||
        igpu.use_shader_local_memory || igpu.use_cooperative_matrix || igpu.use_subgroup_ops ||
        !igpu.use_int8_storage) {
        std::cerr << "integrated profile bits wrong\n";
        return 1;
    }

    ncnn::Option dgpu;
    upix::apply_profile(dgpu, GpuPrecisionProfile::Discrete);
    if (!dgpu.use_fp16_arithmetic || !dgpu.use_shader_local_memory || dgpu.use_fp16_uniform) {
        std::cerr << "discrete profile bits wrong\n";
        return 1;
    }

    if (upix::option_key(igpu) == upix::option_key(dgpu)) {
        std::cerr << "profile keys collided\n";
        return 1;
    }

    GpuFeatureCaps phone;
    phone.discrete = false;
    phone.mobile_vendor = true;
    phone.fp16_packed = true;
    phone.fp16_storage = true;
    phone.fp16_arithmetic = true;
    phone.int8_packed = true;
    phone.int8_storage = true;
    auto p = upix::select_profile(phone, false);
    if (!p || *p != GpuPrecisionProfile::Integrated) {
        std::cerr << "phone should pick integrated even with arith support\n";
        return 1;
    }

    GpuFeatureCaps nvidia;
    nvidia.discrete = true;
    nvidia.fp16_packed = true;
    nvidia.fp16_storage = true;
    nvidia.fp16_arithmetic = true;
    nvidia.int8_packed = true;
    nvidia.int8_storage = true;
    p = upix::select_profile(nvidia, false);
    if (!p || *p != GpuPrecisionProfile::Discrete) {
        std::cerr << "dGPU should pick discrete\n";
        return 1;
    }

    GpuFeatureCaps weak;
    weak.fp16_packed = true;
    if (upix::select_profile(weak, false)) {
        std::cerr << "missing storage/int8 should not pick a profile\n";
        return 1;
    }

    std::cout << "ok integrated=0x" << std::hex << upix::option_key(igpu)
              << " discrete=0x" << upix::option_key(dgpu) << "\n";
    return 0;
}
