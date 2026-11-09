#include "core/model_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace upix {

namespace {

// Matching rules:
//   - Patterns are lowercase; the name is normalized the same way
//     (lowercase, '_' and ' ' → '-'), so "Real-ESRGANv3-anime_x2" and
//     "animesharp x4" both resolve.
//   - Substring match, table ordered most-specific first: "real-cugan" is
//     checked before the 1x "denoise" nets so Real-CUGAN's -denoise
//     variants keep their anime label instead of the photo-restoration one,
//     and "real-esrgan-x4plus-anime" precedes "real-esrgan-x4plus".
//   - `recommended` mirrors the picks models/README.md calls out as first
//     choices ("the default recommendation in both apps", "best photo
//     default on the phone", "first pick for PC-offloaded photo jobs",
//     "the community's default all-rounder", "recommended starting point").
struct SceneRule {
    std::string_view pattern;
    std::string_view scene;    // zh label shown verbatim by CLI and GUI
    bool recommended;
};

constexpr std::array kSceneRules{
    // Real-ESRGAN v3 compacts — the default recommendation in both apps.
    SceneRule{"real-esrganv3-anime",  "动漫", true},
    SceneRule{"real-esrganv3-general","照片", true},
    // x4plus family; the anime variant must be tested before the photo base.
    SceneRule{"real-esrgan-x4plus-anime", "动漫", false},
    SceneRule{"real-esrgan-x4plus",   "照片", true},
    SceneRule{"real-esrgan-x2plus",   "照片", false},
    // Real-CUGAN — anime specialist (before the 1x "-denoise" restores).
    SceneRule{"real-cugan",          "动漫", false},
    // Phhofm photo compacts / 4x nets.
    SceneRule{"nomosuni-compact",    "照片", false},
    SceneRule{"parimg-compact",      "照片", false},
    SceneRule{"nomosuni-span",       "照片", false},
    SceneRule{"lsdir-compact",       "照片", false},
    SceneRule{"nomos2-otf",          "照片", false},
    SceneRule{"nomos8ksc",           "照片", false},
    SceneRule{"nomoswebphoto",       "照片", false},
    // 1x restoration nets — clean in place, no resize (scale already says x1).
    SceneRule{"dejpg",               "照片·修复", false},
    SceneRule{"denoise",             "照片·修复", false},
    SceneRule{"deh264",              "视频·修复", false},
    SceneRule{"archivist-medium",    "动漫·修复", true},
    SceneRule{"archivist-rough",     "动漫·修复", false},
    SceneRule{"nes-composite",       "游戏·修复", false},
    SceneRule{"genesis-cleanup",     "游戏·修复", false},
    // Community anime restoration ladder (both HFA2k variants in one rule).
    SceneRule{"hfa2k",               "动漫", false},
    SceneRule{"modernspanimation",   "动漫", false},
    // Community classics.
    SceneRule{"ultrasharp",          "综合", true},
    SceneRule{"remacri",             "综合", true},
    SceneRule{"animesharp",          "动漫", true},
    SceneRule{"animejanai",          "动漫", false},
    SceneRule{"nmkd-siax",           "综合", false},  // photos & textures
    SceneRule{"bsrgan",              "照片", false},
};

auto contains(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

auto classify_model_scene(std::string_view name) -> ModelSceneInfo {
    std::string key;
    key.reserve(name.size());
    for (char c : name) {
        c = (c == '_' || c == ' ') ? '-'
                                   : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key.push_back(c);
    }
    const auto it = std::ranges::find_if(kSceneRules, [&](const SceneRule& r) {
        return contains(key, r.pattern);
    });
    if (it == kSceneRules.end()) return {};
    return ModelSceneInfo{it->scene, it->recommended};
}

} // namespace upix
