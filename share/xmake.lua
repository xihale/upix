target("crossscale-core")
    set_kind("static")
    
    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "glaze", "libsodium")
    add_packages("libdeflate")
    add_includedirs(".", {public = true})
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
    
    -- Auto-collect share sources, excluding platform-specific files handled below.
    add_files(
        "core/*.cpp",
        "core/spirv_pack.S",
        "core/distributed/*.cpp",
        "core/serve/*.cpp",
        "utils/*.cpp|unix_socket.cpp|android_utils.cpp"
    )
    add_asflags("-I" .. os.scriptdir() .. "/core")

    if is_plat("android") then
        add_files("utils/android_utils.cpp")
    end

    -- AF_UNIX serve daemon is Linux/macOS only. The UnixSocket helper compiles
    -- only on POSIX-with-AF_UNIX platforms; on others the daemon source is a no-op stub.
    if is_plat("linux", "macosx") then
        add_files("utils/unix_socket.cpp")
    end
