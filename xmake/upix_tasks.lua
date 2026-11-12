-- Shared helpers for custom xmake tasks (see xmake/tasks.lua).
-- Isolates host (.xmake/host) and android (.xmake/android) config directories
-- so switching targets never clobbers cached toolchains or packages.

import("core.base.option")
import("lib.detect.find_tool")

local function host_cfgdir()
    return path.join(os.projectdir(), ".xmake", "host")
end

local function android_cfgdir()
    return path.join(os.projectdir(), ".xmake", "android")
end

local function run_xmake(cfgdir, args)
    os.mkdir(cfgdir)
    os.execv(os.programfile(), args, {envs = {XMAKE_CONFIGDIR = cfgdir}})
end

local function to_yn(val, fallback)
    if val == nil then
        return fallback and "y" or "n"
    end
    if type(val) == "boolean" then
        return val and "y" or "n"
    end
    local s = tostring(val):lower()
    return (s == "y" or s == "yes" or s == "true" or s == "1") and "y" or "n"
end

local function append_avif_flags(args)
    local both = option.get("avif")
    table.insert(args, "--avif-encoder=" .. to_yn(option.get("avif-encoder"), both))
    table.insert(args, "--avif-decoder=" .. to_yn(option.get("avif-decoder"), both))
end

local function append_compiler_flags(args, plat)
    if plat ~= "linux" then
        return
    end
    local cc = option.get("cc") or os.getenv("CC")
    local cxx = option.get("cxx") or os.getenv("CXX")
    if not cc then
        local tool = find_tool("gcc-14")
        if tool and tool.program and tool.program:find("gcc-14", 1, true) then
            cc = tool.program
        end
    end
    if not cxx then
        local tool = find_tool("g++-14")
        if tool and tool.program and tool.program:find("g++-14", 1, true) then
            cxx = tool.program
        end
    end
    if cc then
        table.insert(args, "--cc=" .. cc)
    end
    if cxx then
        table.insert(args, "--cxx=" .. cxx)
    end
end

local function configure_host(opts)
    opts = opts or {}
    local plat = opts.plat or os.host()
    local arch = opts.arch or os.arch()
    local mode = opts.mode or option.get("mode") or "release"
    local args = {"config", "-P", os.projectdir(), "-p", plat, "-a", arch, "-m", mode, "--yes"}
    append_avif_flags(args)
    append_compiler_flags(args, plat)
    if opts.native_isa ~= nil then
        table.insert(args, "--native_isa=" .. to_yn(opts.native_isa, true))
    end
    if opts.upx ~= nil then
        table.insert(args, "--upx=" .. (opts.upx and "y" or "n"))
    end
    run_xmake(host_cfgdir(), args)
end

local function find_android_sdk()
    local sdk = option.get("sdk") or os.getenv("ANDROID_SDK_ROOT") or os.getenv("ANDROID_HOME")
    if sdk and os.isdir(sdk) then
        return sdk
    end
    local local_props = path.join(os.projectdir(), "apps", "local.properties")
    if os.isfile(local_props) then
        local content = io.readfile(local_props)
        local dir = content:match("sdk%.dir=([^\r\n]+)")
        if dir and os.isdir(dir) then
            return dir
        end
    end
    raise("Android SDK not found (pass --sdk=... or set ANDROID_SDK_ROOT / ANDROID_HOME)")
end

local function find_android_ndk(sdk)
    local ndk = option.get("ndk") or os.getenv("ANDROID_NDK_ROOT")
        or os.getenv("ANDROID_NDK_HOME") or os.getenv("NDK_HOME")
    if ndk and os.isdir(ndk) then
        return ndk
    end
    local gradle_file = path.join(os.projectdir(), "apps", "app", "build.gradle.kts")
    local ver = io.readfile(gradle_file):match('ndkVersion%s*=%s*"([^"]+)"')
    if ver then
        local candidate = path.join(sdk, "ndk", ver)
        if os.isdir(candidate) then
            return candidate
        end
    end
    raise("Android NDK not found (pass --ndk=... or set ANDROID_NDK_ROOT)")
end

function gui()
    configure_host()
    run_xmake(host_cfgdir(), {"build", "-P", os.projectdir(), "upix"})
    if not option.get("build") then
        run_xmake(host_cfgdir(), {"run", "-P", os.projectdir(), "upix"})
    end
end

function cli()
    configure_host({
        plat = option.get("plat"),
        arch = option.get("arch"),
        native_isa = option.get("native-isa"),
        upx = option.get("upx"),
    })
    run_xmake(host_cfgdir(), {"build", "-P", os.projectdir(), "upix-cli"})
end

function spirv()
    local pack = path.join(os.projectdir(), "share", "core", "spirv_pack.bin")
    if os.isfile(pack) and not option.get("force") then
        print("spirv pack present: %s (%d bytes)", pack, os.filesize(pack))
        return
    end
    configure_host({mode = "release"})
    run_xmake(host_cfgdir(), {"build", "-P", os.projectdir(), "dump-spirv"})
    local bin = path.join(os.projectdir(), "build", os.host(), os.arch(), "release", "dump-spirv")
    assert(os.isfile(bin), "dump-spirv missing: " .. bin)
    os.execv(bin, {pack})
end

function native()
    local mode = option.get("mode") or "release"
    local arch = option.get("arch") or "arm64-v8a"
    local sdk = find_android_sdk()
    local ndk = find_android_ndk(sdk)
    local args = {
        "config", "-P", os.projectdir(),
        "-p", "android", "-a", arch, "-m", mode,
        "--ndk=" .. ndk, "--android_sdk=" .. sdk,
        "--ndk_sdkver=26", "--runtimes=c++_shared",
        "--yes"
    }
    append_avif_flags(args)
    run_xmake(android_cfgdir(), args)
    run_xmake(android_cfgdir(), {"build", "-P", os.projectdir(), "upix-native"})
end

function apk()
    local mode = option.get("mode") or "debug"
    local gradle_task = mode == "release" and "assembleRelease" or "assembleDebug"
    local apps_dir = path.join(os.projectdir(), "apps")
    local gradlew = is_host("windows") and "gradlew.bat" or "./gradlew"
    local args = {gradle_task, "--no-daemon"}
    if option.get("avif") then
        table.insert(args, "-Pupix.avifEncoder=true")
        table.insert(args, "-Pupix.avifDecoder=true")
    end
    os.execv(gradlew, args, {curdir = apps_dir})
end

function tests()
    configure_host()
    local targets = {"test_local_socket", "test_app_state_jobs", "test_gui_math_and_motion"}
    for _, file in ipairs(os.files(path.join(os.projectdir(), "cli", "tests", "test_*.cpp"))) do
        table.insert(targets, path.basename(file))
    end
    table.sort(targets)
    local build_args = {"build", "-P", os.projectdir()}
    for _, t in ipairs(targets) do
        table.insert(build_args, t)
    end
    run_xmake(host_cfgdir(), build_args)
    if not option.get("build") then
        run_xmake(host_cfgdir(), {"test", "-P", os.projectdir(), "--yes"})
    end
end
