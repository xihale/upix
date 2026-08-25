package com.upix.data.model

import androidx.compose.runtime.Immutable

/** PC 侧模型库中的一个模型（仅元数据，文件本体在 PC 上）。 */
@Immutable
data class PcModel(
    val name: String,
    val scaleFactor: Int,
    val totalSize: Long
)

/**
 * 模型在双端的存在情况。name 为展示名：
 * 手机端存在时以手机 ModelInfo 为准，否则以 PC 元数据合成。
 */
@Immutable
data class ModelLibraryEntry(
    val name: String,
    val scaleFactor: Int,
    val totalSize: Long,
    val onPhone: Boolean,
    val onPc: Boolean,
    /** onPhone == true 时的本地模型（含落盘路径）；PC-only 时为 null。 */
    val localModel: ModelInfo?
)
