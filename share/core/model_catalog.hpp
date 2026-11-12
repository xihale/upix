// Published offline model catalog — single source of truth for CLI, Desktop,
// and offline fallbacks (mirrors crates/upix-core/src/model_catalog.rs).
// Header-only and self-contained so standalone targets (e.g. test_model_downloader)
// can consume it without extra link dependencies.
#pragma once

#include <array>
#include <string_view>

namespace upix {

inline constexpr std::array<std::string_view, 34> kModelCatalog = {{
    "AnimeJaNai-v2-Compact-x2",
    "AnimeSharp-x4",
    "Archivist-Medium-Compact-x1",
    "Archivist-Rough-Compact-x1",
    "BSRGAN-x2",
    "BSRGAN-x4",
    "DeH264-RealPLKSR-x1",
    "DeJPG-RealPLKSR-x1",
    "DeNoise-RealPLKSR-x1",
    "Genesis-Cleanup-Compact-x1",
    "HFA2k-LUDVAE-SPAN-x2",
    "HFA2k-x4",
    "LSDIR-Compact-v2-x4",
    "ModernSpanimation-x2",
    "NES-Composite-RGB-Compact-x1",
    "NMKD-Siax-x4",
    "Nomos2-OTF-x4",
    "Nomos8kSC-x4",
    "NomosUni-Compact-x2",
    "NomosUni-SPAN-x2",
    "NomosUni-SPAN-x4",
    "NomosWebPhoto-RealPLKSR-x4",
    "Parimg-Compact-x2",
    "Real-CUGAN-denoise-x2",
    "Real-CUGAN-denoise-x3",
    "Real-CUGAN-no-denoise-x2",
    "Real-ESRGAN-x2plus",
    "Real-ESRGAN-x4plus",
    "Real-ESRGAN-x4plus-anime",
    "Real-ESRGANv3-anime-x2",
    "Real-ESRGANv3-anime-x3",
    "Real-ESRGANv3-general-x4",
    "Remacri-x4",
    "UltraSharp-x4",
}};

constexpr auto catalog_names() -> const std::array<std::string_view, 34>& {
    return kModelCatalog;
}

constexpr auto is_known_catalog_model(std::string_view name) -> bool {
    for (std::string_view entry : kModelCatalog) {
        if (entry == name) return true;
    }
    return false;
}

// Lightweight constexpr parser matching scale_from_filename()'s last `x<1..8>` rule
// (used when populating catalog_models() without pulling model_graph.cpp into
// minimal targets such as test_model_downloader).
constexpr auto catalog_scale_hint(std::string_view name) -> int {
    int found = 0;
    for (size_t i = 0; i + 1 < name.size(); ++i) {
        if (name[i] != 'x' && name[i] != 'X') continue;
        const char d = name[i + 1];
        if (d < '0' || d > '9') continue;
        int n = 0;
        size_t j = i + 1;
        while (j < name.size() && n <= 8 && name[j] >= '0' && name[j] <= '9') {
            n = n * 10 + (name[j] - '0');
            ++j;
        }
        if (n >= 1 && n <= 8) found = n;
    }
    return found > 0 ? found : 2;
}

}  // namespace upix
