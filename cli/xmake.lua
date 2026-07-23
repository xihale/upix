target("crossscale-cli")
    set_kind("binary")
    
    add_deps("crossscale-core")
    add_packages("ncnn", "libwebp", "glaze", "qr-code-generator-cpp")
    
    add_files(
        "src/main.cpp"
    )
    
    after_install(function (target)
        local bindir = path.join(target:installdir(), "bin")
        os.cp("$(projectdir)/models", bindir)
    end)
