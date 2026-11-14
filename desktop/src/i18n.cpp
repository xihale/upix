#include "i18n.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cwchar>
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace upix::i18n {
namespace {

struct Entry {
    Key key;
    const char* en;
    const char* zh_cn;
};

constexpr std::array<Entry, key_count()> kEntries = {{
    // Top bar / window
    {Key::TabWorkbench, "Workbench", "工作台"},
    {Key::TabModels, "Models", "模型"},
    {Key::TabHost, "Remote", "远程"},
    {Key::TabSettings, "Settings", "设置"},
    {Key::TabTipWorkbench, "Workbench (Ctrl+1)", "工作台 (Ctrl+1)"},
    {Key::TabTipModels, "Models (Ctrl+2)", "模型 (Ctrl+2)"},
    {Key::TabTipHost, "Remote (Ctrl+3)", "远程 (Ctrl+3)"},
    {Key::TabTipSettings, "Settings (Ctrl+4)", "设置 (Ctrl+4)"},
    {Key::BackendTipGpu, "Backend: Vulkan (open Settings)", "计算后端：Vulkan（点击前往设置）"},
    {Key::BackendTipCpu, "Backend: CPU (open Settings)", "计算后端：CPU（点击前往设置）"},
    {Key::ThemeToggleTip, "Toggle theme: Dark / Light", "切换外观：深色 / 浅色"},
    {Key::WinMinimize, "Minimize", "最小化"},
    {Key::WinRestore, "Restore", "还原"},
    {Key::WinMaximize, "Maximize", "最大化"},
    {Key::WinClose, "Close", "关闭"},

    // Common UI & scenes
    {Key::CommonConfirmQ, "Confirm?", "确认?"},
    {Key::CommonConfirmClearQ, "Clear all?", "确认清空？"},
    {Key::CommonClear, "Clear", "清空"},
    {Key::CommonReset, "Reset", "重置"},
    {Key::CommonCurrent, "Current", "当前"},
    {Key::CommonRecommended, "Recommended", "推荐"},
    {Key::CommonAuto, "Auto", "自动"},
    {Key::CommonFollow, "Auto", "跟随"},
    {Key::CommonDirectory, "Folder", "目录"},
    {Key::CommonOpenDir, "Open Folder", "打开目录"},
    {Key::CommonOpenImage, "Open Image", "打开图像"},
    {Key::CommonRevealFile, "Reveal File", "定位文件"},
    {Key::SceneAll, "All", "全部"},
    {Key::SceneAnime, "Anime", "动漫"},
    {Key::ScenePhoto, "Photo", "照片"},
    {Key::SceneGeneral, "General", "综合"},
    {Key::SceneRestore, "Restore", "修复"},
    {Key::ScenePhotoRestore, "Photo Restore", "照片修复"},

    // Settings
    {Key::SettingsTitle, "Settings", "设置"},
    {Key::SettingsSecEngine, "Inference Engine", "推理引擎"},
    {Key::SettingsComputeDevice, "Compute Device", "计算设备"},
    {Key::SettingsTileSize, "Tile Size", "分块尺寸"},
    {Key::SettingsDaemonTitle, "Resident Engine", "常驻热引擎"},
    {Key::SettingsDaemonStarting, "Starting…", "正在启动…"},
    {Key::SettingsDaemonReady, "Ready", "已就绪"},
    {Key::SettingsDaemonReadyPid, "Ready · PID {}", "已就绪 · PID {}"},
    {Key::SettingsDaemonOff, "Off", "未开启"},
    {Key::SettingsIdleTime, "Idle Timeout", "常驻时间"},
    {Key::SettingsIdle5m, "5 min", "5分钟"},
    {Key::SettingsIdle15m, "15 min", "15分钟"},
    {Key::SettingsIdle30m, "30 min", "30分钟"},
    {Key::SettingsIdleNever, "Never", "不退出"},
    {Key::SettingsSecEncoding, "Default Encoding & Batch", "默认编码与批处理"},
    {Key::SettingsDefaultFmt, "Default Format", "默认输出格式"},
    {Key::SettingsDefaultQuality, "Default Quality", "默认质量"},
    {Key::SettingsChroma, "Chroma Subsampling", "色度抽样"},
    {Key::SettingsEncodeSpeed, "Encoding Speed", "编码速度"},
    {Key::SettingsSpeedFine, "Best", "精细"},
    {Key::SettingsSpeedBalanced, "Balanced", "均衡"},
    {Key::SettingsSpeedFast, "Fast", "快速"},
    {Key::SettingsRecursive, "Include Subfolders", "文件夹包含子目录"},
    {Key::SettingsForce, "Force Re-run (Ignore Marker)", "忽略超分标记强制重跑"},
    {Key::SettingsSecAppearance, "Appearance & Behavior", "交互与外观"},
    {Key::SettingsLanguage, "Language", "界面语言"},
    {Key::SettingsLangAuto, "Follow System", "跟随系统"},
    {Key::SettingsLangZhCn, "简体中文", "简体中文"},
    {Key::SettingsLangEn, "English", "English"},
    {Key::SettingsTheme, "Theme", "外观主题"},
    {Key::SettingsThemeDark, "Dark", "深色"},
    {Key::SettingsThemeLight, "Light", "浅色"},
    {Key::SettingsReduceMotion, "Reduce Motion", "减少动态效果"},
    {Key::SettingsSecAbout, "Storage & About", "目录与关于"},
    {Key::SettingsModelsDir, "Models Directory", "模型存储目录"},

    // Models
    {Key::ModelsTitle, "Models", "模型库"},
    {Key::ModelsSummaryFmt, "Cached {} / {} · {}", "已缓存 {} / {} · {}"},
    {Key::ModelsImport, "Import", "导入模型"},
    {Key::ModelsMirror, "Mirror", "镜像源"},
    {Key::ModelsRefresh, "Refresh", "刷新列表"},
    {Key::ModelsSourceLabel, "Source", "下载源"},
    {Key::ModelsSourceHintHfMirror, "Prefer HF-Mirror, fallback to Hugging Face · GitHub", "优先 HF-Mirror，失败自动回退 Hugging Face · GitHub"},
    {Key::ModelsSourceHintHf, "Prefer Hugging Face, fallback to GitHub", "优先 Hugging Face，失败自动回退 GitHub"},
    {Key::ModelsSourceHintGh, "Prefer GitHub, fallback to Hugging Face", "优先 GitHub，失败自动回退 Hugging Face"},
    {Key::ModelsSearchHint, "Search models...", "搜索模型..."},
    {Key::ModelsClearFilters, "Clear Filters", "清除筛选"},
    {Key::ModelsDownloadingBanner, "Downloading {}", "正在下载 {}"},
    {Key::ModelsEmptyFiltered, "No matching models", "无匹配模型"},
    {Key::ModelsEmptyCatalog, "No models available", "暂无可用模型"},
    {Key::ModelsCloseBanner, "Dismiss", "关闭提示"},
    {Key::ModelsDelete, "Delete Model", "删除模型"},
    {Key::ModelsDeleteCurrentTip, "Cannot remove the active model", "当前使用中的模型不可直接移除"},
    {Key::ModelsBusyDownloadingTip, "Another download is in progress", "正在下载其他模型"},
    {Key::ModelsDownload, "Download", "下载模型"},
    {Key::ModelsStandalone, "Standalone", "独立模型"},
    {Key::ModelsGroupInstalled, "Installed", "已安装模型"},
    {Key::ModelsGroupAvailable, "Available", "可下载模型"},
    {Key::ModelsProbeText, "ModelAg", "模型Ag"},

    // Workbench
    {Key::WbCompareOrigDims, "Original {}×{}", "原图 {}×{}"},
    {Key::WbCompareOrig, "Original", "原图"},
    {Key::WbDropTitle, "Drop or paste images", "拖放或粘贴图像"},
    {Key::WbSelectImages, "Select Images", "选择图像"},
    {Key::WbSelectFolder, "Select Folder", "选择文件夹"},
    {Key::WbIncludeSubfolders, "Include Subfolders", "包含子文件夹"},
    {Key::WbZoomFit, "Fit", "适应"},
    {Key::WbZoomPixel, "Pixel", "像素"},
    {Key::WbPhaseCancelling, "Cancelling…", "正在取消…"},
    {Key::WbPhaseInferringFmt, "Inferring {}/{}", "推理 {}/{}"},
    {Key::WbPhasePreparing, "Preparing", "准备中"},
    {Key::WbStatsTileEtaFmt, "{} · Tile {}/{} · ~{}s · {}", "{} · 分块 {}/{} · ~{}s · {}"},
    {Key::WbStatsTileFmt, "{} · Tile {}/{} · {}", "{} · 分块 {}/{} · {}"},
    {Key::WbErrMissingModelTitle, "Missing Model", "缺少模型"},
    {Key::WbErrFailedTitle, "Processing Failed", "处理失败"},
    {Key::WbErrUnknownMsg, "Unknown inference error", "推理发生未知错误"},
    {Key::WbErrSelectModel, "Select Model", "选择模型"},
    {Key::WbErrRetry, "Retry", "重试"},
    {Key::DockAddImage, "Add Images", "添加图像"},
    {Key::DockQueued, "Queued", "排队"},
    {Key::DockDone, "Done", "完成"},
    {Key::DockFailed, "Failed", "失败"},
    {Key::DockRemoveImage, "Remove Image", "移除图像"},
    {Key::PickerManageModels, "Manage Models", "管理模型"},
    {Key::PickerClearSearch, "Clear Search", "清除搜索"},
    {Key::PickerCloseEsc, "Close (Esc)", "关闭 (Esc)"},
    {Key::InspectorSecModel, "Model", "模型"},
    {Key::InspectorSecOutput, "Output", "输出"},
    {Key::InspectorSecExport, "Export", "导出设置"},
    {Key::InspectorQuality, "Quality", "质量"},
    {Key::InspectorChangeDir, "Change", "更改"},
    {Key::InspectorChooseDir, "Choose…", "指定…"},
    {Key::InspectorFollowOrigDir, "Same as source", "跟随原图目录"},
    {Key::InspectorOverwriteInPlace, "Overwrite in place", "原位覆盖"},
    {Key::InspectorOverwriteOrig, "Overwrite original", "覆盖原图"},
    {Key::InspectorBackupOrig, "Backup original (upix_orig/)", "备份原图 (upix_orig/)"},
    {Key::InspectorCtaCancel, "Cancel", "取消超分"},
    {Key::InspectorCtaAddImageFirst, "Add images first", "请先添加图像"},
    {Key::InspectorCtaSelectModelFirst, "Select a model first", "请先选择模型"},
    {Key::InspectorCtaRerun, "Re-run", "重新超分"},
    {Key::InspectorCtaResume, "Resume", "继续超分"},
    {Key::InspectorCtaStart, "Start Upscale", "开始超分"},

    // Host / Remote
    {Key::HostTitle, "Remote Collaboration", "远程协同"},
    {Key::HostFingerprint, "Fingerprint", "指纹"},
    {Key::HostAddress, "Address", "地址"},
    {Key::HostPort, "Port", "端口"},
    {Key::HostStopped, "Service stopped", "服务未启动"},
    {Key::HostStart, "Start Service", "启动服务"},
    {Key::HostStop, "Stop Service", "停止服务"},
    {Key::HostCopied, "Copied", "已复制"},
    {Key::HostCopyFp, "Copy Fingerprint", "复制指纹"},
    {Key::HostPortLockedTip, "Port is locked while running; stop service to edit", "服务运行中端口已锁定，停止服务后可修改"},
    {Key::HostTrustedTitle, "Trusted Devices", "已信任设备"},
    {Key::HostNoTrusted, "No trusted devices", "暂无已信任设备"},
    {Key::HostTrustedBadge, "Trusted", "已信任"},
    {Key::HostRemoveDevice, "Remove Device", "移除设备"},
    {Key::HostLogTitle, "Activity Log", "运行日志"},
    {Key::HostLogWaiting, "Waiting for pairing…", "等待配对…"},
    {Key::HostPairHeading, "New Device Pairing Request", "新设备请求连接"},
    {Key::HostPairBody, "Verify device fingerprint", "核对设备指纹"},
    {Key::HostPairDeny, "Deny", "拒绝"},
    {Key::HostPairAllow, "Allow", "允许配对"},
    {Key::HostTrustedFallbackName, "Authorized Device #{}", "已授权设备 #{}"},

    // Notices / Status / Errors / Daemon / Host logs / Fixtures
    {Key::NoticeClipboardEmpty, "Clipboard has no image or valid file path", "剪贴板里没有图片或有效文件路径"},
    {Key::NoticeJobRunning, "A job is already running; wait or cancel first", "任务进行中，请先等待完成或取消"},
    {Key::NoticeNoImagesInDir, "No supported images found in folder", "文件夹里没有受支持的图片"},
    {Key::NoticeIgnoredFilesFmt, "Ignored {} unsupported file(s)", "已忽略 {} 个不支持的文件"},
    {Key::StatusImageCountFmt, "{} image(s)", "{} 张图片"},
    {Key::StatusPending, "Pending", "待处理"},
    {Key::StatusInferring, "Inferring", "推理中"},
    {Key::StatusDoneFmt, "Done · {:.1f}s", "完成 · {:.1f}s"},
    {Key::StatusBatchDone, "Batch done", "批处理完成"},
    {Key::StatusSkipped, "Skipped", "已跳过"},
    {Key::StatusFailed, "Failed", "失败"},
    {Key::StatusError, "Error", "异常"},
    {Key::StatusCancelled, "Cancelled", "已取消"},
    {Key::StatusDone, "Done", "完成"},
    {Key::StatusMissingModel, "Missing model", "缺少模型"},
    {Key::JobErrMissingModelFmt, "Model not found: {}", "未找到模型 {}"},
    {Key::JobErrMissingModelDetail, "Download this model or switch to an installed one", "请先下载该模型或切换其他已安装模型"},
    {Key::JobErrReadDir, "Failed to read directory", "目录读取失败"},
    {Key::JobErrInferFailed, "Inference failed", "推理执行失败"},
    {Key::JobErrInferDetail, "Check model files or compute device status", "请检查模型文件或计算设备状态"},
    {Key::ModelsImporting, "Importing models…", "正在导入模型…"},
    {Key::ModelsImportDone, "Models imported", "模型导入完成"},
    {Key::ModelsImportFailed, "Import failed", "导入失败"},
    {Key::ModelsDownloadingFmt, "Downloading {}…", "正在下载 {}…"},
    {Key::ModelsNotFoundRemoteFmt, "Not found in remote manifest: {}", "未在远端清单中找到: {}"},
    {Key::ModelsDownloadPctFmt, "Downloading {} · {}%", "正在下载 {} · {}%"},
    {Key::ModelsInstalledFmt, "Installed {}", "已安装 {}"},
    {Key::ModelsDownloadFailedFmt, "Download failed: {}", "下载失败: {}"},
    {Key::ModelsRemovedFmt, "Removed model: {}", "已移除模型: {}"},
    {Key::DaemonReady, "Engine ready", "热引擎已就绪"},
    {Key::DaemonStopped, "Engine stopped", "热引擎未启动"},
    {Key::DaemonStopping, "Stopping engine…", "正在停止热引擎…"},
    {Key::DaemonStarting, "Starting engine…", "正在启动热引擎…"},
    {Key::DaemonStartFailedFmt, "Engine start failed: {}", "热引擎启动失败: {}"},
    {Key::HostLogStartingFmt, "Starting service :{}", "正在启动服务 :{}"},
    {Key::HostStartFailedFmt, "Service start failed: {}", "服务启动失败: {}"},
    {Key::HostLogListeningFmt, "Listening :{} · QR ready", "监听已就绪 :{} · 二维码可用"},
    {Key::HostLogPairReqFmt, "Pairing request · {}", "收到配对请求 · {}"},
    {Key::HostErrRuntimeFmt, "Service error: {}", "服务异常: {}"},
    {Key::HostErrStartException, "Service startup error", "服务启动异常"},
    {Key::HostLogStopped, "Service stopped", "服务已停止"},
    {Key::HostLogPairAllowedFmt, "Pairing allowed · {}", "已允许配对 · {}"},
    {Key::HostLogPairDeniedFmt, "Pairing denied · {}", "已拒绝配对 · {}"},
    {Key::FixtureErrOutOfVram, "Out of VRAM", "显存不足"},
    {Key::FixtureErrVramAlloc, "VRAM allocation failed", "显存分配不足"},
    {Key::FixtureMissingModelHint, "Download or switch to an installed model in Models", "请先在模型库下载或切换已安装模型"},
    {Key::FixtureHostLogCodecSummary, "Negotiated: Vulkan FP16 · Bidirectional AVIF enabled", "协商能力: Vulkan FP16 · AVIF 双向编解码已启用"},
    {Key::FixtureHostLogCodecDetail, "Trust store: 2 devices", "信任库: 2 台设备"},
    {Key::FixtureHostLogTaskSummary, "Pixel 9 Pro co-inference · AnimeSharp-x4 (4×)", "Pixel 9 Pro 协同推理 · AnimeSharp-x4 (4×)"},
    {Key::FixtureHostLogTaskDetail, "12 / 16 tiles", "12 / 16 块"},
}};

constexpr bool verify_table() {
    for (std::size_t i = 0; i < kEntries.size(); ++i) {
        if (static_cast<std::size_t>(kEntries[i].key) != i) return false;
        if (!kEntries[i].en || !kEntries[i].zh_cn) return false;
    }
    return true;
}

static_assert(verify_table(), "i18n kEntries must match Key enum order 1:1");

std::string ascii_lower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string_view trim_ascii(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return sv;
}

std::optional<Locale> match_single_bcp47(std::string_view raw) {
    raw = trim_ascii(raw);
    if (raw.empty()) return std::nullopt;

    // Strip POSIX codeset (.UTF-8) and modifier (@euro, @hans, etc.), recording modifier if useful.
    if (const auto dot = raw.find('.'); dot != std::string_view::npos) {
        raw = raw.substr(0, dot);
    }
    std::string modifier;
    if (const auto at = raw.find('@'); at != std::string_view::npos) {
        modifier = ascii_lower(raw.substr(at + 1));
        raw = raw.substr(0, at);
    }
    raw = trim_ascii(raw);
    if (raw.empty()) return std::nullopt;

    std::vector<std::string> subtags;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t next = raw.find_first_of("-_", pos);
        if (next == std::string_view::npos) next = raw.size();
        if (next > pos) {
            subtags.push_back(ascii_lower(raw.substr(pos, next - pos)));
        }
        pos = next + 1;
    }
    if (subtags.empty()) return std::nullopt;

    const std::string& lang = subtags[0];
    if (lang == "c" || lang == "posix") {
        return std::nullopt;
    }
    if (lang == "zh") {
        bool hasHans = (modifier == "hans");
        bool hasHant = (modifier == "hant");
        bool hasTradRegion = false;
        for (size_t i = 1; i < subtags.size(); ++i) {
            const std::string& st = subtags[i];
            if (st == "hans") hasHans = true;
            else if (st == "hant") hasHant = true;
            else if (st == "tw" || st == "hk" || st == "mo") hasTradRegion = true;
        }
        if (hasHant || (hasTradRegion && !hasHans)) {
            return Locale::En;
        }
        return Locale::ZhCn;
    }
    if (lang == "en") {
        return Locale::En;
    }
    return std::nullopt;
}

std::atomic<int>& pref_atomic() {
    static std::atomic<int> v{static_cast<int>(LangPref::Auto)};
    return v;
}

std::atomic<int>& locale_atomic() {
    static std::atomic<int> v{static_cast<int>(detect_os_locale())};
    return v;
}

#if !defined(_WIN32) && !defined(__APPLE__)
const char* std_getenv_adapter(const char* name, void*) {
    return std::getenv(name);
}
#endif

} // namespace

