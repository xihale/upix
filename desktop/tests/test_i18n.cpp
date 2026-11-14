#include "i18n.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__,  \
                         #expr);                                                    \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {

using namespace upix::i18n;

struct MockEnv {
    std::unordered_map<std::string, std::string> vars;
};

const char* lookupMockEnv(const char* name, void* user) {
    const auto* env = static_cast<const MockEnv*>(user);
    if (!env || !name) return nullptr;
    auto it = env->vars.find(name);
    if (it == env->vars.end()) return nullptr;
    return it->second.c_str();
}

bool isValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0x80u) == 0) {
            ++i;
        } else if ((c & 0xE0u) == 0xC0u) {
            if (i + 1 >= s.size()) return false;
            if ((static_cast<unsigned char>(s[i + 1]) & 0xC0u) != 0x80u) return false;
            i += 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            if (i + 2 >= s.size()) return false;
            if ((static_cast<unsigned char>(s[i + 1]) & 0xC0u) != 0x80u ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0u) != 0x80u) return false;
            i += 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            if (i + 3 >= s.size()) return false;
            if ((static_cast<unsigned char>(s[i + 1]) & 0xC0u) != 0x80u ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0u) != 0x80u ||
                (static_cast<unsigned char>(s[i + 3]) & 0xC0u) != 0x80u) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::vector<std::string> extractFormatPlaceholders(std::string_view s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            if (i + 1 < s.size() && s[i + 1] == '{') {
                ++i;
                continue;
            }
            const size_t close = s.find('}', i + 1);
            CHECK(close != std::string_view::npos);
            out.emplace_back(s.substr(i, close - i + 1));
            i = close;
        }
    }
    return out;
}

} // namespace

