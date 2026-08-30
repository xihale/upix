local spirv_pack = path.join(os.scriptdir(), "core", "spirv_pack.bin")

target("upix-core")
    set_kind("static")
    add_rules("upix.host_isa", "upix.upx")

    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "glaze", "libsodium")
    add_packages("libdeflate")
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

    add_files(
        "core/*.cpp",
        "core/distributed/*.cpp",
        "utils/*.cpp|unix_socket.cpp|android_utils.cpp"
    )
    add_asflags("-I" .. os.scriptdir() .. "/core")

    -- SPIR-V pack is embedded for the Vulkan path (host + Android arm64).
    if not is_plat("android") or is_arch("arm64-v8a") then
        add_files("core/spirv_pack.S")
    end

    -- AF_UNIX serve daemon is Linux/macOS only.
    if is_plat("linux", "macosx") then
        add_files("core/serve/*.cpp")
        add_files("utils/unix_socket.cpp")
    end

    -- Android NDK cannot run dump-spirv. If the pack is missing, spawn an
    -- isolated host xmake so the in-progress android config is not overwritten.
    -- Single hook (multiple before_build registrations are not cumulative):
    -- also bump the .S mtime whenever the pack is newer. .incbin does not
    -- track the packed blob as a build dependency — without this a stale pack
    -- silently ships.
    before_build(function (target)
        local asm = path.join(os.scriptdir(), "core", "spirv_pack.S")
        if is_plat("android") and not os.isfile(spirv_pack) then
            print("generating " .. spirv_pack)
            local cfgdir = path.join(os.projectdir(), "build", "host-dump-config")
            os.mkdir(cfgdir)
            local envs = {XMAKE_CONFIGDIR = cfgdir}
            os.execv(os.programfile(), {
                "-P", os.projectdir(), "config", "-p", os.host(), "-a", os.arch(),
                "-m", "release", "--yes"
            }, {envs = envs})
            os.execv(os.programfile(), {
                "-P", os.projectdir(), "build", "dump-spirv"
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