LangPref parse_lang_pref(std::string_view value) noexcept {
    const std::string low = ascii_lower(trim_ascii(value));
    if (low.empty() || low == "auto") {
        return LangPref::Auto;
    }
    if (const auto matched = match_single_bcp47(value)) {
        return (*matched == Locale::ZhCn) ? LangPref::ZhCn : LangPref::En;
    }
    return LangPref::Auto;
}

const char* lang_pref_id(LangPref pref) noexcept {
    switch (pref) {
    case LangPref::ZhCn: return "zh-CN";
    case LangPref::En: return "en";
    case LangPref::Auto:
    default: return "auto";
    }
}

Locale normalize_bcp47(std::string_view tag) noexcept {
    // Support colon-separated priority lists (e.g. LANGUAGE="fr:zh_CN:en").
    size_t pos = 0;
    while (pos < tag.size()) {
        size_t sep = tag.find(':', pos);
        if (sep == std::string_view::npos) sep = tag.size();
        const std::string_view part = tag.substr(pos, sep - pos);
        if (auto matched = match_single_bcp47(part)) {
            return *matched;
        }
        if (sep == tag.size()) break;
        pos = sep + 1;
    }
    return Locale::En;
}

Locale detect_os_locale_from_env(EnvLookupFn lookup, void* user) noexcept {
    if (!lookup) return Locale::En;
    const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    for (const char* var : vars) {
        const char* val = lookup(var, user);
        if (!val || !val[0]) continue;
        // Treat C / POSIX / C.UTF-8 in LC_ALL as explicit non-localized unless overridden only for LANG,
        // so e.g. LANG=C.UTF-8 + LANGUAGE=zh_CN still picks up LANGUAGE.
        if (std::string_view(var) == "LC_ALL") {
            const auto matched = match_single_bcp47(val);
            if (matched) return *matched;
            // If LC_ALL is literally "C" or "POSIX" (without encoding), POSIX overrides LANGUAGE too.
            const std::string low = ascii_lower(trim_ascii(val));
            if (low == "c" || low == "posix") return Locale::En;
            continue;
        }
        size_t pos = 0;
        const std::string_view sv(val);
        while (pos < sv.size()) {
            size_t sep = sv.find(':', pos);
            if (sep == std::string_view::npos) sep = sv.size();
            if (auto matched = match_single_bcp47(sv.substr(pos, sep - pos))) {
                return *matched;
            }
            if (sep == sv.size()) break;
            pos = sep + 1;
        }
    }
    return Locale::En;
}

