set_project("crossscale")
set_version("0.1.0")
set_languages("c++23")

add_rules("mode.debug", "mode.release")

if is_mode("debug") then
    add_cxflags("-fsanitize=address")
    add_ldflags("-fsanitize=address")
end

if is_mode("release") then
    set_optimize("fastest")
    set_strip("all")
end

-- Security hardening flags
if is_plat("linux") or is_plat("macosx") then
    add_cxflags("-fstack-protector-strong", "-D_FORTIFY_SOURCE=2")
    add_cxflags("-fPIE")
    add_ldflags("-pie", "-Wl,-z,relro,-z,now")
end

-- Compiler warnings
add_cxflags("-Wall", "-Wextra", "-Wpedantic")
if is_mode("release") then
    add_cxflags("-Werror")
end

-- Pin to 20260113: glslang-nihui (a vulkan build dep of ncnn) has no 20260526 release,
-- so the latest ncnn (20260526) fails to resolve its glslang-nihui dependency.
-- 20260113 is the newest ncnn version with a matching glslang-nihui release.
add_requires("ncnn 20260113", {configs = {vulkan = true}})
add_requires("libwebp")
add_requires("libpng")
add_requires("libjpeg")
-- System-only require: pkg-config discovery, never a source build.
add_requires("libdeflate", {system = true})
add_requires("glaze")
add_requires("qr-code-generator-cpp")
add_requires("cli11")
add_requires("libsodium")
-- HTTPS client for `model download` (GitHub releases manifest + model zips).
add_requires("libcurl")
add_requires("openssl")
add_requires("libzip")

includes("share", "share/perf", "cli")
