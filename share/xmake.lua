local spirv_pack = path.join(os.scriptdir(), "core", "spirv_pack.bin")

target("upix-core")
    set_kind("static")
    add_rules("upix.host_isa", "upix.upx")

    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "glaze", "libsodium")
    add_packages("libdeflate")
    -- Model release downloads (core/model_downloader.cpp): desktop + CLI
    -- only — Android fetches models through its own store flow and must not
    -- grow the curl/zip link deps.
    if is_plat("android") then
        remove_files("core/model_downloader.cpp")
    else
        add_packages("libcurl", "libzip")
    end
    -- image.cpp post-muxes XMP via libwebpmux; xmake-repo does not always
    -- propagate that extra lib to dependents on macosx.
    add_links("webpmux")
    -- Optional AVIF codecs (root xmake.lua options): the defines gate the
    -- static-libavif branch of utils/image_avif.cpp; the package provides
    -- avif/avif.h + the static libs for everything that links upix-core.
    if has_config("avif-encoder") then
        add_packages("libavif")
        add_defines("UPIX_WITH_AVIF_ENCODER")
    end
    if has_config("avif-decoder") then
        add_packages("libavif")
        add_defines("UPIX_WITH_AVIF_DECODER")
    end
    add_includedirs(".", {public = true})
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
    if is_plat("android") then
        -- Custom ncnn layers inherit from a base built without Android RTTI.
        add_cxflags("-fno-rtti")
    end

    add_files(
        "core/*.cpp",
        "core/distributed/*.cpp",
        "utils/*.cpp|android_utils.cpp"
    )
    add_asflags("-I" .. os.scriptdir() .. "/core")

    -- SPIR-V pack is embedded for the Vulkan path (host + Android arm64).
    if not is_plat("android") or is_arch("arm64-v8a") then
        add_files("core/spirv_pack.S")
    end

    -- Local serve daemon (AF_UNIX on Linux/macOS, named pipes on Windows —
    -- utils/local_socket.hpp). The wrong-platform local_socket_*.cpp
    -- compiles to an empty TU, so no file branching is needed.
    if is_plat("linux", "macosx", "windows", "mingw") then
        add_files("core/serve/*.cpp")
    end

    if is_plat("windows", "mingw") then
        -- WinSock (network.cpp WSAStartup) + adapters enumeration.
        add_syslinks("ws2_32", "iphlpapi")
    end

    -- Android NDK cannot run dump-spirv. If the pack is missing, spawn an
    -- isolated host xmake so the in-progress android config is not overwritten.
    -- Scope: android only — the host flow is owned by tools/xmake.lua's
    -- upix-core hook. `before_build` registrations are not cumulative (a second
    -- registration on the same target replaces this one), so each hook
    -- early-returns outside its own platform and the pair stays correct no
    -- matter which `includes()` in the root project file runs last.
    -- Also bump the .S mtime whenever the pack is newer. .incbin does not
    -- track the packed blob as a build dependency — without this a stale pack
    -- silently ships.
    before_build(function (target)
        if not is_plat("android") then return end
        local asm = path.join(os.scriptdir(), "core", "spirv_pack.S")
        if not os.isfile(spirv_pack) then
            print("generating " .. spirv_pack)
            local cfgdir = path.join(os.projectdir(), ".xmake", "host")
            os.mkdir(cfgdir)
            local envs = {XMAKE_CONFIGDIR = cfgdir}
            os.execv(os.programfile(), {
                "config", "-P", os.projectdir(), "-p", os.host(), "-a", os.arch(),
                "-m", "release", "--yes"
            }, {envs = envs})
            os.execv(os.programfile(), {
                "build", "-P", os.projectdir(), "dump-spirv"
            }, {envs = envs})
            local bin = path.join(os.projectdir(), "build", os.host(), os.arch(), "release", "dump-spirv")
            assert(os.isfile(bin), "dump-spirv missing: " .. bin)
            os.execv(bin, {spirv_pack})
        end
        if os.isfile(spirv_pack) and os.mtime(spirv_pack) > os.mtime(asm) then
            print("bumping " .. asm .. " for regenerated pack")
            os.touch(asm)
        end
    end)

-- Headless transport tests (utils/local_socket): POSIX CI + WIN32/wine.
-- Links only the socket slice — no ncnn/curl — so a mingw build can pin the
-- named-pipe backend without the engine.
target("test_local_socket")
    set_kind("binary")
    set_default(false)
    add_tests("default", {rundir = "$(projectdir)"})
    add_includedirs(".", {public = true})
    add_files(
        "tests/test_local_socket.cpp",
        "utils/local_socket_posix.cpp",
        "utils/local_socket_win.cpp",
        "utils/stream_socket.cpp",
        "utils/network.cpp"
    )
    add_packages("libpng", "libjpeg", "libwebp", "libdeflate")
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "iphlpapi")
    end