Locale detect_os_locale() noexcept {
#ifdef _WIN32
    auto wideToUtf8 = [](const wchar_t* wstr) -> std::string {
        if (!wstr || !wstr[0]) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
        return out;
    };

    ULONG numLangs = 0;
    ULONG bufLen = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLangs, nullptr, &bufLen) && bufLen > 0) {
        std::vector<wchar_t> buf(bufLen);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLangs, buf.data(), &bufLen)) {
            const wchar_t* cur = buf.data();
            while (*cur) {
                const std::string tag = wideToUtf8(cur);
                if (auto matched = match_single_bcp47(tag)) {
                    return *matched;
                }
                cur += std::wcslen(cur) + 1;
            }
        }
    }

    wchar_t locName[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(locName, LOCALE_NAME_MAX_LENGTH) > 0) {
        return normalize_bcp47(wideToUtf8(locName));
    }
    return Locale::En;
#elif defined(__APPLE__)
    if (CFArrayRef langs = CFLocaleCopyPreferredLanguages()) {
        const CFIndex count = CFArrayGetCount(langs);
        for (CFIndex i = 0; i < count; ++i) {
            const void* item = CFArrayGetValueAtIndex(langs, i);
            if (item && CFGetTypeID(item) == CFStringGetTypeID()) {
                char buf[128] = {};
                if (CFStringGetCString(static_cast<CFStringRef>(item), buf, sizeof(buf), kCFStringEncodingUTF8)) {
                    if (auto matched = match_single_bcp47(buf)) {
                        CFRelease(langs);
                        return *matched;
                    }
                }
            }
        }
        CFRelease(langs);
    }
    return Locale::En;
