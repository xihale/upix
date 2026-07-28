package com.crossscale.viewmodel

import com.crossscale.data.model.ProcessingTask
import com.crossscale.data.repository.BatchOutputDirectory
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.data.repository.ProcessingFailureDetail
import com.crossscale.data.repository.ResolvedImageInput

data class ProcessingUiState(
    val task: ProcessingTask? = null,
    val isProcessing: Boolean = false,
    val canResume: Boolean = false,
    val checkpointSummary: ProcessingCheckpointSummary? = null,
    val canRetryFailedOnly: Boolean = false,
    val expectedTotalItems: Int? = null,
    val isEstimatingInputCount: Boolean = false,
    val queueItems: List<ProcessingQueueItemSummary> = emptyList(),
    val isQueueRunning: Boolean = false
)

data class ProcessingQueueItemSummary(
    val id: String,
    val source: String,
    val modelName: String,
    val distributedMode: Boolean,
    val retryFailedOnly: Boolean = false,
    val outputDirectoryUri: String? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true,
    val processNestedFolders: Boolean = true,
    val createdAtEpochMs: Long,
    val expectedItems: Int? = null,
    val status: PersistedQueueItemStatus = PersistedQueueItemStatus.PENDING,
    val attempts: Int = 0,
    val lastErrorMessage: String? = null,
    val lastOutputUri: String? = null
)

data class ProcessingCheckpointSummary(
    val taskId: String,
    val source: String,
    val modelName: String,
    val distributedMode: Boolean,
    val outputDirectory: String,
    val outputDirectoryUri: String?,
    val updatedAtEpochMs: Long,
    val totalItems: Int,
    val completedItems: Int,
    val pendingItems: Int,
    val failedItems: Int,
    val deferredItems: Int,
    val statusLabel: String,
    val lastErrorMessage: String?,
    val pendingItemDetails: List<ProcessingCheckpointItemSummary> = emptyList()
)

data class ProcessingCheckpointItemSummary(
    val uri: String,
    val displayName: String,
    val attempts: Int,
    val isFailed: Boolean,
    val lastErrorMessage: String?
)

// Visible for the viewmodel package only. Held as a local value in
// ProcessingViewModel.buildRunSetup and consumed by startProcessingInternal.
internal data class ProcessingRunSetup(
    val allImages: List<ResolvedImageInput>,
    val runImages: List<ResolvedImageInput>,
    val pendingImages: List<ResolvedImageInput>,
    val completedItems: Int,
    val totalItems: Int,
    val outputDirectory: BatchOutputDirectory,
    val failureDetails: MutableMap<String, ProcessingFailureDetail>
)
