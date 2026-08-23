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

-- Pin to 20260113: glslang-nihui (a vulkan build dep of ncnn) has no 20260526 release,
-- so the latest ncnn (20260526) fails to resolve its glslang-nihui dependency.
-- 20260113 is the newest ncnn version with a matching glslang-nihui release.
-- Runtime ncnn looks up a frozen SPIR-V pack and does not link glslang.
-- dump-spirv (glslang-linked ncnn) generates share/core/spirv_pack.bin if missing.
add_requires("ncnn~runtime 20260113", {configs = {vulkan = true, online_spirv = false}})
add_requires("libwebp", "libpng", "libjpeg", "glaze", "libsodium")
add_requires("libdeflate", {system = not is_plat("android")})
if is_plat("android") then
    add_requires("libomp")
end

if not is_plat("android") then
    add_requires("ncnn~glslang 20260113", {configs = {vulkan = true, online_spirv = true, skip_device_defines = true}})
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