#else
    return detect_os_locale_from_env(std_getenv_adapter, nullptr);
#endif
}

Locale resolve_locale(LangPref pref) noexcept {
    switch (pref) {
    case LangPref::ZhCn: return Locale::ZhCn;
    case LangPref::En: return Locale::En;
    case LangPref::Auto:
    default: return detect_os_locale();
    }
}

void set_preference(LangPref pref) noexcept {
    pref_atomic().store(static_cast<int>(pref), std::memory_order_relaxed);
    locale_atomic().store(static_cast<int>(resolve_locale(pref)), std::memory_order_relaxed);
}

void set_preference(std::string_view pref_id) noexcept {
    set_preference(parse_lang_pref(pref_id));
}

LangPref preference() noexcept {
    return static_cast<LangPref>(pref_atomic().load(std::memory_order_relaxed));
}

void set_locale(Locale loc) noexcept {
    locale_atomic().store(static_cast<int>(loc), std::memory_order_relaxed);
}

Locale current_locale() noexcept {
    return static_cast<Locale>(locale_atomic().load(std::memory_order_relaxed));
}

const char* tr(Key key) noexcept {
    return tr_for(current_locale(), key);
}

const char* tr_for(Locale loc, Key key) noexcept {
    const std::size_t idx = static_cast<std::size_t>(key);
    if (idx >= kEntries.size()) return "";
    const auto& e = kEntries[idx];
    return (loc == Locale::ZhCn) ? e.zh_cn : e.en;
}

} // namespace upix::i18n
