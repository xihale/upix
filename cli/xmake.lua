-- CLI target. Models are NOT bundled with the binary: they are distributed
-- via the GitHub "models" release and fetched with `upix model download`
-- (or imported locally with `upix model import`).
target("upix-cli")
    set_kind("binary")
    set_rules("upix.host_isa", "upix.upx")

    -- Single source of truth: root xmake.lua's set_version() feeds the
    -- CLI11 --version output via UPIX_VERSION (cli_app.cpp). The
    -- project module is script-scope only, hence on_load. Optional codec
    -- builds append their capabilities so `upix --version` says whether
    -- AVIF is linked in or dlopen'd at runtime.
    on_load(function (target)
        import("core.project.project")
        local caps = ""
        if has_config("avif-encoder") then
            caps = caps .. " +avif-encoder"
        end
        if has_config("avif-decoder") then
            caps = caps .. " +avif-decoder"
        end
        if caps == "" then
            caps = " avif:system-dlopen"
        end
        target:add('defines', 'UPIX_VERSION="v' .. project.version() .. caps .. '"')
    end)

    add_deps("upix-core")
    add_packages("ncnn~runtime", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium", "libcurl", "openssl", "libzip")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end

    add_files(
        "src/main.cpp",
        "src/cli_app.cpp",
        "src/common.cpp",
        "src/cmd_model.cpp",
        "src/model_downloader.cpp",
        "src/cmd_host.cpp",
        "src/cmd_daemon.cpp",
        "src/cmd_completions.cpp",
        "src/cmd_complete.cpp",
        "src/cmd_process.cpp",
        "src/process_batch.cpp"
    )

target("test_crypto")
    set_kind("binary")
    add_deps("upix-core")
    add_packages("libsodium")
    add_files("$(projectdir)/cli/tests/test_crypto.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_protocol")
    set_kind("binary")
    add_deps("upix-core")
    add_files("$(projectdir)/cli/tests/test_protocol.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_image_avif")
    set_kind("binary")
    add_deps("upix-core")
    add_packages("libwebp", "libpng", "libjpeg", "libdeflate")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
    add_files("$(projectdir)/cli/tests/test_image_avif.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_sr_marker")
    set_kind("binary")
    add_deps("upix-core")
    add_packages("libwebp", "libpng", "libjpeg", "libdeflate")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
    add_files("$(projectdir)/cli/tests/test_sr_marker.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_gpu_precision")
    set_kind("binary")
    add_packages("ncnn~runtime")
    add_includedirs("$(projectdir)/share")
    add_files("$(projectdir)/cli/tests/test_gpu_precision.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_model_downloader")
    set_kind("binary")
    add_files("$(projectdir)/cli/src/model_downloader.cpp")
    add_packages("libcurl", "openssl", "libzip", "glaze")
    add_includedirs("$(projectdir)/cli/src")
    add_files("$(projectdir)/cli/tests/test_model_downloader.cpp")
    set_targetdir("$(projectdir)/cli/build")
