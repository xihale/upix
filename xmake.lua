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

add_requires("ncnn", {configs = {vulkan = true}})
add_requires("libwebp")
add_requires("libpng")
add_requires("libjpeg")
add_requires("nlohmann_json")
add_requires("cxxopts")
add_requires("qr-code-generator-cpp")

includes("share", "cli")