int main() {
    // 1. Language preference parsing and round-trip IDs
    CHECK(parse_lang_pref("auto") == LangPref::Auto);
    CHECK(parse_lang_pref("AUTO") == LangPref::Auto);
    CHECK(parse_lang_pref("") == LangPref::Auto);
    CHECK(parse_lang_pref("invalid") == LangPref::Auto);
    CHECK(parse_lang_pref("zh-CN") == LangPref::ZhCn);
    CHECK(parse_lang_pref("zh_CN") == LangPref::ZhCn);
    CHECK(parse_lang_pref("zh-Hans") == LangPref::ZhCn);
    CHECK(parse_lang_pref("zh-hans-cn") == LangPref::ZhCn);
    CHECK(parse_lang_pref("zh") == LangPref::ZhCn);
    CHECK(parse_lang_pref("en") == LangPref::En);
    CHECK(parse_lang_pref("en-US") == LangPref::En);
    CHECK(parse_lang_pref("en_GB") == LangPref::En);
    CHECK(std::strcmp(lang_pref_id(LangPref::Auto), "auto") == 0);
    CHECK(std::strcmp(lang_pref_id(LangPref::ZhCn), "zh-CN") == 0);
    CHECK(std::strcmp(lang_pref_id(LangPref::En), "en") == 0);

    // 2. BCP-47 / POSIX locale normalization
    CHECK(normalize_bcp47("zh-Hans") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh-Hans-CN") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh-Hans-SG") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh-Hans-TW") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh-CN") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh_CN.UTF-8") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh_CN.utf8@hans") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh-SG") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh_SG.UTF-8") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh") == Locale::ZhCn);
    CHECK(normalize_bcp47("zh.UTF-8") == Locale::ZhCn);

    // Traditional Chinese variants map to English fallback (not Simplified)
    CHECK(normalize_bcp47("zh-Hant") == Locale::En);
    CHECK(normalize_bcp47("zh-Hant-TW") == Locale::En);
    CHECK(normalize_bcp47("zh-TW") == Locale::En);
    CHECK(normalize_bcp47("zh_TW.UTF-8") == Locale::En);
    CHECK(normalize_bcp47("zh-HK") == Locale::En);
    CHECK(normalize_bcp47("zh-MO") == Locale::En);

    // Non-Chinese and POSIX neutral tags
    CHECK(normalize_bcp47("en") == Locale::En);
    CHECK(normalize_bcp47("en_US.UTF-8") == Locale::En);
    CHECK(normalize_bcp47("en-GB") == Locale::En);
    CHECK(normalize_bcp47("ja_JP.UTF-8") == Locale::En);
    CHECK(normalize_bcp47("fr-FR") == Locale::En);
    CHECK(normalize_bcp47("C") == Locale::En);
    CHECK(normalize_bcp47("POSIX") == Locale::En);
    CHECK(normalize_bcp47("") == Locale::En);

    // Colon-separated priority lists (GNU LANGUAGE)
    CHECK(normalize_bcp47("zh_CN:en_US:en") == Locale::ZhCn);
    CHECK(normalize_bcp47("fr:zh_CN:en") == Locale::ZhCn);
    CHECK(normalize_bcp47("en_US:zh_CN") == Locale::En);
    CHECK(normalize_bcp47("zh_TW:zh_CN") == Locale::En);
    CHECK(normalize_bcp47("C:zh_CN") == Locale::ZhCn);

    // 3. POSIX env precedence (LC_ALL -> LC_MESSAGES -> LANG -> LANGUAGE)
    {
        MockEnv env;
        env.vars["LANGUAGE"] = "zh_CN:en";
        env.vars["LANG"] = "en_US.UTF-8";
        // Explicit LANG=en_US takes precedence over LANGUAGE? Wait: let's check both C.UTF-8 and explicit!
        CHECK(detect_os_locale_from_env(lookupMockEnv, &env) == Locale::En);

        env.vars["LANG"] = "C.UTF-8";
        CHECK(detect_os_locale_from_env(lookupMockEnv, &env) == Locale::ZhCn);

        env.vars["LC_MESSAGES"] = "en_GB.UTF-8";
        CHECK(detect_os_locale_from_env(lookupMockEnv, &env) == Locale::En);

        env.vars["LC_MESSAGES"] = "zh_CN.UTF-8";
        CHECK(detect_os_locale_from_env(lookupMockEnv, &env) == Locale::ZhCn);

        // LC_ALL=C overrides everything below it
        env.vars["LC_ALL"] = "C";
        CHECK(detect_os_locale_from_env(lookupMockEnv, &env) == Locale::En);

        env.vars["LC_ALL"] = "zh_Hans_CN.UTF-8";
        CHECK(detect_os_locale_from_env(lookupMockEnv, &env) == Locale::ZhCn);
    }

    // 4. Table completeness, UTF-8 validity & placeholder parity across all keys
    for (std::size_t idx = 0; idx < key_count(); ++idx) {
        const auto k = static_cast<Key>(idx);
        const char* en = tr_for(Locale::En, k);
        const char* zh = tr_for(Locale::ZhCn, k);
        CHECK(en != nullptr && en[0] != '\0');
        CHECK(zh != nullptr && zh[0] != '\0');
        CHECK(isValidUtf8(en));
        CHECK(isValidUtf8(zh));
        const auto enPh = extractFormatPlaceholders(en);
        const auto zhPh = extractFormatPlaceholders(zh);
        CHECK(enPh == zhPh);
    }

    // 5. Runtime preference switching & formatting
    set_preference("en");
    CHECK(preference() == LangPref::En);
    CHECK(current_locale() == Locale::En);
    CHECK(std::string_view(tr(Key::TabSettings)) == "Settings");
    CHECK(fmt(Key::ModelsSummaryFmt, 3, 12, "HF-Mirror") == "Cached 3 / 12 · HF-Mirror");

    set_preference("zh-CN");
    CHECK(preference() == LangPref::ZhCn);
    CHECK(current_locale() == Locale::ZhCn);
    CHECK(std::string_view(tr(Key::TabSettings)) == "设置");
    CHECK(fmt(Key::ModelsSummaryFmt, 3, 12, "HF-Mirror") == "已缓存 3 / 12 · HF-Mirror");

    set_preference("auto");
    CHECK(preference() == LangPref::Auto);
    CHECK(current_locale() == detect_os_locale());

    std::printf("test_i18n: PASS (%zu keys verified)\n", key_count());
    return 0;
}
