package io.upix.data.model

import androidx.compose.runtime.Immutable

/**
 * 目录模型的场景标注（动漫/照片/综合/修复…+ 推荐星标），供模型选择 sheet、
 * 远程下载 sheet 与模型库卡的场景徽标使用。本表是
 * share/core/model_metadata.cpp 的 Kotlin 对照版 —— 规则顺序即匹配优先级
 * （"real-cugan" 先于 1x "-denoise" 修复网、"x4plus-anime" 先于
 * "x4plus"），推荐位取自 models/README.md 点名的首选。目录新增模型时
 * 两处同步，权威描述见 README 的 "Use it for" 列。
 */
@Immutable
data class ModelScene(
    /** 展示标签，如 "动漫"、"照片·修复"。 */
    val label: String,
    /** 该场景下的首选推荐（README 点名）。 */
    val recommended: Boolean
)

/** 与 ModelManager::find_model 相同的归一化：小写 + 下划线/空格归并为连字符。 */
private fun normalize(name: String): String =
    name.lowercase().replace('_', '-').replace(' ', '-')

private val SCENE_RULES: List<Pair<String, ModelScene>> = listOf(
    // Real-ESRGAN v3 compact —— 双端默认推荐。
    "real-esrganv3-anime" to ModelScene("动漫", true),
    "real-esrganv3-general" to ModelScene("照片", true),
    // x4plus 家族；anime 变体必须先于照片基础款。
    "real-esrgan-x4plus-anime" to ModelScene("动漫", false),
    "real-esrgan-x4plus" to ModelScene("照片", true),
    "real-esrgan-x2plus" to ModelScene("照片", false),
    // Real-CUGAN —— 动漫专精（先于 1x "-denoise" 修复网）。
    "real-cugan" to ModelScene("动漫", false),
    // Phhofm 照片 compact / 4x。
    "nomosuni-compact" to ModelScene("照片", false),
    "parimg-compact" to ModelScene("照片", false),
    "nomosuni-span" to ModelScene("照片", false),
    "lsdir-compact" to ModelScene("照片", false),
    "nomos2-otf" to ModelScene("照片", false),
    "nomos8ksc" to ModelScene("照片", false),
    "nomoswebphoto" to ModelScene("照片", false),
    // 1x 修复网 —— 原尺寸清理，不放大。
    "dejpg" to ModelScene("照片·修复", false),
    "denoise" to ModelScene("照片·修复", false),
    "deh264" to ModelScene("视频·修复", false),
    "archivist-medium" to ModelScene("动漫·修复", true),
    "archivist-rough" to ModelScene("动漫·修复", false),
    "nes-composite" to ModelScene("游戏·修复", false),
    "genesis-cleanup" to ModelScene("游戏·修复", false),
    // 社区动漫修复梯（一条规则同时命中 HFA2k 两个变体）。
    "hfa2k" to ModelScene("动漫", false),
    "modernspanimation" to ModelScene("动漫", false),
    // 社区经典。
    "ultrasharp" to ModelScene("综合", true),
    "remacri" to ModelScene("综合", true),
    "animesharp" to ModelScene("动漫", true),
    "animejanai" to ModelScene("动漫", false),
    "nmkd-siax" to ModelScene("综合", false),
    "bsrgan" to ModelScene("照片", false),
)

/** 目录内模型返回场景标注；目录外（用户自导入）返回 null，调用方不画徽标。 */
fun classifyModelScene(name: String): ModelScene? {
    val key = normalize(name)
    return SCENE_RULES.firstOrNull { (pattern, _) -> pattern in key }?.second
}
