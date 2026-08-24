-- crossscale-perf: modular performance testing library.
-- Depends on crossscale-core for Engine/TileProcessor; NVML is loaded at runtime
-- via dlopen so no compile-time dependency on nvidia-ml dev headers.
target("crossscale-perf")
    set_kind("static")
    set_default(false)
    add_rules("crossscale.host_isa")

    add_deps("crossscale-core")
    -- gpu-reuse test drives ncnn's public Vulkan API directly (builds its own
    -- ncnn::Net + VkCompute in the test), so it needs the ncnn package itself,
    -- not just the transitive dep through crossscale-core.
    add_packages("ncnn~runtime")
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
        "tests/gpu_efficiency.cpp",
        "tests/gpu_concurrency.cpp",
        "tests/gpu_reuse.cpp"
    )
