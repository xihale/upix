set_project("upix")
-- CLI/desktop release channel: feeds upix-cli --version (on_load in
-- cli/xmake.lua). The Android app versions independently in
-- apps/app/build.gradle.kts. Release tags: cli-v*, android-v*, models.
set_version("0.1.0")
set_languages("c++23")

add_repositories("upix-local " .. os.scriptdir())

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

-- Security hardening flags. -pie / -z relro are ELF; Mach-O PIE is the
-- default and clang errors on unused -pie with -Werror.
if is_plat("linux") then
    add_cxflags("-fstack-protector-strong", "-D_FORTIFY_SOURCE=2")
    add_cxflags("-fPIE")
    add_ldflags("-pie", "-Wl,-z,relro,-z,now")
elseif is_plat("macosx") then
    add_cxflags("-fstack-protector-strong", "-D_FORTIFY_SOURCE=2")
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
rule("upix.host_isa")
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
-- Intended for distribution builds: shrinks upix-cli ~15.9M -> ~7.9M
-- (+46 ms per launch). Never packs debug binaries, shared libs, or android.
option("upx")
    set_default(false)
    set_showmenu(true)
    set_description("Pack linux release executables with UPX after build")
option_end()

rule("upix.upx")
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

-- Optional AVIF codecs, opt in with: xmake f --avif-encoder=y --avif-decoder=y
-- (Android: ./gradlew -Pupix.avifEncoder=true -Pupix.avifDecoder=true, or
-- UPIX_AVIF_ENCODER=1/UPIX_AVIF_DECODER=1 for scripts/build_native.sh).
-- Encoder (libavif+aom) enables standalone/on-device AVIF output
-- (+~4.9 MB arm64 .so / +~2.4 MB compressed APK, measured 2026-08-26);
-- decoder (libavif+dav1d) enables native AVIF input decode — e.g. AVIF
-- inputs below Android 12 (+~0.9 MB / +~0.4 MB). The default build stays
-- lean: the host dlopens the system libavif at runtime, Android reports
-- AVIF encode unavailable and input decode keeps the platform BitmapFactory.
option("avif-encoder")
    set_default(false)
    set_showmenu(true)
    set_description("Link libavif+aom for AVIF encoding (default: dlopen system libavif)")
option_end()

option("avif-decoder")
    set_default(false)
    set_showmenu(true)
    set_description("Link libavif+dav1d for AVIF decoding (default: dlopen/system decode)")
option_end()

-- ncnn package: "auto" lets its install script probe the host CPU and skip
-- compiling kernel variants it could never dispatch to ("all" = upstream).
local ncnn_x86_isa = has_config("native_isa") and is_plat("linux") and "auto" or "all"

-- Pin to 20260526 (upstream's newest release). Upstream ncnn vendors glslang
-- as a submodule; glslang-nihui (the xrepo package of that fork) has no
-- 20260526 release, so the vendored ncnn package maps 20260526 onto
-- glslang-nihui 20260113 — ncnn 20260526's submodule is the exact commit
-- (fe88f421) that glslang-nihui 20260113 pins, so the pairing is identical
-- to a native release. Runtime ncnn looks up a frozen SPIR-V pack and does
-- not link glslang.
-- dump-spirv (glslang-linked ncnn) generates share/core/spirv_pack.bin if missing.
-- Desktop builds use ncnn's built-in minimal omp runtime (simpleomp): host
-- binaries (release tarballs, dump-spirv) run directly with no rpath into
-- xmake package dirs, so the real libomp shared runtime must not be a
-- dependency. Android keeps real libomp (packaged inside the APK).
add_requires("ncnn~runtime 20260526", {configs = {vulkan = true, online_spirv = false, x86_isa = ncnn_x86_isa, simpleomp = not is_plat("android")}})
-- Version policy (component audit 2026-09-15): anything statically embedded
-- in the Android APK must not resolve to a silently ancient version — xrepo
-- ships libwebp 1.3.0 to this day (missing the CVE-2023-4863 fix), which the
-- APK embedded for months. Security-relevant embeds get a floor pin that
-- turns an unresolvable floor into a loud failure; the libwebp manifest is
-- vendored into the upix-local repo (packages/l/libwebp) because xrepo
-- upstream stopped at 1.3.0. glaze gets an exact pin: header-only static on
-- both platforms and a history of breaking major bumps. libpng/libjpeg/
-- libsodium/libdeflate stay unpinned — host builds link rolling system libs
-- (libjpeg's IJG 9f vs turbo 3.x version lines are not even comparable), and
-- none of them is currently behind a security fix.
-- libwebpmux: XMP marker chunks for WebP outputs (image.cpp post-mux).
add_requires("libwebp >=1.6.0", {configs = {libwebpmux = true}})
add_requires("libpng", "libjpeg", "libsodium")
add_requires("glaze v7.9.0")
add_requires("libdeflate", {system = not is_plat("android")})
-- One libavif require with the merged codec set (aom/dav1d) so both options
-- share a single package install. Must stay in table-config form: variadic
-- configs get swallowed (see libwebp note above / memory of the SR work).
if has_config("avif-encoder") or has_config("avif-decoder") then
    local avif_codec_configs = {}
    if has_config("avif-encoder") then
        avif_codec_configs.aom = true
    end
    if has_config("avif-decoder") then
        avif_codec_configs.dav1d = true
    end
    add_requires("libavif", {configs = avif_codec_configs})
end
if is_plat("android") then
    add_requires("libomp")
end

if not is_plat("android") then
    add_requires("ncnn~glslang 20260526", {configs = {vulkan = true, online_spirv = true, skip_device_defines = true, x86_isa = ncnn_x86_isa, simpleomp = true}})
    add_requires("qr-code-generator-cpp")
    add_requires("cli11")
    add_requires("libcurl")
    -- Test-only oracle: cli/tests/test_model_downloader.cpp hashes with
    -- OpenSSL EVP on purpose, so the libsodium sha256 in model_downloader is
    -- cross-validated against an independent implementation. Production code
    -- must not add new OpenSSL includes.
    add_requires("openssl")
    add_requires("libzip")
end

includes("share")
if is_plat("android") then
    includes("apps")
else
    includes("share/perf", "cli", "tools")
end
