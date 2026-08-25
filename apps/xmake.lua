-- JNI shared library. after_build writes jniLibs/ for Gradle to package.
if not is_plat("android") then
    return
end

target("upix-native")
    set_kind("shared")
    add_deps("upix-core")
    add_packages("ncnn~runtime", "libwebp", "libpng", "libjpeg", "glaze", "libsodium", "libdeflate", "libomp")
    add_files("app/src/main/cpp/jni_bridge.cpp")
    add_includedirs("$(projectdir)/share/core/distributed")
    add_syslinks("android", "log", "vulkan")
    add_cxflags("-fexceptions", "-frtti")

    after_build(function (target)
        local abi = target:arch()
        local dest = path.join(os.projectdir(), "apps/app/src/main/jniLibs", abi)
        os.mkdir(dest)
        for _, f in ipairs(os.files(path.join(dest, "*"))) do
            os.rm(f)
        end
        os.cp(target:targetfile(), dest)

        local function copy_so(file)
            local name = path.filename(file)
            if name:endswith(".so") and name ~= "libarcher.so" then
                os.cp(file, path.join(dest, name))
            end
        end
        for _, pkg in ipairs(target:orderpkgs()) do
            local libdir = path.join(pkg:installdir(), "lib")
            if os.isdir(libdir) then
                for _, f in ipairs(os.files(path.join(libdir, "*.so"))) do
                    copy_so(f)
                end
            end
        end

        local triples = {
            ["arm64-v8a"] = "aarch64-linux-android",
            ["armeabi-v7a"] = "arm-linux-androideabi",
            ["x86_64"] = "x86_64-linux-android",
            ["x86"] = "i686-linux-android",
        }
        local triple = triples[abi]
        local ndk = get_config("ndk")
        if not ndk and triple then
            -- get_config("ndk") is only set when the project was configured
            -- with --ndk=...; fall back to xmake's own NDK detection (global
            -- config, $ANDROID_NDK_HOME / $ANDROID_NDK_ROOT, sdk/ndk-bundle)
            -- plus a bare $NDK env var so packaging still works.
            import("detect.sdks.find_ndk")
            local ndkinfo = find_ndk(nil, {arch = abi, force = true})
            if type(ndkinfo) == "table" and ndkinfo.sdkdir then
                ndk = ndkinfo.sdkdir
            else
                local env_ndk = os.getenv("NDK")
                if env_ndk and os.isdir(env_ndk) then
                    ndk = env_ndk
                end
            end
            if not ndk then
                wprint("libc++_shared.so not copied: NDK not found (configure with --ndk=... or set ANDROID_NDK_HOME/$NDK)")
            end
        end
        if ndk and triple then
            local found = os.files(path.join(ndk, "toolchains/llvm/prebuilt/*/sysroot/usr/lib", triple, "libc++_shared.so"))
            if #found > 0 then
                os.cp(found[1], dest)
            end
        end
        print("jniLibs <- " .. dest)
    end)
