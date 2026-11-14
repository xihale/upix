-- Desktop GUI target (SDL3 + Vulkan + Dear ImGui, direct linkage to share/).
target("upix")
    set_kind("binary")
    set_rules("upix.host_isa", "upix.upx")

    on_load(function (target)
        import("core.project.project")
        local ver = os.getenv("UPIX_VERSION")
        if not ver or ver == "" then
            ver = project.version()
        end
        target:add("defines", 'UPIX_VERSION="' .. ver .. '"')
        local assets = path.join(os.scriptdir(), "assets")
        target:add("defines", 'FIXTURE_DIR="' .. assets:gsub("\\", "/") .. '"')
    end)

    add_deps("upix-core")
    add_packages("libsdl3", "imgui", "ncnn~runtime", "libwebp", "libpng", "libjpeg",
                 "libdeflate", "glaze", "qr-code-generator-cpp", "libsodium", "libcurl", "libzip")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end

    if is_plat("linux", "bsd") then
        add_syslinks("vulkan", "dl", "pthread")
    elseif is_plat("windows", "mingw") then
        add_syslinks("vulkan-1", "ws2_32", "iphlpapi")
    elseif is_plat("macosx") then
        add_frameworks("CoreFoundation")
        -- SDL3's Cocoa Vulkan loader resolves vkGetInstanceProcAddr via
        -- dlsym(RTLD_DEFAULT, ...) when no dynamic loader dylib is present;
        -- force-pull it from static libMoltenVK.a and preserve it across -dead_strip.
        add_ldflags("-Wl,-u,_vkGetInstanceProcAddr", "-Wl,-export_dynamic", {force = true})
    end

    -- Minimal mingw toolchains (e.g. zig cc) do not ship libvulkan-1.a;
    -- synthesize a PE import library from Vulkan headers so the binary links
    -- cleanly against the runtime vulkan-1.dll on Windows.
    on_config(function (target)
        if not target:is_plat("mingw") then
            return
        end
        if os.is_host("windows") and get_config("toolchain") ~= "zigcc" then
            return
        end
        import("lib.detect.find_library")
        local linkdirs = table.wrap(target:get("linkdirs"))
        for _, pkg in pairs(target:pkgs() or {}) do
            table.join2(linkdirs, table.wrap(pkg:get("linkdirs")))
        end
        if #linkdirs > 0 and find_library("vulkan-1", linkdirs, {plat = "mingw"}) then
            return
        end

        local seen_dirs = {}
        local hdrs = {}
        local function add_include_dir(dir)
            if not dir or seen_dirs[dir] then return end
            seen_dirs[dir] = true
            for _, name in ipairs({"vulkan/vulkan_core.h", "vulkan/vulkan_win32.h"}) do
                local p = path.join(dir, name)
                if os.isfile(p) then
                    table.insert(hdrs, p)
                end
            end
        end
        for _, dir in ipairs(table.wrap(target:get("includedirs"))) do add_include_dir(dir) end
        for _, dir in ipairs(table.wrap(target:get("sysincludedirs"))) do add_include_dir(dir) end
        for _, pkg in pairs(target:pkgs() or {}) do
            for _, dir in ipairs(table.wrap(pkg:get("includedirs"))) do add_include_dir(dir) end
            for _, dir in ipairs(table.wrap(pkg:get("sysincludedirs"))) do add_include_dir(dir) end
            local vh = pkg.dep and pkg:dep("vulkan-headers")
            if vh then
                add_include_dir(vh:installdir("include"))
            end
        end
        assert(#hdrs > 0, "failed to locate vulkan_core.h / vulkan_win32.h to synthesize libvulkan-1.a")

        local shimdir = path.join(target:autogendir(), "vulkan-shim")
        local implib = path.join(shimdir, "libvulkan-1.a")
        if not os.isfile(implib) then
            local arch_map = {
                x86_64 = "i386:x86-64",
                x64 = "i386:x86-64",
                i386 = "i386",
                x86 = "i386",
                arm64 = "arm64",
                aarch64 = "arm64",
            }
            local machine = arch_map[target:arch()]
            assert(machine, "unsupported mingw arch for vulkan-1 import lib: " .. tostring(target:arch()))

            local syms = {}
            local seen_syms = {}
            for _, h in ipairs(hdrs) do
                local text = io.readfile(h) or ""
                for sym in text:gmatch("VKAPI_CALL%s+(vk[%w_]+)%s*%(") do
                    if not seen_syms[sym] then
                        seen_syms[sym] = true
                        table.insert(syms, sym)
                    end
                end
            end
            table.sort(syms)
            assert(#syms > 0, "no vk* prototypes found in vulkan headers")

            os.mkdir(shimdir)
            local def = path.join(shimdir, "vulkan-1.def")
            io.writefile(def, "LIBRARY vulkan-1.dll\nEXPORTS\n" .. table.concat(syms, "\n") .. "\n")

            import("lib.detect.find_tool")
            local zig = find_tool("zig")
            local dlltool = find_tool("llvm-dlltool") or find_tool("dlltool")
            if zig then
                os.runv(zig.program, {"dlltool", "-m", machine, "-d", def, "-l", implib})
            elseif dlltool then
                os.runv(dlltool.program, {"-m", machine, "-d", def, "-l", implib})
            else
                raise("neither zig nor dlltool found to generate libvulkan-1.a")
            end
        end
        target:add("linkdirs", shimdir)
    end)

    add_includedirs("src", "$(projectdir)/share")
    add_files(
        "src/main.cpp",
        "src/app_state.cpp",
        "src/i18n.cpp",
        "src/qrcodegen.cpp",
        "src/views/shell.cpp",
        "src/views/workbench.cpp",
        "src/views/workbench/stage_overlays.cpp",
        "src/views/workbench/filmstrip_dock.cpp",
        "src/views/workbench/model_picker_modal.cpp",
        "src/views/workbench/inspector.cpp",
        "src/views/models.cpp",
        "src/views/host.cpp",
        "src/views/host/widgets.cpp",
        "src/views/host/ip_menu.cpp",
        "src/views/host/pairing_modal.cpp",
        "src/views/host/hero_card.cpp",
        "src/views/host/trusted_devices.cpp",
        "src/views/host/activity_log.cpp",
        "src/views/settings.cpp"
    )
    set_targetdir("$(projectdir)/desktop/build")

-- Headless AppState & job lifecycle test
target("test_app_state_jobs")
    set_kind("binary")
    add_tests("default", {rundir = "$(projectdir)"})
    add_deps("upix-core")
    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "libdeflate",
                 "glaze", "qr-code-generator-cpp", "libsodium", "libcurl", "libzip")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end
    if is_plat("macosx") then
        add_frameworks("CoreFoundation")
    end
    add_includedirs("src", "$(projectdir)/share")
    add_files("tests/test_app_state_jobs.cpp", "src/app_state.cpp", "src/i18n.cpp", "src/qrcodegen.cpp")
    set_targetdir("$(projectdir)/desktop/build/tests")

-- Headless Geist math, slider magnet, DPI snap & motion spring test
target("test_gui_math_and_motion")
    set_kind("binary")
    add_tests("default", {rundir = "$(projectdir)"})
    add_packages("imgui")
    add_includedirs("src")
    add_files("tests/test_gui_math_and_motion.cpp", "src/i18n.cpp")
    if is_plat("macosx") then
        add_frameworks("CoreFoundation")
    end
    set_targetdir("$(projectdir)/desktop/build/tests")

-- Headless i18n locale detection, BCP-47 normalization & translation parity test
target("test_i18n")
    set_kind("binary")
    add_tests("default", {rundir = "$(projectdir)"})
    add_includedirs("src")
    add_files("tests/test_i18n.cpp", "src/i18n.cpp")
    if is_plat("macosx") then
        add_frameworks("CoreFoundation")
    end
    set_targetdir("$(projectdir)/desktop/build/tests")
