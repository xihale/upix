// Model family grouping — the single source of truth
// (docs/SPEC-frontends.md §1 S3). Ports the semantics that used to live only
// in desktop/qml/Grouping.js (KNOWN table + groups()); the Android and CLI
// sides render flat lists and are unaffected.
//
// Rules (pinned by desktop/tests/tst_desk_jobs.cpp golden):
//   1. Known families: members are the exact prefix name or `prefix-*`;
//      a lone member gains nothing from a header, so it is emitted ungrouped.
//   2. Everything else groups by its stem (name minus a trailing `-xN…`),
//      in first-appearance order after an alphabetical sort; a one-member
//      stem stays ungrouped for the same reason.
//   3. Short names drop the family/stem prefix and one leading dash.
//
// Header-only on purpose: desktop consumes it without linking share objects
// (CMake include path only), keeping the Qt build free of the engine.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace upix {

inline constexpr std::string_view kKnownFamilies[][2] = {
    {"Real-ESRGANv3", "Real-ESRGAN v3"},
    {"Real-ESRGAN-x4plus", "Real-ESRGAN x4plus"},
    {"Real-CUGAN-pro", "Real-CUGAN pro"},
    {"Real-CUGAN-se", "Real-CUGAN se"},
};

// One grouped entry: the original model name and its short display name.
struct FamilyItem {
    std::string name;
    std::string short_name;
};

// A header (label empty = ungrouped run of rows) and its items.
struct FamilyGroup {
    std::string label;
    std::vector<FamilyItem> items;
};

// Stem = name minus a case-insensitive trailing `-x<N>…` ("NomosUni-SPAN-x4"
// -> "NomosUni-SPAN"); names without one are their own stem.
inline auto model_stem(std::string_view name) -> std::string_view {
    for (size_t i = 0; i + 2 < name.size(); ++i) {
        const bool at_x = name[i] == '-' &&
                          std::tolower(static_cast<unsigned char>(name[i + 1])) == 'x' &&
                          i + 2 < name.size() &&
                          std::isdigit(static_cast<unsigned char>(name[i + 2]));
        if (!at_x) continue;
        return name.substr(0, i);
    }
    return name;
}

inline auto groups(const std::vector<std::string>& names) -> std::vector<FamilyGroup> {
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());

    std::vector<FamilyGroup> out;
    std::vector<bool> used(sorted.size(), false);

    for (const auto& [prefix, label] : kKnownFamilies) {
        std::vector<size_t> members;
        for (size_t i = 0; i < sorted.size(); ++i) {
            const std::string_view n = sorted[i];
            if (n == prefix || (n.substr(0, prefix.size()) == prefix &&
                                n.size() > prefix.size() && n[prefix.size()] == '-')) {
                members.push_back(i);
            }
        }
        if (members.empty()) continue;
        for (const size_t i : members) used[i] = true;
        if (members.size() == 1) {
            out.push_back({"", {{sorted[members.front()], sorted[members.front()]}}});
            continue;
        }
        std::vector<FamilyItem> items;
        for (const size_t i : members) {
            const std::string& n = sorted[i];
            items.push_back({n, n == prefix ? n : n.substr(prefix.size() + 1)});
        }
        out.push_back({std::string(label), std::move(items)});
    }

    // Unknown stems, in first-appearance order over the sorted names.
    std::vector<std::string> stems;
    std::vector<std::vector<size_t>> by_stem;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (used[i]) continue;
        const std::string stem{model_stem(sorted[i])};
        size_t slot = std::string_view::npos;
        for (size_t s = 0; s < stems.size(); ++s) {
            if (stems[s] == stem) { slot = s; break; }
        }
        if (slot == std::string_view::npos) {
            stems.push_back(stem);
            by_stem.push_back({});
            slot = stems.size() - 1;
        }
        by_stem[slot].push_back(i);
    }
    for (size_t s = 0; s < stems.size(); ++s) {
        const auto& stem = stems[s];
        if (by_stem[s].size() == 1) {
            const size_t i = by_stem[s].front();
            out.push_back({"", {{sorted[i], sorted[i]}}});
            continue;
        }
        std::vector<FamilyItem> items;
        for (const size_t i : by_stem[s]) {
            const std::string& n = sorted[i];
            std::string short_name = n == stem ? n : n.substr(stem.size());
            if (!short_name.empty() && short_name.front() == '-')
                short_name.erase(short_name.begin());
            items.push_back({n, std::move(short_name)});
        }
        out.push_back({stem, std::move(items)});
    }
    return out;
}

}  // namespace upix
