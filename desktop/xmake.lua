-- Desktop GUI target (SDL3 + Vulkan + Dear ImGui, direct linkage to share/).
target("upix")
    set_kind("binary")
    set_rules("upix.host_isa", "upix.upx")

    on_load(function (target)
        import("core.project.project")
        target:add("defines", 'UPIX_VERSION="' .. project.version() .. '"')
        local assets = path.join(os.scriptdir(), "assets")
        target:add("defines", 'FIXTURE_DIR="' .. assets:gsub("\\", "/") .. '"')
    end)

    add_deps("upix-core")
    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "libdeflate",
                 "glaze", "qr-code-generator-cpp", "libsodium", "libcurl", "libzip",
                 "libsdl3", "imgui")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end

    if is_plat("linux", "bsd") then
        add_syslinks("vulkan", "dl", "pthread")
    elseif is_plat("windows", "mingw") then
        add_syslinks("vulkan-1", "ws2_32", "iphlpapi")
    end

    add_includedirs("src", "$(projectdir)/share")
    add_files(
        "src/main.cpp",
        "src/app_state.cpp",
        "src/qrcodegen.cpp",
        "src/views/shell.cpp",
        "src/views/workbench.cpp",
        "src/views/models.cpp",
        "src/views/host.cpp"
    )
    set_targetdir("$(projectdir)/desktop/build")
