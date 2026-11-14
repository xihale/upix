// views/models/grouping.hpp — Model filtering, search matching, size formatting, and family grouping.
#pragma once

#include "app_state.hpp"
#include "i18n.hpp"
#include "ui/controls.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace upix::views::models_detail {

inline std::string toLowerTrimmed(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x80) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return s;
}

inline std::string toLowerAscii(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x80) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

inline bool filterMatches(const std::string& filter, const upix::gui::ModelItem& m) {
    if (filter == "all" || filter.empty()) return true;
    if (filter == "rec") return m.recommended;
    const std::string& scene = m.scene;
    if (filter == "动漫") return scene.find("动漫") != std::string::npos;
    if (filter == "照片") {
        return scene.find("照片") != std::string::npos && scene.find("修复") == std::string::npos;
    }
    if (filter == "综合") {
        return scene.find("综合") != std::string::npos || scene.find("通用") != std::string::npos || scene.empty();
    }
    if (filter == "修复") return scene.find("修复") != std::string::npos;
    return scene.find(filter) != std::string::npos;
}

inline bool queryMatches(const std::string& qLower, const upix::gui::ModelItem& m, const std::string& family) {
    if (qLower.empty()) return true;
    char scBuf[32] = {};
    const char* scLbl = upix::ui::ctl::sceneLabel(m.scene.c_str(), scBuf, sizeof(scBuf));
    if (toLowerAscii(m.name).find(qLower) != std::string::npos) return true;
    if (!m.short_name.empty() && toLowerAscii(m.short_name).find(qLower) != std::string::npos) return true;
    if (!family.empty() && toLowerAscii(family).find(qLower) != std::string::npos) return true;
    if (!m.scene.empty() && toLowerAscii(m.scene).find(qLower) != std::string::npos) return true;
    if (scLbl && scLbl[0] && toLowerAscii(scLbl).find(qLower) != std::string::npos) return true;
    return false;
}

inline bool isMissingSize(const std::string& s) {
    return s.empty() || s == "0.00 MB" || s == "0 MB" || s == "0 B";
}

inline double parseSizeMB(const std::string& s) {
    if (s.empty()) return 0.0;
    char* end = nullptr;
    const double val = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || val <= 0.0) return 0.0;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    std::string unit(end);
    for (char& c : unit) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (unit.rfind("GB", 0) == 0) return val * 1024.0;
    if (unit.rfind("KB", 0) == 0) return val / 1024.0;
    if (unit.rfind("B", 0) == 0) return val / 1048576.0;
    return val;
}

inline std::string formatTotalMB(double mb) {
    if (mb <= 0.005) return "0 MB";
    if (mb >= 1024.0) return std::format("{:.2f} GB", mb / 1024.0);
    if (mb >= 100.0) return std::format("{:.0f} MB", mb);
    return std::format("{:.1f} MB", mb);
}

inline std::string_view stripFamilyPrefixForDisplay(const std::string& familyTitle,
                                                    const upix::gui::ModelItem& m) {
    using upix::i18n::Key;
    using upix::i18n::tr;
    if (familyTitle.empty() ||
        familyTitle == "独立模型" || familyTitle == tr(Key::ModelsStandalone) ||
        familyTitle == "已安装模型" || familyTitle == tr(Key::ModelsGroupInstalled) ||
        familyTitle == "可下载模型" || familyTitle == tr(Key::ModelsGroupAvailable)) {
        return m.name;
    }
    if (!m.short_name.empty() && m.short_name != m.name) {
        return m.short_name;
    }

    auto normKey = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (c == '-' || c == '_' || c == ' ') continue;
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        return out;
    };

    const std::string target = normKey(familyTitle);
    if (target.empty()) return m.name;

    std::string acc;
    for (size_t i = 0; i < m.name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(m.name[i]);
        if (c != '-' && c != '_' && c != ' ') {
            acc.push_back(static_cast<char>(std::tolower(c)));
            if (acc.size() == target.size() && acc == target) {
                if (i + 1 < m.name.size() && (m.name[i + 1] == '-' || m.name[i + 1] == '_')) {
                    const std::string_view rest = std::string_view(m.name).substr(i + 2);
                    if (!rest.empty()) return rest;
                }
                break;
            }
            if (acc.size() >= target.size()) break;
        }
    }
    return m.name;
}

struct MergedModelGroup {
    std::string title;
    std::vector<upix::gui::ModelItem> items;
};

inline std::vector<MergedModelGroup> buildUnifiedFilteredGroups(const upix::gui::Snapshot& snap,
                                                                const std::string& filter,
                                                                const std::string& qLower) {
    std::unordered_map<std::string, upix::gui::ModelItem> byName;
    std::vector<std::string> candidateNames;

    auto ingest = [&](const upix::gui::ModelItem& m, bool isRemote) {
        if (byName.count(m.name)) return;
        upix::gui::ModelItem copy = m;
        copy.remote = isRemote;
        byName.emplace(copy.name, std::move(copy));
    };

    for (const auto& g : snap.installed_groups) {
        for (const auto& m : g.items) ingest(m, false);
    }
    for (const auto& m : snap.installed) ingest(m, false);
    for (const auto& g : snap.remote_groups) {
        for (const auto& m : g.items) ingest(m, true);
    }
    for (const auto& m : snap.remote) ingest(m, true);

    // Map each model to its family label once so family-name search also works.
    std::vector<std::string> allNames;
    allNames.reserve(byName.size());
    for (const auto& [name, _] : byName) allNames.push_back(name);
    std::unordered_map<std::string, std::string> familyOf;
    for (const auto& fg : upix::groups(allNames)) {
        const std::string fam = fg.label.empty()
            ? std::string(upix::i18n::tr(upix::i18n::Key::ModelsStandalone))
            : fg.label;
        for (const auto& it : fg.items) familyOf[it.name] = fam;
    }

    for (const auto& [name, item] : byName) {
        if (!filterMatches(filter, item)) continue;
        if (!queryMatches(qLower, item, familyOf[name])) continue;
        candidateNames.push_back(name);
    }

    std::vector<MergedModelGroup> result;
    std::vector<upix::gui::ModelItem> orphans;
    for (const auto& fg : upix::groups(candidateNames)) {
        std::vector<upix::gui::ModelItem> items;
        items.reserve(fg.items.size());
        for (const auto& fit : fg.items) {
            auto it = byName.find(fit.name);
            if (it == byName.end()) continue;
            upix::gui::ModelItem mi = it->second;
            mi.short_name = fit.short_name;
            items.push_back(std::move(mi));
        }
        if (items.empty()) continue;
        if (fg.label.empty()) {
            orphans.insert(orphans.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
        } else {
            result.push_back(MergedModelGroup{fg.label, std::move(items)});
        }
    }
    if (!orphans.empty()) {
        result.push_back(MergedModelGroup{upix::i18n::tr(upix::i18n::Key::ModelsStandalone), std::move(orphans)});
    }
    return result;
}

} // namespace upix::views::models_detail
