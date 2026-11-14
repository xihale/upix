package io.upix.data.model

import androidx.annotation.StringRes
import androidx.compose.runtime.Immutable
import io.upix.R

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
    /** 展示标签资源，如动漫、照片·修复。 */
    @param:StringRes val labelRes: Int,
    /** 该场景下的首选推荐（README 点名）。 */
    val recommended: Boolean
)

/** 与 ModelManager::find_model 相同的归一化：小写 + 下划线/空格归并为连字符。 */
private fun normalize(name: String): String =
    name.lowercase().replace('_', '-').replace(' ', '-')

private val SCENE_RULES: List<Pair<String, ModelScene>> = listOf(
    // Real-ESRGAN v3 compact —— 双端默认推荐。
    "real-esrganv3-anime" to ModelScene(R.string.model_scene_anime, true),
    "real-esrganv3-general" to ModelScene(R.string.model_scene_photo, true),
    // x4plus 家族；anime 变体必须先于照片基础款。
    "real-esrgan-x4plus-anime" to ModelScene(R.string.model_scene_anime, false),
    "real-esrgan-x4plus" to ModelScene(R.string.model_scene_photo, true),
    "real-esrgan-x2plus" to ModelScene(R.string.model_scene_photo, false),
    // Real-CUGAN —— 动漫专精（先于 1x "-denoise" 修复网）。
    "real-cugan" to ModelScene(R.string.model_scene_anime, false),
    // Phhofm 照片 compact / 4x。
    "nomosuni-compact" to ModelScene(R.string.model_scene_photo, false),
    "parimg-compact" to ModelScene(R.string.model_scene_photo, false),
    "nomosuni-span" to ModelScene(R.string.model_scene_photo, false),
    "lsdir-compact" to ModelScene(R.string.model_scene_photo, false),
    "nomos2-otf" to ModelScene(R.string.model_scene_photo, false),
    "nomos8ksc" to ModelScene(R.string.model_scene_photo, false),
    "nomoswebphoto" to ModelScene(R.string.model_scene_photo, false),
    // 1x 修复网 —— 原尺寸清理，不放大。
    "dejpg" to ModelScene(R.string.model_scene_photo_restore, false),
    "denoise" to ModelScene(R.string.model_scene_photo_restore, false),
    "deh264" to ModelScene(R.string.model_scene_video_restore, false),
    "archivist-medium" to ModelScene(R.string.model_scene_anime_restore, true),
    "archivist-rough" to ModelScene(R.string.model_scene_anime_restore, false),
    "nes-composite" to ModelScene(R.string.model_scene_game_restore, false),
    "genesis-cleanup" to ModelScene(R.string.model_scene_game_restore, false),
    // 社区动漫修复梯（一条规则同时命中 HFA2k 两个变体）。
    "hfa2k" to ModelScene(R.string.model_scene_anime, false),
    "modernspanimation" to ModelScene(R.string.model_scene_anime, false),
    // 社区经典。
    "ultrasharp" to ModelScene(R.string.model_scene_general, true),
    "remacri" to ModelScene(R.string.model_scene_general, true),
    "animesharp" to ModelScene(R.string.model_scene_anime, true),
    "animejanai" to ModelScene(R.string.model_scene_anime, false),
    "nmkd-siax" to ModelScene(R.string.model_scene_general, false),
    "bsrgan" to ModelScene(R.string.model_scene_photo, false),
)

/** 目录内模型返回场景标注；目录外（用户自导入）返回 null，调用方不画徽标。 */
fun classifyModelScene(name: String): ModelScene? {
    val key = normalize(name)
    return SCENE_RULES.firstOrNull { (pattern, _) -> pattern in key }?.second
}
