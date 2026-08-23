-- JNI shared library. after_build writes jniLibs/ for Gradle to package.
if not is_plat("android") then
    return
end

target("crossscale-native")
    set_kind("shared")
    add_deps("crossscale-core")
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
        if ndk and triple then
            local found = os.files(path.join(ndk, "toolchains/llvm/prebuilt/*/sysroot/usr/lib", triple, "libc++_shared.so"))
            if #found > 0 then
                os.cp(found[1], dest)
            end
        end
        print("jniLibs <- " .. dest)
    end)
