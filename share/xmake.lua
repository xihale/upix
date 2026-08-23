local spirv_pack = path.join(os.scriptdir(), "core", "spirv_pack.bin")

target("crossscale-core")
    set_kind("static")

    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "glaze", "libsodium")
    add_packages("libdeflate")
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
    before_build(function (target)
        if not is_plat("android") or os.isfile(spirv_pack) then
            return
        end
        print("generating " .. spirv_pack)
        local cfgdir = path.join(os.projectdir(), "build", "host-dump-config")
        os.mkdir(cfgdir)
        local envs = {XMAKE_CONFIGDIR = cfgdir}
        os.execv(os.programfile(), {
            "-P", os.projectdir(), "config", "-p", os.host(), "-a", os.arch(),
            "-m", "release", "-y"
        }, {envs = envs})
        os.execv(os.programfile(), {
            "-P", os.projectdir(), "build", "dump-spirv"
        }, {envs = envs})
        local bin = path.join(os.projectdir(), "build", os.host(), os.arch(), "release", "dump-spirv")
        assert(os.isfile(bin), "dump-spirv missing: " .. bin)
        os.execv(bin, {spirv_pack})
    end)
