package io.upix.viewmodel

import androidx.compose.runtime.Immutable
import io.upix.data.model.ProcessingTask
import io.upix.data.repository.BatchOutputDirectory
import io.upix.data.repository.PersistedQueueItemStatus
import io.upix.data.repository.ProcessingFailureDetail
import io.upix.data.repository.ResolvedImageInput

// List fields are treated as immutable by convention (never mutated after
// construction), which keeps these states skippable for Compose.

@Immutable
data class ProcessingUiState(
    val task: ProcessingTask? = null,
    val isProcessing: Boolean = false,
    val checkpointSummary: ProcessingCheckpointSummary? = null,
    val canRetryFailedOnly: Boolean = false,
    val queueItems: List<ProcessingQueueItemSummary> = emptyList(),
    val isQueueRunning: Boolean = false
)

@Immutable
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
    val skipSrMarkedInputs: Boolean = true,
    val createdAtEpochMs: Long,
    val expectedItems: Int? = null,
    val status: PersistedQueueItemStatus = PersistedQueueItemStatus.PENDING,
    val attempts: Int = 0,
    val lastErrorMessage: String? = null,
    val lastOutputUri: String? = null
)

@Immutable
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

@Immutable
data class ProcessingCheckpointItemSummary(
    val uri: String,
    val displayName: String,
    val attempts: Int,
    val isFailed: Boolean,
    val lastErrorMessage: String?
)

// Visible for the viewmodel package only. Held as a local value in
// ProcessingRunPlanner.buildRunSetup and consumed by ProcessingRunEngine.
internal data class ProcessingRunSetup(
    val allImages: List<ResolvedImageInput>,
    val runImages: List<ResolvedImageInput>,
    val pendingImages: List<ResolvedImageInput>,
    val completedItems: Int,
    val totalItems: Int,
    /** Inputs skipped because they carry this tool's own SR marker. */
    val skippedItems: Int = 0,
    val outputDirectory: BatchOutputDirectory,
    val failureDetails: MutableMap<String, ProcessingFailureDetail>
)
