package com.crossscale.data.model

import androidx.compose.runtime.Immutable

@Immutable
data class ModelInfo(
    val name: String,
    val scaleFactor: Int,
    val inputChannels: Int,
    val tileSize: Int,
    val totalSize: Long,
    val hash: String,
    val paramPath: String,
    val binPath: String
)
