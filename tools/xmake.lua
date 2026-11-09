-- Host-only SPIR-V dumper. Links the glslang-enabled ncnn variant; runtime
-- targets use the overlay with online_spirv=false. engine/core/spirv_pack.bin
-- is tracked, so this only runs when shaders change and the pack is
-- regenerated deliberately (or removed).

target("dump-spirv")
    set_kind("binary")
    set_default(false)
    add_rules("upix.host_isa", "upix.upx")
    add_packages("ncnn~glslang", "libdeflate")
    add_includedirs("$(projectdir)/engine")
    add_files("dump_spirv.cpp")
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
