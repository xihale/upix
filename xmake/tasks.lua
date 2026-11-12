-- Project-level xmake custom tasks: `xmake gui`, `xmake cli`, `xmake native`,
-- `xmake apk`, `xmake spirv`, `xmake tests`.

task("gui")
    set_category("plugin")
    on_run(function ()
        import("upix_tasks").gui()
    end)
    set_menu {
        usage = "xmake gui [options]",
        description = "Build and run the desktop GUI (SDL3 + Vulkan + Dear ImGui).",
        options = {
            {'m', "mode",  "kv", "release", "Build mode (release or debug)."},
            {'a', "avif",  "k",  nil,       "Bundle static libavif (encoder + decoder)."},
            {'b', "build", "k",  nil,       "Build only; do not launch the GUI."},
            {nil, "cc",    "kv", nil,       "C compiler."},
            {nil, "cxx",   "kv", nil,       "C++ compiler."},
        }
    }

task("cli")
    set_category("plugin")
    on_run(function ()
        import("upix_tasks").cli()
    end)
    set_menu {
        usage = "xmake cli [options]",
        description = "Build the upix-cli binary.",
        options = {
            {'m', "mode",         "kv", "release", "Build mode (release or debug)."},
            {'p', "plat",         "kv", nil,       "Target platform (default: host)."},
            {'a', "arch",         "kv", nil,       "Target architecture (default: host)."},
            {nil, "avif",         "k",  nil,       "Bundle static libavif (encoder + decoder)."},
            {nil, "avif-encoder", "kv", nil,       "Explicit --avif-encoder (y or n)."},
            {nil, "avif-decoder", "kv", nil,       "Explicit --avif-decoder (y or n)."},
            {nil, "native-isa",   "kv", nil,       "Tune for host CPU SIMD (y or n)."},
            {nil, "upx",          "k",  nil,       "Pack release executable with UPX."},
            {nil, "cc",           "kv", nil,       "C compiler."},
            {nil, "cxx",          "kv", nil,       "C++ compiler."},
        }
    }

task("spirv")
    set_category("plugin")
    on_run(function ()
        import("upix_tasks").spirv()
    end)
    set_menu {
        usage = "xmake spirv [options]",
        description = "Generate share/core/spirv_pack.bin using host dump-spirv.",
        options = {
            {'f', "force", "k",  nil, "Regenerate even if spirv_pack.bin already exists."},
            {nil, "cc",    "kv", nil, "C compiler."},
            {nil, "cxx",   "kv", nil, "C++ compiler."},
        }
    }

task("native")
    set_category("plugin")
    on_run(function ()
        import("upix_tasks").native()
    end)
    set_menu {
        usage = "xmake native [options]",
        description = "Cross-compile libupix-native.so into apps/app/src/main/jniLibs/.",
        options = {
            {'m', "mode",         "kv", "release",   "Build mode (release or debug)."},
            {'a', "arch",         "kv", "arm64-v8a", "Android ABI."},
            {nil, "avif",         "k",  nil,         "Bundle static libavif (encoder + decoder)."},
            {nil, "avif-encoder", "kv", nil,         "Explicit --avif-encoder (y or n)."},
            {nil, "avif-decoder", "kv", nil,         "Explicit --avif-decoder (y or n)."},
            {nil, "sdk",          "kv", nil,         "Android SDK directory."},
            {nil, "ndk",          "kv", nil,         "Android NDK directory."},
        }
    }

task("apk")
    set_category("plugin")
    on_run(function ()
        import("upix_tasks").apk()
    end)
    set_menu {
        usage = "xmake apk [options]",
        description = "Assemble the Android APK via Gradle (automatically builds native JNI).",
        options = {
            {'m', "mode", "kv", "debug", "Build mode (debug or release)."},
            {'a', "avif", "k",  nil,     "Bundle static libavif (encoder + decoder)."},
        }
    }

task("tests")
    set_category("plugin")
    on_run(function ()
        import("upix_tasks").tests()
    end)
    set_menu {
        usage = "xmake tests [options]",
        description = "Build and run host C++ unit tests (share, cli, desktop).",
        options = {
            {'m', "mode",  "kv", "release", "Build mode (release or debug)."},
            {'b', "build", "k",  nil,       "Build test binaries only; do not run."},
            {nil, "cc",    "kv", nil,       "C compiler."},
            {nil, "cxx",   "kv", nil,       "C++ compiler."},
        }
    }
