package com.crossscale.data.model

import androidx.compose.runtime.Immutable

/** GitHub "models" release manifest 中的一项（见 scripts/publish_models.py）。 */
@Immutable
data class RemoteModel(
    val name: String,
    val scale: Int,
    val sizeBytes: Long,
    val sha256: String,
    val url: String
)
