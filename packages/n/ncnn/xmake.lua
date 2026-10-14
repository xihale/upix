package("ncnn")
    set_homepage("https://github.com/Tencent/ncnn")
    set_description("High-performance neural network inference framework optimized for the mobile platform")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/Tencent/ncnn/archive/refs/tags/$(version).tar.gz",
            "https://github.com/Tencent/ncnn.git", { submodules = false })

    add_versions("20260526", "da1ade826bc22858a9fb87ae052789bbd614d042b3ec2c22e6544ca83db6bc04")
    add_versions("20260113", "2fdc5c6e37f8552921a9daad498a1be54a6fa6edd32c1a9e3030b27fab253b47")
    add_versions("20250916", "7d463f1e5061facd02b8af5e792e059088695cdcfcc152c8f4892f6ffe5eab1a")
    add_versions("20250503", "3afea4cf092ce97d06305b72c6affbcfb3530f536ae8e81a4f22007d82b729e9")

    add_configs("vulkan",        {description = "Enable Vulkan support", default = false, type = "boolean"})
    add_configs("openmp",        {description = "Enable OpenMP support", default = true, type = "boolean"})
    add_configs("threads",       {description = "Enable threads support", default = true, type = "boolean"})
    add_configs("c_api",         {description = "Build ncnn with C api", default = false, type = "boolean"})
    add_configs("online_spirv",  {description = "Compile SPIR-V online with glslang", default = true, type = "boolean"})
    add_configs("skip_device_defines", {description = "Omit live GpuInfo macros from SPIR-V", default = false, type = "boolean"})
    -- x86 kernel dispatch variants: "auto" probes the build host's CPU and
    -- skips compiling variants it could never dispatch to (they are
    -- runtime-dispatched, so this is dead weight). Falls back to upstream
    -- defaults when the host cannot be probed, and is a no-op on non-x86.
    add_configs("x86_isa",      {description = "x86 kernel ISA set (auto = detect build host)", default = "auto", type = "string", values = {"auto", "all"}})

    add_configs("simpleomp",     {description = "Enable minimal openmp runtime emulation", default = false, type = "boolean"})
    add_configs("simpleocv",     {description = "Enable minimal opencv structure emulation", default = false, type = "boolean"})
    add_configs("simplestl",     {description = "Enable minimal cpp stl structure emulation", default = false, type = "boolean"})
    add_configs("simplemath",    {description = "Enable minimal cmath", default = false, type = "boolean"})

    add_configs("pixel",         {description = "Enable pixel convert and resize", default = true, type = "boolean"})
    add_configs("pixel_rotate",  {description = "Enable pixel rotate", default = true, type = "boolean"})
    add_configs("pixel_affine",  {description = "Enable pixel affine", default = true, type = "boolean"})
    add_configs("pixel_drawing", {description = "Enable pixel drawing", default = true, type = "boolean"})

    add_deps("cmake")

    add_includedirs("include/ncnn")

    if is_plat("android") then
        add_syslinks("android")
    end

    if on_check then
        on_check("android", function (package)
            local ndk = package:toolchain("ndk")
            local ndk_sdkver = ndk:config("ndk_sdkver")
            assert(ndk_sdkver and tonumber(ndk_sdkver) > 25, "package(ncnn): need ndk api level > 25")
        end)
    end

    on_load(function (package)
        local ncnn_ver = package:version()
        -- glslang-nihui releases normally mirror ncnn tags one-to-one. ncnn
        -- 20260526 never got a matching glslang-nihui release, but its glslang
        -- submodule is the exact commit xrepo's glslang-nihui 20260113 pins
        -- (fe88f421), so map it there instead of failing resolution.
        local glslang_versions = {["20260526"] = "20260113"}
        local glslang_ver = ncnn_ver and glslang_versions[tostring(ncnn_ver)] or ncnn_ver
        local glslang = "glslang-nihui" .. (glslang_ver and (" " .. glslang_ver) or "")
        local want_glslang = package:config("vulkan") and package:config("online_spirv")
        if want_glslang then
            package:add("deps", glslang)
        end
        if package:config("vulkan") then
            if package:is_plat("macosx", "iphoneos") then
                local vk_driver
                if ncnn_ver and ncnn_ver:ge("20260113") then
                    local icd = os.getenv("VK_ICD_FILENAMES")
                    local ncnn_vk_driver = os.getenv("NCNN_VULKAN_DRIVER")
                    if icd then
                        package:addenv("VK_ICD_FILENAMES", icd)
                        wprint("package(ncnn): Environment variable '%s' detected. If ncnn fails to build, please unset this variable and retry.", "VK_ICD_FILENAMES")
                    end
                    if ncnn_vk_driver then
                        package:addenv("NCNN_VULKAN_DRIVER", ncnn_vk_driver)
                        wprint("package(ncnn): Environment variable '%s' detected. If ncnn fails to build, please unset this variable and retry.", "NCNN_VULKAN_DRIVER")
                    end
                    vk_driver = icd or ncnn_vk_driver
                end
                package:add("deps", "moltenvk", {configs = {vk_driver = vk_driver}})
                package:add("frameworks", "Metal", "Foundation", "QuartzCore", "CoreGraphics", "IOSurface")
                if package:is_plat("macosx") then
                    package:add("frameworks", "IOKit", "AppKit")
                else
                    package:add("frameworks", "UIKit")
                end
            end
        end

        if package:is_plat("linux", "bsd") and package:config("threads") then
            package:add("syslinks", "pthread")
        end

        if package:config("openmp") and not package:config("simpleomp") then
            if package:is_plat("mingw", "msys") and not is_subhost("macosx") then
                package:add("ldflags", "-fopenmp")
            end
            if package:is_plat("linux", "macosx", "cross", "android", "mingw", "msys", "bsd") then
                package:add("deps", "libomp")
            end
        end
        if package:config("simpleomp") then
            package:config_set("openmp", true)
        end
    end)

    on_install(function (package)
        local function must_replace(path, old, new)
            local ok = io.replace(path, old, new, {plain = true})
            assert(ok, "ncnn overlay failed to patch " .. path)
        end

        must_replace("CMakeLists.txt",
            'option(NCNN_VULKAN "vulkan compute support" OFF)',
            'option(NCNN_VULKAN "vulkan compute support" OFF)\noption(NCNN_ONLINE_SPIRV "compile spirv online via glslang" ON)\noption(NCNN_SKIP_DEVICE_DEFINES "omit live GpuInfo macros from spirv compile" OFF)')
        must_replace("CMakeLists.txt",
            "if(NCNN_VULKAN)\n    if(NCNN_SYSTEM_GLSLANG)",
            "if(NCNN_VULKAN AND NCNN_ONLINE_SPIRV)\n    if(NCNN_SYSTEM_GLSLANG)")
        must_replace("src/platform.h.in",
            "#cmakedefine01 NCNN_VULKAN",
            "#cmakedefine01 NCNN_VULKAN\n#cmakedefine01 NCNN_ONLINE_SPIRV\n#cmakedefine01 NCNN_SKIP_DEVICE_DEFINES")
        must_replace("src/CMakeLists.txt",
[[    # link in-house glslang
    target_include_directories(ncnn PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../>)
    target_link_libraries(ncnn PRIVATE glslang SPIRV)
endif()]],
[[    if(NCNN_ONLINE_SPIRV)
        # link in-house glslang
        target_include_directories(ncnn PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../>)
        target_link_libraries(ncnn PRIVATE glslang SPIRV)
    endif()
endif()]])

        must_replace("src/gpu.h",
[[// online spirv compilation
NCNN_EXPORT int compile_spirv_module(const char* comp_string, const Option& opt, std::vector<uint32_t>& spirv);
NCNN_EXPORT int compile_spirv_module(const char* comp_data, int comp_data_size, const Option& opt, std::vector<uint32_t>& spirv);
NCNN_EXPORT int compile_spirv_module(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv);]],
[[// online spirv compilation
NCNN_EXPORT int compile_spirv_module(const char* comp_string, const Option& opt, std::vector<uint32_t>& spirv);
NCNN_EXPORT int compile_spirv_module(const char* comp_data, int comp_data_size, const Option& opt, std::vector<uint32_t>& spirv);
NCNN_EXPORT int compile_spirv_module(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv);
NCNN_EXPORT void set_compile_spirv_module_lookup(
    int (*index_fn)(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv),
    int (*source_fn)(const char* comp_data, int comp_data_size, const Option& opt, std::vector<uint32_t>& spirv));]])

        must_replace("src/gpu.cpp",
[[#include "glslang/SPIRV/GlslangToSpv.h"
#if NCNN_SYSTEM_GLSLANG
#include "glslang/Public/ShaderLang.h"
#else
#include "glslang/glslang/Public/ShaderLang.h"
#endif]],
[[#if NCNN_ONLINE_SPIRV
#include "glslang/SPIRV/GlslangToSpv.h"
#if NCNN_SYSTEM_GLSLANG
#include "glslang/Public/ShaderLang.h"
#else
#include "glslang/glslang/Public/ShaderLang.h"
#endif
#endif]])

        must_replace("src/gpu.cpp",
[[static const int layer_shader_registry_entry_count = sizeof(layer_shader_registry) / sizeof(layer_shader_registry_entry);]],
[[static const int layer_shader_registry_entry_count = sizeof(layer_shader_registry) / sizeof(layer_shader_registry_entry);

static int (*g_compile_spirv_index_lookup)(int, const Option&, std::vector<uint32_t>&) = 0;
static int (*g_compile_spirv_source_lookup)(const char*, int, const Option&, std::vector<uint32_t>&) = 0;

void set_compile_spirv_module_lookup(
    int (*index_fn)(int, const Option&, std::vector<uint32_t>&),
    int (*source_fn)(const char*, int, const Option&, std::vector<uint32_t>&))
{
    g_compile_spirv_index_lookup = index_fn;
    g_compile_spirv_source_lookup = source_fn;
}]])

        must_replace("src/gpu.cpp",
            "    g_instance.glslang_initialized = glslang::InitializeProcess();",
[[#if NCNN_ONLINE_SPIRV
    g_instance.glslang_initialized = glslang::InitializeProcess();
#else
    g_instance.glslang_initialized = false;
#endif]])
        must_replace("src/gpu.cpp",
[[    if (g_instance.glslang_initialized)
    {
        glslang::FinalizeProcess();
        g_instance.glslang_initialized = false;
    }]],
[[#if NCNN_ONLINE_SPIRV
    if (g_instance.glslang_initialized)
    {
        glslang::FinalizeProcess();
        g_instance.glslang_initialized = false;
    }
#endif]])

        must_replace("src/gpu.cpp",
[[static TBuiltInResource get_default_TBuiltInResource()]],
[[#if NCNN_ONLINE_SPIRV
static TBuiltInResource get_default_TBuiltInResource()]])

        must_replace("src/gpu.cpp",
[[int compile_spirv_module(const char* comp_string, const Option& opt, std::vector<uint32_t>& spirv)
{
    // -1 for omitting the tail '\0'
    int length = strlen(comp_string) - 1;
    return compile_spirv_module(comp_string, length, opt, spirv);
}]],
[[#endif // NCNN_ONLINE_SPIRV

int compile_spirv_module(const char* comp_string, const Option& opt, std::vector<uint32_t>& spirv)
{
    // -1 for omitting the tail '\0'
    int length = strlen(comp_string) - 1;
    return compile_spirv_module(comp_string, length, opt, spirv);
}]])

        must_replace("src/gpu.cpp",
[[int compile_spirv_module(const char* comp_data, int comp_data_size, const Option& opt, std::vector<uint32_t>& spirv)
{
    DefinitionCollector custom_defines;]],
[[int compile_spirv_module(const char* comp_data, int comp_data_size, const Option& opt, std::vector<uint32_t>& spirv)
{
    if (g_compile_spirv_source_lookup)
    {
        int ret = g_compile_spirv_source_lookup(comp_data, comp_data_size, opt, spirv);
        if (ret == 0)
            return 0;
    }
#if !NCNN_ONLINE_SPIRV
    (void)comp_data;
    (void)comp_data_size;
    (void)opt;
    (void)spirv;
    return -1;
#else
    DefinitionCollector custom_defines;]])

        must_replace("src/gpu.cpp",
[[    // fill device macros
    {]],
[[#if NCNN_SKIP_DEVICE_DEFINES
#else
    // fill device macros
    {]])

        must_replace("src/gpu.cpp",
[[#undef DD_APPEND_PROPERTY
    }

    std::string define_macro_data;]],
[[#undef DD_APPEND_PROPERTY
    }
#endif // NCNN_SKIP_DEVICE_DEFINES

    std::string define_macro_data;]])

        must_replace("src/gpu.cpp",
[[    return compile_success ? 0 : -1;
}

int compile_spirv_module(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv)
]],
[[    return compile_success ? 0 : -1;
#endif
}

int compile_spirv_module(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv)
]])

        must_replace("src/gpu.cpp",
[[int compile_spirv_module(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv)
{
    if (shader_type_index < 0 || shader_type_index >= layer_shader_registry_entry_count)
    {
        NCNN_LOGE("no such shader module %d", shader_type_index);
        return -1;
    }]],
[[int compile_spirv_module(int shader_type_index, const Option& opt, std::vector<uint32_t>& spirv)
{
    if (g_compile_spirv_index_lookup)
    {
        int ret = g_compile_spirv_index_lookup(shader_type_index, opt, spirv);
        if (ret == 0)
            return 0;
    }
#if !NCNN_ONLINE_SPIRV
    (void)opt;
    (void)spirv;
    return -1;
#endif
    if (shader_type_index < 0 || shader_type_index >= layer_shader_registry_entry_count)
    {
        NCNN_LOGE("no such shader module %d", shader_type_index);
        return -1;
    }]])

        -- vkCreateBuffer/vkCreateImage OOM returns a null handle; the fastMalloc
        -- sites below fed it straight into vkGet*MemoryRequirements, which the
        -- Adreno driver dereferences → SIGSEGV instead of a clean error return.
        must_replace("src/allocator.cpp",
[[    block->buffer = create_buffer(new_block_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    block->offset = 0;]],
[[    block->buffer = create_buffer(new_block_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!block->buffer)
    {
        delete block;
        return 0;
    }
    block->offset = 0;]])
        must_replace("src/allocator.cpp",
[[    block->buffer = create_buffer(new_block_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    block->offset = 0;]],
[[    block->buffer = create_buffer(new_block_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!block->buffer)
    {
        delete block;
        return 0;
    }
    block->offset = 0;]])
        must_replace("src/allocator.cpp",
[[    ptr->buffer = create_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ptr->offset = 0;]],
[[    ptr->buffer = create_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!ptr->buffer)
    {
        delete ptr;
        return 0;
    }
    ptr->offset = 0;]])
        must_replace("src/allocator.cpp",
[[    ptr->buffer = create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ptr->offset = 0;]],
[[    ptr->buffer = create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!ptr->buffer)
    {
        delete ptr;
        return 0;
    }
    ptr->offset = 0;]])
        must_replace("src/allocator.cpp",
[[    ptr->image = create_image(width, height, depth, format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    ptr->width = width;]],
[[    ptr->image = create_image(width, height, depth, format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!ptr->image)
    {
        delete ptr;
        return 0;
    }

    ptr->width = width;]])
        must_replace("src/allocator.cpp",
[[    ptr->image = create_image(width, height, depth, format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    ptr->width = width;]],
[[    ptr->image = create_image(width, height, depth, format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!ptr->image)
    {
        delete ptr;
        return 0;
    }

    ptr->width = width;]])

        local moltenvk = package:dep("moltenvk")
        if moltenvk and not moltenvk:config("shared") then
            io.replace("src/CMakeLists.txt", "if(NOT NCNN_SHARED_LIB AND APPLE)", "if(APPLE)", {plain = true})
            io.replace("src/CMakeLists.txt", "                if(NOT NCNN_SHARED_LIB)", "                if(1)", {plain = true})
        end
        local configs = {
            "-DNCNN_BUILD_EXAMPLES=OFF",
            "-DNCNN_BUILD_TOOLS=OFF",
            "-DNCNN_BUILD_BENCHMARK=OFF",
            "-DNCNN_BUILD_TESTS=OFF",
            "-DNCNN_PYTHON=OFF",
            "-DNCNN_SYSTEM_GLSLANG=ON",
        }
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"))
        table.insert(configs, "-DNCNN_SHARED_LIB=" .. (package:config("shared") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_VULKAN=" .. (package:config("vulkan") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_ONLINE_SPIRV=" .. ((package:config("vulkan") and package:config("online_spirv")) and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_SKIP_DEVICE_DEFINES=" .. (package:config("skip_device_defines") and "ON" or "OFF"))
        if package:config("x86_isa") == "auto" and os.host() == "linux" then
            -- try's return protocol differs between scopes; assign via closure.
            local flags
            try {
                function ()
                    flags = os.iorun("grep -m1 '^flags' /proc/cpuinfo")
                end,
                catch {
                    function () flags = nil end
                }
            }
            if type(flags) == "string" then
                local padded = " " .. flags:gsub("%s+", " ") .. " "
                local function has(flag)
                    return padded:find(" " .. flag .. " ", 1, true) ~= nil
                end
                local avx512 = has("avx2") and has("fma")
                        and has("avx512f") and has("avx512cd")
                        and has("avx512bw") and has("avx512dq") and has("avx512vl")
                table.insert(configs, "-DNCNN_AVX512=" .. (avx512 and "ON" or "OFF"))
                table.insert(configs, "-DNCNN_XOP=" .. (has("xop") and "ON" or "OFF"))
            end
        end
        table.insert(configs, "-DNCNN_OPENMP=" .. (package:config("openmp") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_THREADS=" .. (package:config("threads") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_C_API=" .. (package:config("c_api") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_SIMPLEOMP=" .. (package:config("simpleomp") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_SIMPLEOCV=" .. (package:config("simpleocv") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_SIMPLESTL=" .. (package:config("simplestl") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_SIMPLEMATH=" .. (package:config("simplemath") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_PIXEL=" .. (package:config("pixel") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_PIXEL_ROTATE=" .. (package:config("pixel_rotate") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_PIXEL_AFFINE=" .. (package:config("pixel_affine") and "ON" or "OFF"))
        table.insert(configs, "-DNCNN_PIXEL_DRAWING=" .. (package:config("pixel_drawing") and "ON" or "OFF"))
        if package:config("vulkan") then
            table.insert(configs, "-DCMAKE_CXX_STANDARD=11")
            if package:version() and package:version():lt("20260113") and moltenvk then
                table.insert(configs, "-DVulkan_LIBRARY=" .. path.join(moltenvk:installdir("lib"), "libMoltenVK." .. (moltenvk:config("shared") and "dylib" or "a")))
            end
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        if not package:config("c_api") then
            assert(package:check_cxxsnippets({test = [[
                #include <net.h>
                void test() {
                    ncnn::Net net;
                    net.load_param("model.param");
                }
            ]]}, {configs = package:config("vulkan") and {languages = "c++11"} or {}}))
        else
            assert(package:check_csnippets({test = [[
                #include <c_api.h>
                void test() {
                    const char* ver = ncnn_version();
                }
            ]]}))
        end
    end)
