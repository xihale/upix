#include "app_state.hpp"
#include "i18n.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__,  \
                         #expr);                                                    \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace fs = std::filesystem;

namespace {

void writeDummyFile(const fs::path& p, const std::string& content = "stub") {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    setenv(name, value, 1);
#endif
}

void unsetEnvVar(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

int main() {
    CHECK(std::string(upix::gui::Snapshot::kDefaultModel) == "Real-ESRGANv3-anime-x2");
    CHECK(upix::gui::Snapshot{}.model == "Real-ESRGANv3-anime-x2");

    const fs::path tmpRoot = fs::temp_directory_path() / "upix_test_app_state_jobs";
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    const fs::path prefsFile = tmpRoot / "prefs.json";
    const fs::path inputDir = tmpRoot / "photos";
    writeDummyFile(inputDir / "a.png");
    writeDummyFile(inputDir / "b.jpg");
    writeDummyFile(inputDir / "notes.txt");
    writeDummyFile(inputDir / "sub" / "c.webp");
    // Must be skipped even when recursive=true to prevent re-upscaling output dirs
    writeDummyFile(inputDir / "upix_AnimeSharp-x4" / "already.png");

    // Disable network model refresh interference cleanly using UPIX_FAKE
    setEnvVar("UPIX_FAKE", "1");
    const std::string modelsDir = (tmpRoot / "models").string();
    setEnvVar("UPIX_MODELS_DIR", modelsDir.c_str());

    {
        upix::gui::AppState app(prefsFile.string());

        // 1. Folder scan: non-recursive finds 2 (a.png, b.jpg)
        app.addFolder(inputDir.string(), false);
        auto s1 = app.snapshot();
        CHECK(s1.inputs.size() == 1);
        CHECK(s1.inputs[0].is_dir);
        CHECK(s1.inputs[0].count == 2);

        // Recursive skips upix_* directory, so finds a.png, b.jpg, sub/c.webp = 3
        app.clearInputs();
        app.addFolder(inputDir.string(), true);
        auto s2 = app.snapshot();
        CHECK(s2.inputs.size() == 1);
        CHECK(s2.inputs[0].count == 3);

        // 2. Duplicate path ignored, missing/unsupported rejected
        app.addPaths({(inputDir / "a.png").string(),
                      (inputDir / "a.png").string(),
                      (inputDir / "notes.txt").string(),
                      (inputDir / "missing.png").string()});
        auto s3 = app.snapshot();
        CHECK(s3.inputs.size() == 2);

        // 3. Clipboard text path parsing (file:// + quotes)
        app.clearInputs();
        std::string clip = "  \"file://" + (inputDir / "a.png").string() + "\"  \n" +
                           (inputDir / "b.jpg").string() + "\n";
        CHECK(app.addFromClipboardText(clip));
        CHECK(app.snapshot().inputs.size() == 2);

        // 4. Overwrite-in-place locks format to Follow ("")
        app.setFormat("avif");
        CHECK(app.snapshot().format == "avif");
        app.setOverwriteOriginal(true);
        CHECK(app.snapshot().overwrite_original == true);
        CHECK(app.snapshot().format.empty());
        app.setFormat("webp");
        CHECK(app.snapshot().format.empty());
        app.setOverwriteOriginal(false);
        app.setFormat("webp");
        CHECK(app.snapshot().format == "webp");

        // 5. Validation & clamping
        app.setQuality(150);
        CHECK(app.snapshot().quality == 100);
        app.setQuality(-10);
        CHECK(app.snapshot().quality == 1);
        app.setQuality(85);
        app.setHostPort(99999);
        CHECK(app.snapshot().host.port == 9527);
        app.setHostPort(9600);
        CHECK(app.snapshot().host.port == 9600);
        app.setGpu(false);
        app.setTileSize(256);
        app.setDarkMode(false);
        app.setReduceMotion(true);
        app.setDaemonIdleSecs(900);
        app.setModel("UltraSharp-x4");
        app.setBackupOriginal(false);
        app.setLanguage("zh-CN");
        CHECK(app.snapshot().language == "zh-CN");
        CHECK(upix::i18n::current_locale() == upix::i18n::Locale::ZhCn);
        app.setLanguage("en");
        CHECK(app.snapshot().language == "en");
        CHECK(upix::i18n::current_locale() == upix::i18n::Locale::En);
        app.setLanguage("zh-CN");

        // 6. Missing model check on startJob
        app.startJob();
        auto errSnap = app.snapshot();
        CHECK(errSnap.job.phase == upix::gui::JobPhase::Error);
        CHECK(errSnap.job.missing_model == "UltraSharp-x4");

        // 7. Running state blocks queue mutations & transitions Running -> Cancelling
        app.applyFixture("running");
        const size_t runningCount = app.snapshot().inputs.size();
        app.removeInput("shot_01.png");
        app.clearInputs();
        app.addPaths({(inputDir / "a.png").string()});
        CHECK(app.snapshot().inputs.size() == runningCount);
        app.cancelJob();
        CHECK(app.snapshot().job.phase == upix::gui::JobPhase::Cancelling);
    }

    // 8. Prefs persistence across AppState instances
    unsetEnvVar("UPIX_FAKE");
    {
        upix::gui::AppState app2(prefsFile.string());
        auto loaded = app2.snapshot();
        CHECK(loaded.model == "UltraSharp-x4");
        CHECK(loaded.format == "webp");
        CHECK(loaded.quality == 85);
        CHECK(loaded.gpu == false);
        CHECK(loaded.tile_size == 256);
        CHECK(loaded.dark_mode == false);
        CHECK(loaded.reduce_motion == true);
        CHECK(loaded.host.port == 9600);
        CHECK(loaded.daemon.idle_secs == 900);
        CHECK(loaded.backup_original == false);
        CHECK(loaded.language == "zh-CN");
        CHECK(upix::i18n::current_locale() == upix::i18n::Locale::ZhCn);
        app2.setLanguage("auto");
        CHECK(app2.snapshot().language == "auto");
    }

    // 9. Reload "auto" preference cleanly
    {
        upix::gui::AppState app3(prefsFile.string());
        CHECK(app3.snapshot().language == "auto");
        CHECK(upix::i18n::preference() == upix::i18n::LangPref::Auto);
    }

    fs::remove_all(tmpRoot, ec);
    std::printf("test_app_state_jobs: PASS\n");
    return 0;
}
