-- Host-only SPIR-V dumper. Links the glslang-enabled ncnn variant; runtime
-- targets use the overlay with online_spirv=false.
target("dump-spirv")
    set_kind("binary")
    set_default(false)
    add_packages("ncnn~glslang", "libdeflate")
    add_includedirs("$(projectdir)/share")
    add_files("dump_spirv.cpp")
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
