-- CLI target. Models are NOT bundled with the binary: they are distributed
-- via the GitHub "models" release and fetched with `upix model download`
-- (or imported locally with `upix model import`).
target("upix-cli")
    set_kind("binary")
    set_rules("upix.host_isa", "upix.upx")

    -- Single source of truth: root xmake.lua's set_version() feeds the
    -- CLI11 --version output via UPIX_VERSION (cli_app.cpp). The
    -- project module is script-scope only, hence on_load. Optional codec
    -- builds append their capabilities so `upix --version` says whether
    -- AVIF is linked in or dlopen'd at runtime.
    on_load(function (target)
        import("core.project.project")
        local caps = ""
        if has_config("avif-encoder") then
            caps = caps .. " +avif-encoder"
        end
        if has_config("avif-decoder") then
            caps = caps .. " +avif-decoder"
        end
        if caps == "" then
            caps = " avif:system-dlopen"
        end
        target:add('defines', 'UPIX_VERSION="v' .. project.version() .. caps .. '"')
    end)

    add_deps("upix-core")
    add_packages("ncnn~runtime", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium", "libcurl", "libzip")
    if has_config("avif-encoder") or has_config("avif-decoder") then
        add_packages("libavif")
    end

    add_files(
        "src/main.cpp",
        "src/cli_app.cpp",
        "src/common.cpp",
        "src/cmd_model.cpp",
        "src/cmd_host.cpp",
        "src/cmd_daemon.cpp",
        "src/cmd_completions.cpp",
        "src/cmd_complete.cpp",
        "src/cmd_process.cpp",
        "src/process_batch.cpp"
    )

-- ── Host test binaries ───────────────────────────────────────────────────────
-- Single source of truth: one standalone test = one cli/tests/test_*.cpp (own
-- main, plain CHECK/abort, no framework). The target list is discovered on the
-- filesystem here AND by scripts/lib/host_tests.sh (globs the same files), so
-- there is no list to keep in sync: adding a test file adds it to every
-- consumer. Per-target differences live in HOST_TEST_SPECS; the after_load
-- check below makes a missing/extra spec entry (or an empty file set) a
-- configure-time error.
local host_test_targets = {}
for _, file in ipairs(os.files(path.join(os.projectdir(), "cli/tests/test_*.cpp"))) do
    table.insert(host_test_targets, path.basename(file))
end
table.sort(host_test_targets)

-- Package sets shared by several test binaries.
local TEST_CODEC_PKGS = {"libwebp", "libpng", "libjpeg", "libdeflate"}
local TEST_ENGINE_PKGS = {"ncnn~runtime", "glaze", "libsodium", "libwebp", "libpng", "libjpeg", "libdeflate"}

local HOST_TEST_SPECS = {
    test_crypto = {deps = {"upix-core"}, packages = {"libsodium"}},
    test_protocol = {deps = {"upix-core"}},
    test_distributed_session = {deps = {"upix-core"}, packages = TEST_ENGINE_PKGS, syslinks = {"dl"}},
    test_sr_marker = {deps = {"upix-core"}, packages = TEST_CODEC_PKGS, avif = true, syslinks = {"dl"}},
    test_exif = {deps = {"upix-core"}, packages = TEST_CODEC_PKGS, avif = true, syslinks = {"dl"}},
    test_gpu_precision = {packages = {"ncnn~runtime"}, includedirs = {"$(projectdir)/share"}},
    test_model_downloader = {
        -- share/core/model_downloader.cpp compiles in directly; openssl is
        -- the independent sha256 oracle its libsodium implementation is
        -- cross-checked against (see root xmake.lua).
        packages = {"libcurl", "openssl", "libzip", "glaze", "libsodium"},
        includedirs = {"$(projectdir)/share"},
        extra_files = {"$(projectdir)/share/core/model_downloader.cpp"},
    },
    test_processing_config = {
        deps = {"upix-core"},
        packages = {"ncnn~runtime", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium", "libcurl", "libzip"},
        avif = true,
        includedirs = {"$(projectdir)/cli/src", "$(projectdir)/share"},
        extra_files = {"$(projectdir)/cli/src/common.cpp"},
    },
    test_machine_events = {
        -- Compiles cli/src/common.cpp in (like test_processing_config):
        -- json_escape / build_connection_card live there.
        deps = {"upix-core"},
        packages = {"ncnn~runtime", "libwebp", "glaze", "qr-code-generator-cpp", "cli11", "libsodium", "libcurl", "libzip"},
        avif = true,
        includedirs = {"$(projectdir)/cli/src", "$(projectdir)/share"},
        extra_files = {"$(projectdir)/cli/src/common.cpp"},
    },
    test_model_catalog = {deps = {"upix-core"}, packages = TEST_ENGINE_PKGS, syslinks = {"dl"}, includedirs = {"$(projectdir)/share"}},
    test_model_scene = {deps = {"upix-core"}, packages = TEST_ENGINE_PKGS, syslinks = {"dl"}, includedirs = {"$(projectdir)/share"}},
    test_image_avif = {deps = {"upix-core"}, packages = TEST_CODEC_PKGS, avif = true, syslinks = {"dl"}},
}

for _, name in ipairs(host_test_targets) do
    local spec = HOST_TEST_SPECS[name]
    if spec then
        target(name)
            set_kind("binary")
            if spec.deps then
                add_deps(spec.deps)
            end
            if spec.packages then
                add_packages(spec.packages)
            end
            if spec.avif and (has_config("avif-encoder") or has_config("avif-decoder")) then
                add_packages("libavif")
            end
            if spec.syslinks and is_plat("linux", "bsd") then
                add_syslinks(spec.syslinks)
            end
            if spec.includedirs then
                add_includedirs(spec.includedirs)
            end
            add_files("$(projectdir)/cli/tests/" .. name .. ".cpp")
            if spec.extra_files then
                add_files(spec.extra_files)
            end
            set_targetdir("$(projectdir)/cli/build")
        target_end()
    end
end

-- File <-> spec drift check. Runs in after_load because the project main chunk
-- is sandboxed (no assert/raise there, only os.files/path/table/string).
after_load(function ()
    local missing = {}
    if #host_test_targets == 0 then
        table.insert(missing, "no cli/tests/test_*.cpp found")
    end
    for _, name in ipairs(host_test_targets) do
        if not HOST_TEST_SPECS[name] then
            table.insert(missing, "cli/tests/" .. name .. ".cpp has no HOST_TEST_SPECS entry in cli/xmake.lua")
        end
    end
    for name in pairs(HOST_TEST_SPECS) do
        if not table.contains(host_test_targets, name) then
            table.insert(missing, "cli/xmake.lua has a HOST_TEST_SPECS entry for " .. name ..
                                  " but cli/tests/" .. name .. ".cpp does not exist")
        end
    end
    if #missing > 0 then
        raise("host test manifest mismatch:\n  " .. table.concat(missing, "\n  "))
    end
end)


