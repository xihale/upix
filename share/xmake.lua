target("crossscale-core")
    set_kind("static")
    
    add_packages("ncnn", "libwebp", "libpng", "libjpeg", "glaze", "libsodium")
    add_includedirs(".", {public = true})
    
    -- Auto-collect share sources, excluding platform-specific files handled below.
    add_files(
        "core/*.cpp",
        "core/distributed/*.cpp",
        "core/serve/*.cpp",
        "utils/*.cpp|unix_socket.cpp|android_utils.cpp"
    )

    if is_plat("android") then
        add_files("utils/android_utils.cpp")
    end

    -- AF_UNIX serve daemon is Linux/macOS only. The UnixSocket helper compiles
    -- only on POSIX-with-AF_UNIX platforms; on others the daemon source is a no-op stub.
    if is_plat("linux", "macosx") then
        add_files("utils/unix_socket.cpp")
    end
