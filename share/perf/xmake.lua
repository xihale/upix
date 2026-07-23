-- crossscale-perf: modular performance testing library.
-- Depends on crossscale-core for Engine/TileProcessor; NVML is loaded at runtime
-- via dlopen so no compile-time dependency on nvidia-ml dev headers.
target("crossscale-perf")
    set_kind("static")
    set_default(false)

    add_deps("crossscale-core")
    add_includedirs(".", {public = true})

    if not is_plat("android") then
        -- dlopen needs libdl on glibc; on modern glibc it's in libc, harmless to add.
        add_syslinks("dl")
    end

    add_files(
        "probes.cpp",
        "reporter.cpp",
        "workload.cpp",
        "runner.cpp",
        "tests/gpu_efficiency.cpp"
    )
