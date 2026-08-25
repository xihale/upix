-- Host-only SPIR-V dumper. Links the glslang-enabled ncnn variant; runtime
-- targets use the overlay with online_spirv=false. The pack is a build
-- artifact (gitignored *.bin): dump it when share/core/spirv_pack.bin is missing.
local spirv_pack = path.join(os.projectdir(), "share/core/spirv_pack.bin")

target("dump-spirv")
    set_kind("binary")
    set_default(false)
    add_rules("upix.host_isa", "upix.upx")
    add_packages("ncnn~glslang", "libdeflate")
    add_includedirs("$(projectdir)/share")
    add_files("dump_spirv.cpp")
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end

-- Standalone performance-probe CLI. Kept out of upix-cli so the shipped CLI
-- stays lean; probes are developer/tuning tooling. Target name differs from
-- share/perf's static lib "upix-perf"; set_basename restores the natural
-- binary name, so it lands next to upix-cli as build/<plat>/<arch>/<mode>/upix-perf.
target("upix-perf-tool")
    set_kind("binary")
    set_basename("upix-perf")
    add_rules("upix.host_isa")

    -- Single source of truth: root xmake.lua's set_version() feeds --version,
    -- same mechanism as cli_app.cpp.
    on_load(function (target)
        import("core.project.project")
        target:add('defines', 'UPIX_VERSION="v' .. project.version() .. '"')
    end)

    add_deps("upix-core", "upix-perf")
    add_packages("ncnn~runtime", "cli11")
    add_includedirs("$(projectdir)/share")
    add_files("upix_perf_main.cpp")

    if is_plat("linux", "bsd") then
        add_syslinks("dl")  -- NVML is dlopen'd at runtime by share/perf probes
    end

target("upix-core")
    on_load(function (target)
        if not os.isfile(spirv_pack) then
            target:add("deps", "dump-spirv")
        end
    end)
    before_build(function (target)
        if not os.isfile(spirv_pack) then
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
        end
        -- The pack reaches the binary through .incbin inside spirv_pack.S,
        -- which xmake's dependency scan cannot see: after regenerating the
        -- pack, every target would keep linking the OLD shaders until the .S
        -- happened to recompile. Rewrite it (same bytes) to bump its mtime so
        -- the assembler reruns whenever the bin is newer.
        local spirv_pack_s = path.join(os.scriptdir(), "..", "share", "core", "spirv_pack.S")
        if os.mtime(spirv_pack) > os.mtime(spirv_pack_s) then
            local source = io.readfile(spirv_pack_s)
            io.writefile(spirv_pack_s, source)
            print("bumping " .. spirv_pack_s .. " for regenerated SPIR-V pack")
        end
    end)

