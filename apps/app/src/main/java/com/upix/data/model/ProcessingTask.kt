package com.upix.data.model

import androidx.compose.runtime.Immutable
import com.upix.data.repository.ProcessingFailureDetail

enum class ProcessingStatus {
    IDLE,
    RUNNING,
    COMPLETED,
    FAILED,
    CANCELED
}

// List fields would otherwise be inferred unstable by the Compose compiler;
// by convention the lists are never mutated after construction.
@Immutable
data class ProcessingTask(
    val id: String,
    val imagePath: String,
    val modelName: String,
    val distributedMode: Boolean,
    val outputDirectory: String? = null,
    val outputDirectoryUri: String? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true,
    val processNestedFolders: Boolean = true,
    val skipSrMarkedInputs: Boolean = true,
    val currentItemName: String? = null,
    val completedItems: Int = 0,
    val pendingItems: Int = 0,
    val failedItems: Int = 0,
    /** Already-super-resolved inputs skipped by the marker guard. */
    val skippedItems: Int = 0,
    val failureDetails: List<ProcessingFailureDetail> = emptyList(),
    val totalItems: Int = 1,
    val lastErrorMessage: String? = null,
    val progress: Float,
    val status: ProcessingStatus
)
