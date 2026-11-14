-- Shared helpers for custom xmake tasks (see xmake/tasks.lua).
-- Isolates host (.xmake/host) and android (.xmake/android) config directories
-- so switching targets never clobbers cached toolchains or packages.

import("core.base.option")
import("lib.detect.find_program")
import("lib.detect.find_tool")

local function host_cfgdir()
    return path.join(os.projectdir(), ".xmake", "host")
end

local function target_cfgdir(plat, arch, toolchain)
    if plat == os.host() and arch == os.arch() then
        if not toolchain then
            return host_cfgdir()
        end
        return path.join(os.projectdir(), ".xmake", "host-" .. toolchain)
    end
    return path.join(os.projectdir(), ".xmake", "cross-" .. plat .. "-" .. arch)
end

local function normalize_arch(plat, arch)
    if not arch then
        return nil
    end
    if plat == "windows" then
        local map = {x86_64 = "x64", aarch64 = "arm64"}
        return map[arch] or arch
    end
    local map = {x64 = "x86_64", aarch64 = "arm64"}
    return map[arch] or arch
end

local function android_cfgdir()
    return path.join(os.projectdir(), ".xmake", "android")
end

local function run_xmake(cfgdir, args)
    os.mkdir(cfgdir)
    local envs = {XMAKE_CONFIGDIR = cfgdir}
    local bindir = path.join(cfgdir, "bin")
    if os.isdir(bindir) then
        envs.PATH = bindir .. path.envsep() .. (os.getenv("PATH") or "")
    end
    os.execv(os.programfile(), args, {envs = envs})
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

local function append_compiler_flags(args, plat, toolchain)
    if plat ~= "linux" or toolchain then
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

-- Build a windres wrapper backed by llvm-windres (or mingw-w64 windres) +
-- Zig's MinGW headers; invoking `zig rc` directly hangs on non-Windows hosts.
local function ensure_mingw_windres(cfgdir, arch)
    local gnu_arch = ({
        x86_64 = "x86_64",
        x64 = "x86_64",
        i386 = "i686",
        x86 = "i686",
        arm64 = "aarch64",
        aarch64 = "aarch64",
    })[arch]
    assert(gnu_arch, "unsupported mingw arch for zigcc windres: " .. tostring(arch))

    local zig = find_tool("zig")
    assert(zig, "zigcc cross-compilation requested, but 'zig' was not found in PATH")
    local env_out = os.iorunv(zig.program, {"env"})
    local zig_lib = env_out and (env_out:match('%.lib_dir%s*=%s*"([^"]+)"') or env_out:match('"lib_dir"%s*:%s*"([^"]+)"'))
    assert(zig_lib and os.isdir(zig_lib), "failed to locate zig lib_dir from `zig env`")

    local llvm_windres = find_program("llvm-windres")
    local gnu_windres = not llvm_windres and find_program(gnu_arch .. "-w64-mingw32-windres") or nil
    local windres = llvm_windres or gnu_windres
    assert(windres, "mingw cross-compilation requires llvm-windres (or " .. gnu_arch .. "-w64-mingw32-windres) for RC files")

    local bindir = path.join(cfgdir, "bin")
    local extra_flags = ""
    if llvm_windres then
        extra_flags = string.format(' --target=%s-w64-mingw32', gnu_arch)
        if not find_program("clang") then
            local cc_shim = path.join(bindir, "zig-cc")
            local cc_content = string.format('#!/bin/sh\nexec "%s" cc "$@"\n', zig.program)
            if not os.isfile(cc_shim) or io.readfile(cc_shim) ~= cc_content then
                os.mkdir(bindir)
                io.writefile(cc_shim, cc_content)
                os.runv("chmod", {"+x", cc_shim})
            end
            extra_flags = extra_flags .. string.format(' --preprocessor="%s"', cc_shim)
        end
    end

    local wrapper = path.join(bindir, "windres")
    local content = string.format(
        '#!/bin/sh\nexec "%s"%s -I "%s/libc/include/any-windows-any" "$@"\n',
        windres, extra_flags, zig_lib
    )
    if not os.isfile(wrapper) or io.readfile(wrapper) ~= content then
        os.mkdir(bindir)
        io.writefile(wrapper, content)
        os.runv("chmod", {"+x", wrapper})
    end
    return wrapper
