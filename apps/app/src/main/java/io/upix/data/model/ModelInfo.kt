package io.upix.data.model

import androidx.compose.runtime.Immutable

@Immutable
data class ModelInfo(
    val name: String,
    val scaleFactor: Int,
    val totalSize: Long,
    val paramPath: String,
    val binPath: String
)
