-- CLI target. Models are NOT bundled with the binary: they are distributed
-- via the GitHub "models" release and fetched with `crossscale model download`
-- (or imported locally with `crossscale model import`).
target("crossscale-cli")
    set_kind("binary")

    add_deps("crossscale-core", "crossscale-perf")
    add_packages("ncnn", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium", "libcurl", "openssl", "libzip")

    add_files(
        "src/main.cpp",
        "src/cli_app.cpp",
        "src/common.cpp",
        "src/cmd_model.cpp",
        "src/model_downloader.cpp",
        "src/cmd_perf.cpp",
        "src/cmd_server.cpp",
        "src/cmd_serve.cpp",
        "src/cmd_process.cpp",
        "src/process_batch.cpp"
    )

target("test_crypto")
    set_kind("binary")
    add_deps("crossscale-core")
    add_packages("libsodium")
    add_files("$(projectdir)/cli/tests/test_crypto.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_protocol")
    set_kind("binary")
    add_deps("crossscale-core")
    add_files("$(projectdir)/cli/tests/test_protocol.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_image_avif")
    set_kind("binary")
    add_deps("crossscale-core")
    add_packages("libwebp", "libpng", "libjpeg", "libdeflate")
    if is_plat("linux", "bsd") then
        add_syslinks("dl")
    end
    add_files("$(projectdir)/cli/tests/test_image_avif.cpp")
    set_targetdir("$(projectdir)/cli/build")

target("test_model_downloader")
    set_kind("binary")
    add_files("$(projectdir)/cli/src/model_downloader.cpp")
    add_packages("libcurl", "openssl", "libzip", "glaze")
    add_includedirs("$(projectdir)/cli/src")
    add_files("$(projectdir)/cli/tests/test_model_downloader.cpp")
    set_targetdir("$(projectdir)/cli/build")
