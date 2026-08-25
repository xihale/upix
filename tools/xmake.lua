-- Host-only SPIR-V dumper. Links the glslang-enabled ncnn variant; runtime
-- targets use the overlay with online_spirv=false. The pack is a build
-- artifact (gitignored *.bin): dump it when share/core/spirv_pack.bin is missing.
local spirv_pack = path.join(os.projectdir(), "share/core/spirv_pack.bin")

target("dump-spirv")
    set_kind("binary")
    set_default(false)
    add_rules("crossscale.host_isa", "crossscale.upx")
    add_packages("ncnn~glslang", "libdeflate")
    add_includedirs("$(projectdir)/share")
    add_files("dump_spirv.cpp")
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end

target("crossscale-core")
    on_load(function (target)
        if not os.isfile(spirv_pack) then
            target:add("deps", "dump-spirv")
        end
    end)
    before_build(function (target)
        if os.isfile(spirv_pack) then
            return
        end
        import("core.project.project")
        local dump = project.target("dump-spirv")
        assert(dump, "dump-spirv target missing")
        local bin = dump:targetfile()
        if not os.isfile(bin) then
            os.execv(os.programfile(), {"build", "dump-spirv"})
            bin = dump:targetfile()
        end
        print("generating " .. spirv_pack)
        os.execv(bin, {spirv_pack})
    end)