end

local function configure_host(opts)
    opts = opts or {}
    local plat = opts.plat or os.host()
    local mode = opts.mode or option.get("mode") or "release"
    local toolchain = opts.toolchain or option.get("toolchain")

    if plat == "macosx" and not is_host("macosx") then
        raise("cross-compiling macosx from %s is unsupported: ncnn requires moltenvk/xcodebuild and macOS SDK frameworks not shipped with zig cc", os.host())
    end
    if plat == "windows" and (not is_host("windows") or toolchain == "zigcc") and (not toolchain or toolchain == "zigcc") then
        print("note: mapping -p windows to -p mingw for zigcc toolchain on %s", os.host())
        plat = "mingw"
    end

    local arch = normalize_arch(plat, opts.arch or os.arch())
    local cross = plat ~= os.host() or arch ~= os.arch()
    if cross and plat ~= "android" and plat ~= "macosx" and not (is_host("windows") and plat == "windows") and not toolchain then
        toolchain = "zigcc"
    end

    local cfgdir = target_cfgdir(plat, arch, toolchain)
    local args = {"config", "-P", os.projectdir(), "-p", plat, "-a", arch, "-m", mode, "--yes"}
    append_avif_flags(args)
    append_compiler_flags(args, plat, toolchain)
    if toolchain then
        table.insert(args, "--toolchain=" .. toolchain)
    end
    if toolchain == "zigcc" and plat == "mingw" and not is_host("windows") then
        table.insert(args, "--mrc=" .. ensure_mingw_windres(cfgdir, arch))
    end
    if opts.native_isa ~= nil then
        table.insert(args, "--native_isa=" .. to_yn(opts.native_isa, true))
    elseif cross then
        table.insert(args, "--native_isa=n")
    end
    if opts.upx ~= nil then
        table.insert(args, "--upx=" .. (opts.upx and "y" or "n"))
    end
    table.insert(args, "--gui=" .. to_yn(opts.gui, true))
    run_xmake(cfgdir, args)
    return cfgdir, plat, arch, cross
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
    local cfgdir, _, _, cross = configure_host({
        plat = option.get("plat"),
        arch = option.get("arch"),
        native_isa = option.get("native-isa"),
        toolchain = option.get("toolchain"),
    })
    run_xmake(cfgdir, {"build", "-P", os.projectdir(), "upix"})
    if not option.get("build") and not cross then
        run_xmake(cfgdir, {"run", "-P", os.projectdir(), "upix"})
    end
end

function cli()
    local cfgdir = configure_host({
        plat = option.get("plat"),
        arch = option.get("arch"),
        native_isa = option.get("native-isa"),
        gui = option.get("gui"),
        upx = option.get("upx"),
        toolchain = option.get("toolchain"),
    })
    run_xmake(cfgdir, {"build", "-P", os.projectdir(), "upix-cli"})
end

function spirv()
    local pack = path.join(os.projectdir(), "share", "core", "spirv_pack.bin")
    if os.isfile(pack) and not option.get("force") then
        print("spirv pack present: %s (%d bytes)", pack, os.filesize(pack))
        return
    end
    local cfgdir = configure_host({mode = "release"})
    run_xmake(cfgdir, {"build", "-P", os.projectdir(), "dump-spirv"})
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
    local cfgdir, _, _, cross = configure_host({
        plat = option.get("plat"),
        arch = option.get("arch"),
        native_isa = option.get("native-isa"),
        toolchain = option.get("toolchain"),
    })
    local targets = {}
    for _, suite in ipairs({"share", "cli", "desktop"}) do
        for _, file in ipairs(os.files(path.join(os.projectdir(), suite, "tests", "test_*.cpp"))) do
            table.insert(targets, path.basename(file))
        end
    end
    table.sort(targets)
    assert(#targets > 0, "no test_*.cpp targets discovered")
    local build_args = {"build", "-P", os.projectdir()}
    for _, t in ipairs(targets) do
        table.insert(build_args, t)
    end
    run_xmake(cfgdir, build_args)
    if not option.get("build") and not cross then
        run_xmake(cfgdir, {"test", "-P", os.projectdir(), "--yes"})
    end
end
