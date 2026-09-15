-- Vendored from xmake-repo (packages/l/libwebp): upstream xrepo stopped at
-- v1.3.0 (2023), so a floating require silently embedded a WebP decoder
-- missing the CVE-2023-4863 fix (1.3.2) into the Android APK. v1.6.0 is the
-- current stable line (Arch system package is 1.6.0; the host build links it
-- via extsources). This local repo shadows the global one, same as ncnn.
-- Pre-1.3.0 versions are dropped along with their DLL-export patch; the full
-- tool config list must stay: the build loop below relies on passing each
-- WEBP_BUILD_* flag explicitly, since libwebp's own CMake defaults enable
-- example targets that need system image codecs (png.h) Android lacks.
package("libwebp")

    set_homepage("https://chromium.googlesource.com/webm/libwebp/")
    set_description("Library to encode and decode images in WebP format.")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/webmproject/libwebp/archive/$(version).tar.gz",
             "https://github.com/webmproject/libwebp.git")

    add_versions("v1.3.0", "dc9860d3fe06013266c237959e1416b71c63b36f343aae1d65ea9c94832630e1")
    add_versions("v1.6.0", "93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136")

    add_configs("anim_utils", {description = "Build animation utilities.", default = false, type = "boolean"})
    add_configs("cwebp",      {description = "Build the cwebp command line tool.", default = false, type = "boolean"})
    add_configs("dwebp",      {description = "Build the dwebp command line tool.", default = false, type = "boolean"})
    add_configs("gif2webp",   {description = "Build the gif2webp conversion tool.", default = false, type = "boolean"})
    add_configs("img2webp",   {description = "Build the img2webp animation tool.", default = false, type = "boolean"})
    add_configs("vwebp",      {description = "Build the vwebp viewer tool.", default = false, type = "boolean"})
    add_configs("webpinfo",   {description = "Build the webpinfo command line tool.", default = false, type = "boolean"})
    add_configs("libwebpmux", {description = "Build the libwebpmux library.", default = false, type = "boolean"})
    add_configs("webpmux",    {description = "Build the webpmux command line tool.", default = false, type = "boolean"})
    add_configs("sharpyuv",   {description = "Build the sharpyuv library.", default = false, type = "boolean"})
    add_configs("extras",     {description = "Build extras.", default = false, type = "boolean"})
    add_configs("thread",     {description = "Enable threading support.", default = true, type = "boolean"})

    add_deps("cmake")

    if is_plat("macosx") then
        add_extsources("brew::webp")
    elseif is_plat("linux") then
        add_extsources("apt::libwebp-dev", "pacman::libwebp")
    end

    on_load(function (package)
        local links = {"webpdecoder", "webpencoder", "webpdemux"}
        if package:config("libwebpmux") then
            table.insert(links, "webpmux")
        end
        table.insert(links, "webp")
        if package:config("sharpyuv") or package:version():ge("1.2.3") then
            table.insert(links, "sharpyuv")
        end

        for _, l in ipairs(links) do
            package:add("links", (package:is_plat("windows") and "lib" or "") .. l)
        end
    end)

    on_install(function (package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))

        for name, enabled in pairs(package:configs()) do
            if name == "thread" then
                if enabled then
                    package:add("defines", "WEBP_USE_THREAD")
                    if package:is_plat("linux", "bsd") then
                        package:add("syslinks", "pthread")
                    end
                end
            elseif not package:extraconf("configs", name, "builtin") then
                table.insert(configs, "-DWEBP_BUILD_" .. name:upper() .. "=" .. (enabled and "ON" or "OFF"))
            end
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("WebPGetEncoderVersion", {includes = "webp/encode.h"}))
        if package:config("libwebpmux") then
            assert(package:has_cfuncs("WebPGetMuxVersion", {includes = "webp/mux.h"}))
        end
    end)
