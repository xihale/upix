package com.crossscale.data.model

import com.crossscale.data.repository.ProcessingFailureDetail

enum class ProcessingStatus {
    IDLE,
    RUNNING,
    COMPLETED,
    FAILED,
    CANCELED
}

data class ProcessingTask(
    val id: String,
    val imagePath: String,
    val modelName: String,
    val distributedMode: Boolean,
    val outputDirectory: String? = null,
    val outputDirectoryUri: String? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true,
    val currentItemName: String? = null,
    val completedItems: Int = 0,
    val pendingItems: Int = 0,
    val failedItems: Int = 0,
    val failedItemNames: List<String> = emptyList(),
    val failureDetails: List<ProcessingFailureDetail> = emptyList(),
    val totalItems: Int = 1,
    val lastProcessedItemName: String? = null,
    val lastProcessorLabel: String? = null,
    val recentProcessorAttributions: List<String> = emptyList(),
    val lastErrorMessage: String? = null,
    val progress: Float,
    val status: ProcessingStatus
)
