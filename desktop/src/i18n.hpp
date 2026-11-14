#pragma once

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace upix::i18n {

enum class Locale {
    En = 0,
    ZhCn = 1,
};

enum class LangPref {
    Auto = 0,
    ZhCn = 1,
    En = 2,
};

enum class Key {
    // Top bar / window
    TabWorkbench = 0,
    TabModels,
    TabHost,
    TabSettings,
    TabTipWorkbench,
    TabTipModels,
    TabTipHost,
    TabTipSettings,
    BackendTipGpu,
    BackendTipCpu,
    ThemeToggleTip,
    WinMinimize,
    WinRestore,
    WinMaximize,
    WinClose,

    // Common UI & scenes
    CommonConfirmQ,
    CommonConfirmClearQ,
    CommonClear,
    CommonReset,
    CommonCurrent,
    CommonRecommended,
    CommonAuto,
    CommonFollow,
    CommonDirectory,
    CommonOpenDir,
    CommonOpenImage,
    CommonRevealFile,
    SceneAll,
    SceneAnime,
    ScenePhoto,
    SceneGeneral,
    SceneRestore,
    ScenePhotoRestore,

    // Settings
    SettingsTitle,
    SettingsSecEngine,
    SettingsComputeDevice,
    SettingsTileSize,
    SettingsDaemonTitle,
    SettingsDaemonStarting,
    SettingsDaemonReady,
    SettingsDaemonReadyPid,
    SettingsDaemonOff,
    SettingsIdleTime,
    SettingsIdle5m,
    SettingsIdle15m,
    SettingsIdle30m,
    SettingsIdleNever,
    SettingsSecEncoding,
    SettingsDefaultFmt,
    SettingsDefaultQuality,
    SettingsChroma,
    SettingsEncodeSpeed,
    SettingsSpeedFine,
    SettingsSpeedBalanced,
    SettingsSpeedFast,
    SettingsRecursive,
    SettingsForce,
    SettingsSecAppearance,
    SettingsLanguage,
    SettingsLangAuto,
    SettingsLangZhCn,
    SettingsLangEn,
    SettingsTheme,
    SettingsThemeDark,
    SettingsThemeLight,
    SettingsReduceMotion,
    SettingsSecAbout,
    SettingsModelsDir,

    // Models
    ModelsTitle,
    ModelsSummaryFmt,
    ModelsImport,
    ModelsMirror,
    ModelsRefresh,
    ModelsSourceLabel,
    ModelsSourceHintHfMirror,
    ModelsSourceHintHf,
    ModelsSourceHintGh,
    ModelsSearchHint,
    ModelsClearFilters,
    ModelsDownloadingBanner,
    ModelsEmptyFiltered,
    ModelsEmptyCatalog,
    ModelsCloseBanner,
    ModelsDelete,
    ModelsDeleteCurrentTip,
    ModelsBusyDownloadingTip,
    ModelsDownload,
    ModelsStandalone,
    ModelsGroupInstalled,
    ModelsGroupAvailable,
    ModelsProbeText,

    // Workbench
    WbCompareOrigDims,
    WbCompareOrig,
    WbDropTitle,
    WbSelectImages,
    WbSelectFolder,
    WbIncludeSubfolders,
    WbZoomFit,
    WbZoomPixel,
    WbPhaseCancelling,
    WbPhaseInferringFmt,
    WbPhasePreparing,
    WbStatsTileEtaFmt,
    WbStatsTileFmt,
    WbErrMissingModelTitle,
    WbErrFailedTitle,
    WbErrUnknownMsg,
    WbErrSelectModel,
    WbErrRetry,
    DockAddImage,
    DockQueued,
    DockDone,
    DockFailed,
    DockRemoveImage,
    PickerManageModels,
    PickerClearSearch,
    PickerCloseEsc,
    InspectorSecModel,
    InspectorSecOutput,
    InspectorSecExport,
    InspectorQuality,
    InspectorChangeDir,
    InspectorChooseDir,
    InspectorFollowOrigDir,
    InspectorOverwriteInPlace,
    InspectorOverwriteOrig,
    InspectorBackupOrig,
    InspectorCtaCancel,
    InspectorCtaAddImageFirst,
    InspectorCtaSelectModelFirst,
    InspectorCtaRerun,
    InspectorCtaResume,
    InspectorCtaStart,

    // Host / Remote
    HostTitle,
    HostFingerprint,
    HostAddress,
    HostPort,
    HostStopped,
    HostStart,
    HostStop,
    HostCopied,
    HostCopyFp,
    HostPortLockedTip,
    HostTrustedTitle,
    HostNoTrusted,
    HostTrustedBadge,
    HostRemoveDevice,
    HostLogTitle,
    HostLogWaiting,
    HostPairHeading,
    HostPairBody,
    HostPairDeny,
    HostPairAllow,
    HostTrustedFallbackName,

    // Notices / Status / Errors / Daemon / Host logs / Fixtures
    NoticeClipboardEmpty,
    NoticeJobRunning,
    NoticeNoImagesInDir,
    NoticeIgnoredFilesFmt,
    StatusImageCountFmt,
    StatusPending,
    StatusInferring,
    StatusDoneFmt,
    StatusBatchDone,
    StatusSkipped,
    StatusFailed,
    StatusError,
    StatusCancelled,
    StatusDone,
    StatusMissingModel,
    JobErrMissingModelFmt,
    JobErrMissingModelDetail,
    JobErrReadDir,
    JobErrInferFailed,
    JobErrInferDetail,
    ModelsImporting,
    ModelsImportDone,
    ModelsImportFailed,
    ModelsDownloadingFmt,
    ModelsNotFoundRemoteFmt,
    ModelsDownloadPctFmt,
    ModelsInstalledFmt,
    ModelsDownloadFailedFmt,
    ModelsRemovedFmt,
    DaemonReady,
    DaemonStopped,
    DaemonStopping,
    DaemonStarting,
    DaemonStartFailedFmt,
    HostLogStartingFmt,
    HostStartFailedFmt,
    HostLogListeningFmt,
    HostLogPairReqFmt,
    HostErrRuntimeFmt,
    HostErrStartException,
    HostLogStopped,
    HostLogPairAllowedFmt,
    HostLogPairDeniedFmt,
    FixtureErrOutOfVram,
    FixtureErrVramAlloc,
    FixtureMissingModelHint,
    FixtureHostLogCodecSummary,
    FixtureHostLogCodecDetail,
    FixtureHostLogTaskSummary,
    FixtureHostLogTaskDetail,

    KEY_COUNT
};

constexpr std::size_t key_count() noexcept {
    return static_cast<std::size_t>(Key::KEY_COUNT);
}

// Parse and normalize language preference strings ("auto" | "zh-CN" | "en").
LangPref parse_lang_pref(std::string_view value) noexcept;
const char* lang_pref_id(LangPref pref) noexcept;

// Normalize BCP-47 / POSIX locale tags into ZhCn or En.
// Supports colon-separated priority lists (e.g., LANGUAGE="fr:zh_CN:en").
Locale normalize_bcp47(std::string_view tag) noexcept;

// Detect locale from POSIX env vars using an injectable lookup function (for testing).
using EnvLookupFn = const char* (*)(const char* name, void* user);
Locale detect_os_locale_from_env(EnvLookupFn lookup, void* user = nullptr) noexcept;

// Detect the host desktop OS preferred locale (Windows / macOS / Linux POSIX).
Locale detect_os_locale() noexcept;

// Resolve effective locale for a preference.
Locale resolve_locale(LangPref pref) noexcept;

// Active runtime preference & locale (thread-safe).
void set_preference(LangPref pref) noexcept;
void set_preference(std::string_view pref_id) noexcept;
LangPref preference() noexcept;
void set_locale(Locale loc) noexcept;
Locale current_locale() noexcept;

// Lookup static UTF-8 translation pointer for current or specified locale.
const char* tr(Key key) noexcept;
const char* tr_for(Locale loc, Key key) noexcept;

// Format a localized pattern with std::vformat.
template <typename... Args>
inline std::string fmt(Key key, Args&&... args) {
    return std::vformat(tr(key), std::make_format_args(args...));
}

template <typename... Args>
inline std::string fmt_for(Locale loc, Key key, Args&&... args) {
    return std::vformat(tr_for(loc, key), std::make_format_args(args...));
}

} // namespace upix::i18n
