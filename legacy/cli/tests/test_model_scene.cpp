// Scene catalog conformance: every one of the 44 shipped models must
// classify to its documented scene (models/README.md "Use it for") and the
// recommended picks must be exactly the ones the README calls out. Also
// pins the normalization variants (underscore/space separators) and the
// ordering traps (Real-CUGAN's -denoise variants vs the 1x DeNoise net;
// x4plus-anime vs x4plus). Pure name-table test — needs no models dir.

#include "core/model_metadata.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Case {
    std::string_view name;
    std::string_view scene;    // "" = must not classify
    bool recommended;
};

// The full release catalog (see models/README.md).
constexpr Case kCases[] = {
    // Real-ESRGAN v3 compacts — default recommendation in both apps.
    {"Real-ESRGANv3-anime-x2", "动漫", true},
    {"Real-ESRGANv3-anime-x3", "动漫", true},
    {"Real-ESRGANv3-anime-x4", "动漫", true},
    {"Real-ESRGANv3-general-x4", "照片", true},
    // x4plus family.
    {"Real-ESRGAN-x4plus", "照片", true},
    {"Real-ESRGAN-x2plus", "照片", false},
    {"Real-ESRGAN-x4plus-anime", "动漫", false},
    // Real-CUGAN se + pro, all six variants per generation.
    {"Real-CUGAN-conservative-x2", "动漫", false},
    {"Real-CUGAN-conservative-x3", "动漫", false},
    {"Real-CUGAN-denoise-x2", "动漫", false},
    {"Real-CUGAN-denoise-x3", "动漫", false},
    {"Real-CUGAN-no-denoise-x2", "动漫", false},
    {"Real-CUGAN-no-denoise-x3", "动漫", false},
    {"Real-CUGAN-pro-conservative-x2", "动漫", false},
    {"Real-CUGAN-pro-conservative-x3", "动漫", false},
    {"Real-CUGAN-pro-denoise-x2", "动漫", false},
    {"Real-CUGAN-pro-denoise-x3", "动漫", false},
    {"Real-CUGAN-pro-no-denoise-x2", "动漫", false},
    {"Real-CUGAN-pro-no-denoise-x3", "动漫", false},
    // Community photo models.
    {"NomosUni-Compact-x2", "照片", false},
    {"Parimg-Compact-x2", "照片", false},
    {"NomosUni-SPAN-x2", "照片", false},
    {"LSDIR-Compact-v2-x4", "照片", false},
    {"NomosUni-SPAN-x4", "照片", false},
    {"Nomos2-OTF-x4", "照片", false},
    {"Nomos8kSC-x4", "照片", false},
    {"NomosWebPhoto-RealPLKSR-x4", "照片", false},
    // 1x restoration nets.
    {"DeJPG-RealPLKSR-x1", "照片·修复", false},
    {"DeNoise-RealPLKSR-x1", "照片·修复", false},
    {"DeH264-RealPLKSR-x1", "视频·修复", false},
    {"Archivist-Medium-Compact-x1", "动漫·修复", true},
    {"Archivist-Rough-Compact-x1", "动漫·修复", false},
    {"NES-Composite-RGB-Compact-x1", "游戏·修复", false},
    {"Genesis-Cleanup-Compact-x1", "游戏·修复", false},
    // Community anime restoration.
    {"HFA2k-x4", "动漫", false},
    {"HFA2k-LUDVAE-SPAN-x2", "动漫", false},
    {"ModernSpanimation-x2", "动漫", false},
    // Community classics.
    {"UltraSharp-x4", "综合", true},
    {"Remacri-x4", "综合", true},
    {"AnimeSharp-x4", "动漫", true},
    {"AnimeJaNai-v2-Compact-x2", "动漫", false},
    {"NMKD-Siax-x4", "综合", false},
    {"BSRGAN-x4", "照片", false},
    {"BSRGAN-x2", "照片", false},
};

int check(const Case& c) {
    const auto got = upix::classify_model_scene(c.name);
    if (got.scene != c.scene || got.recommended != c.recommended) {
        std::cerr << "FAIL " << c.name << ": scene=\"" << got.scene
                  << "\" recommended=" << got.recommended
                  << ", expected \"" << c.scene << "\"/" << c.recommended << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    for (const auto& c : kCases) failures += check(c);

    // Separator/case normalization must resolve every spelling (the Android
    // asset layout uses '_' before the scale, the CLI shell habit uses ' ').
    const auto spaced = upix::classify_model_scene("real cugan conservative x2");
    if (spaced.scene != "动漫") {
        std::cerr << "FAIL space normalization: \"" << spaced.scene << "\"\n";
        failures++;
    }
    const auto underscored = upix::classify_model_scene("Real-ESRGANv3-anime_x4");
    if (underscored.scene != "动漫" || !underscored.recommended) {
        std::cerr << "FAIL underscore normalization: \"" << underscored.scene << "\"\n";
        failures++;
    }

    // Non-catalog models (user imports) get no label at all.
    for (std::string_view unknown : {"my-experimental-net-x2", "waifu2x-x2", "x2"}) {
        if (const auto got = upix::classify_model_scene(unknown); !got.scene.empty()) {
            std::cerr << "FAIL unknown \"" << unknown
                      << "\" classified as \"" << got.scene << "\"\n";
            failures++;
        }
    }

    if (failures != 0) {
        std::cerr << failures << " scene classification failure(s)\n";
        return 1;
    }
    std::cout << "ok " << std::size(kCases)
              << " catalog scene checks + normalization/unknown rules\n";
    return 0;
}
