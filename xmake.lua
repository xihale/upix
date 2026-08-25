set_project("crossscale")
set_version("0.1.0")
set_languages("c++23")

add_repositories("crossscale-local " .. os.scriptdir())

add_rules("mode.debug", "mode.release")

if is_mode("debug") and not is_plat("android") then
    add_cxflags("-fsanitize=address")
    add_ldflags("-fsanitize=address")
end

if is_mode("release") then
    set_optimize("fastest")
    set_strip("all")
    add_cxflags("-ffunction-sections", "-fdata-sections")
    if is_plat("linux", "android") then
        add_cxflags("-fvisibility=hidden")
        add_ldflags("-Wl,--gc-sections", "-Wl,--exclude-libs,ALL")
    elseif is_plat("macosx") then
        add_ldflags("-Wl,-dead_strip")
    end
end

if is_plat("android") then
    add_cxflags("-fPIC", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections")
    add_ldflags("-Wl,--gc-sections", "-Wl,--exclude-libs,ALL", "-Wl,--as-needed")
end

-- Security hardening flags
if is_plat("linux") or is_plat("macosx") then
    add_cxflags("-fstack-protector-strong", "-D_FORTIFY_SOURCE=2")
    add_cxflags("-fPIE")
    add_ldflags("-pie", "-Wl,-z,relro,-z,now")
end

-- Compiler warnings
add_cxflags("-Wall", "-Wextra", "-Wpedantic")
if is_mode("release") and not is_plat("android") then
    add_cxflags("-Werror")
end

-- Native-ISA tuning (linux desktop only): detect the build host's SIMD
-- support so we can drop ncnn x86 kernel dispatch variants this CPU could
-- never execute (shrinks every host binary by several MB), and raise our own
-- code baseline to match. Resulting binaries assume an AVX2-class CPU
-- (Haswell 2013+); build redistributable binaries with: xmake f --native_isa=n
option("native_isa")
    set_default(true)
    set_showmenu(true)
    set_description("Tune desktop builds for the build host's SIMD support")
option_end()

-- The description scope cannot run processes or read files, so host probing
-- happens in this rule's on_load, which runs with full API access.
rule("crossscale.host_isa")
    on_load(function (target)
        if not has_config("native_isa") then return end
        if os.host() ~= "linux" or target:plat() ~= "linux" or target:arch() ~= "x86_64" then return end
        -- try's return protocol differs between scopes; assign via closure instead.
        local flags
        try {
            function ()
                flags = os.iorun("grep -m1 '^flags' /proc/cpuinfo")
            end,
            catch {
                function () flags = nil end
            }
        }
        if type(flags) ~= "string" then return end
        local padded = " " .. flags:gsub("%s+", " ") .. " "
        local function has(flag)
            return padded:find(" " .. flag .. " ", 1, true) ~= nil
        end
        if has("avx2") and has("fma") then
            -- x86-64-v3 = AVX2+FMA+F16C+BMI baseline; portable across modern
            -- CPUs while letting the compiler emit FMA/F16C without pinning.
            target:add("cxflags", "-march=x86-64-v3")
        end
    end)
rule_end()

-- Optional UPX packing of desktop executables, opt in with: xmake f --upx=y
-- Intended for distribution builds: shrinks crossscale-cli ~15.9M -> ~7.9M
-- (+46 ms per launch). Never packs debug binaries, shared libs, or android.
option("upx")
    set_default(false)
    set_showmenu(true)
    set_description("Pack linux release executables with UPX after build")
option_end()

rule("crossscale.upx")
    after_build(function (target)
        if not has_config("upx") then return end
        if os.host() ~= "linux" or target:plat() ~= "linux" or target:arch() ~= "x86_64" then return end
        if get_config("mode") ~= "release" or target:kind() ~= "binary" then return end
        import("lib.detect.find_tool")
        local upx = find_tool("upx")
        if not upx then
            raise("xmake f --upx=y but upx was not found in PATH")
        end
        local bin = target:targetfile()
        -- Rebuilds rerun after_build; upx refuses to pack an already-packed
        -- file, so probe first (-l fails on a plain ELF).
        local listed
        try {
            function () listed = os.iorunv(upx.program, {"-l", bin}) end,
            catch { function () listed = nil end }
        }
        if type(listed) == "string" and #listed > 0 then return end
        os.execv(upx.program, {"-q", bin})
    end)
rule_end()

-- ncnn package: "auto" lets its install script probe the host CPU and skip
-- compiling kernel variants it could never dispatch to ("all" = upstream).
local ncnn_x86_isa = has_config("native_isa") and is_plat("linux") and "auto" or "all"

-- Pin to 20260113: glslang-nihui (a vulkan build dep of ncnn) has no 20260526 release,
-- so the latest ncnn (20260526) fails to resolve its glslang-nihui dependency.
-- 20260113 is the newest ncnn version with a matching glslang-nihui release.
-- Runtime ncnn looks up a frozen SPIR-V pack and does not link glslang.
-- dump-spirv (glslang-linked ncnn) generates share/core/spirv_pack.bin if missing.
add_requires("ncnn~runtime 20260113", {configs = {vulkan = true, online_spirv = false, x86_isa = ncnn_x86_isa}})
add_requires("libwebp", "libpng", "libjpeg", "glaze", "libsodium")
add_requires("libdeflate", {system = not is_plat("android")})
if is_plat("android") then
    add_requires("libomp")
end

if not is_plat("android") then
    add_requires("ncnn~glslang 20260113", {configs = {vulkan = true, online_spirv = true, skip_device_defines = true, x86_isa = ncnn_x86_isa}})
    add_requires("qr-code-generator-cpp")
    add_requires("cli11")
    add_requires("libcurl")
    add_requires("openssl")
    add_requires("libzip")
end

includes("share")
if is_plat("android") then
    includes("apps")
else
    includes("share/perf", "cli", "tools")
end
