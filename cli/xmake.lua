target("crossscale-cli")
    set_kind("binary")
    
    add_deps("crossscale-core", "crossscale-perf")
    add_packages("ncnn", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium")

    add_files(
        "src/main.cpp",
        "src/cli_app.cpp",
        "src/common.cpp",
        "src/cmd_model.cpp",
        "src/cmd_benchmark.cpp",
        "src/cmd_perf.cpp",
        "src/cmd_server.cpp",
        "src/cmd_serve.cpp",
        "src/cmd_process.cpp",
        "src/process_batch.cpp",
        "src/cmd_completion.cpp"
    )
    
    after_install(function (target)
        local bindir = path.join(target:installdir(), "bin")
        os.cp("$(projectdir)/models", bindir)
    end)

target("test_crypto")
    set_kind("binary")
    add_deps("crossscale-core")
    add_packages("libsodium")
    add_files("$(projectdir)/scripts/test_crypto.cpp")
    set_targetdir("$(projectdir)/cli/build")
