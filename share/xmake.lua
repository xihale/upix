target("crossscale-core")
    set_kind("static")
    
    add_packages("ncnn", "libwebp", "stb", "nlohmann_json")
    add_includedirs(".", {public = true})
    
    add_files(
        "core/engine.cpp",
        "core/ncnn_engine.cpp",
        "core/tile_processor.cpp",
        "core/model_manager.cpp",
        "core/distributed/scheduler.cpp",
        "core/distributed/worker.cpp",
        "core/distributed/coordinator.cpp",
        "core/distributed/protocol.cpp",
        "utils/image.cpp",
        "utils/logger.cpp",
        "utils/network.cpp",
        "utils/benchmark.cpp",
        "utils/crypto.cpp",
        "utils/mdns.cpp"
    )
    
    if is_plat("android") then
        add_files("utils/android_utils.cpp")
    end
