package com.crossscale.viewmodel

import com.crossscale.data.model.ProcessingStatus
import com.crossscale.data.repository.InputSourceResolver
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.canRetryFailedOnly
import com.crossscale.network.CoordinatorClient
import java.util.UUID
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// State-exchange surface between ProcessingQueueController and the ViewModel
// that owns the fused ui-state and the run job. The controller never holds
// _uiState, processingJob or activeQueueItemId — they stay in the ViewModel.
internal interface ProcessingQueueControllerHost {
    // Equivalent of MutableStateFlow.update on the VM's fused ui-state flow.
    fun updateUiState(transform: (ProcessingUiState) -> ProcessingUiState)

    // Snapshot read of the current fused ui-state.
    fun currentUiState(): ProcessingUiState

    // Whether a run coroutine is currently active (VM: processingJob?.isActive == true).
    fun isProcessingJobActive(): Boolean

    // Queue item id of the currently running batch, if any (VM: activeQueueItemId).
    fun currentActiveQueueItemId(): String?

    // Entry point into the run engine (VM private startProcessingInternal delegate).
    fun startProcessingInternal(
        imagePath: String,
        modelName: String,
        distributedMode: Boolean,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean,
        resumeCheckpoint: ProcessingCheckpoint?,
        retryFailedOnly: Boolean,
        resumePendingUris: Set<String>?,
        queueItemId: String?
    ): Boolean
}

// Batch-queue state machine extracted from ProcessingViewModel: queue CRUD with
// PENDING/RUNNING dedup and FAILED reuse, default syncing, retry configuration,
// and the run-next/handle-finished chaining that drives queued batches one at a
// time. All view-state mutations and run kickoff go through the host, so the
// recursive coupling with the run engine (engine finally -> handleQueuedRunFinished
// -> runNextQueuedBatchIfNeeded -> startProcessingInternal) still resolves inside
// the ViewModel, exactly as before the extraction.
internal class ProcessingQueueController(
    private val host: ProcessingQueueControllerHost,
    private val inputSourceRepository: InputSourceResolver,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val logRepository: LogRepository,
    private val coordinatorClient: CoordinatorClient? = null,
    private val scope: CoroutineScope,
    private val ioDispatcher: CoroutineDispatcher
) {
    fun enqueueBatch(
        imagePath: String,
        modelName: String,
        distributedMode: Boolean,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean = false,
        backupOriginal: Boolean = true,
        processNestedFolders: Boolean = true
    ): String? {
        val normalizedImagePath = imagePath.trim()
        val normalizedModelName = modelName.trim()
        if (normalizedImagePath.isBlank()) {
            logRepository.warning("ProcessingViewModel", "Cannot enqueue batch: no input source")
            return null
        }
        if (normalizedModelName.isBlank()) {
            logRepository.warning("ProcessingViewModel", "Cannot enqueue batch: no model selected")
            return null
        }

        val normalizedOutputDirectoryUri = outputDirectoryUri?.trim()?.ifBlank { null }
        val reusablePendingOrRunningBatchId = host.currentUiState().queueItems.firstOrNull { item ->
            (item.status == PersistedQueueItemStatus.PENDING || item.status == PersistedQueueItemStatus.RUNNING) &&
                item.matchesBatchConfiguration(
                    source = normalizedImagePath,
                    modelName = normalizedModelName,
                    distributedMode = distributedMode,
                    outputDirectoryUri = normalizedOutputDirectoryUri,
                    overwriteOriginal = overwriteOriginal,
                    backupOriginal = backupOriginal,
                    processNestedFolders = processNestedFolders
                )
        }?.id

        if (reusablePendingOrRunningBatchId != null) {
            host.updateUiState { state ->
                state.copy(
                    queueItems = state.queueItems.map { item ->
                        if (
                            item.id == reusablePendingOrRunningBatchId &&
                            item.status == PersistedQueueItemStatus.PENDING &&
                            item.retryFailedOnly
                        ) {
                            item.copy(
                                retryFailedOnly = false,
                                lastErrorMessage = null,
                                lastOutputUri = null
                            )
                        } else {
                            item
                        }
                    }
                )
            }
            logRepository.info(
                "ProcessingViewModel",
                "Skipped duplicate enqueue for active batch ${reusablePendingOrRunningBatchId.take(8)} source=$normalizedImagePath"
            )
            estimateQueuedBatchExpectedItems(reusablePendingOrRunningBatchId, normalizedImagePath)
            return reusablePendingOrRunningBatchId
        }

        val reusableFailedBatchId = host.currentUiState().queueItems.firstOrNull { item ->
            item.status == PersistedQueueItemStatus.FAILED &&
                item.matchesBatchConfiguration(
                    source = normalizedImagePath,
                    modelName = normalizedModelName,
                    distributedMode = distributedMode,
                    outputDirectoryUri = normalizedOutputDirectoryUri,
                    overwriteOriginal = overwriteOriginal,
                    backupOriginal = backupOriginal,
                    processNestedFolders = processNestedFolders
                )
        }?.id

        if (reusableFailedBatchId != null) {
            host.updateUiState { state ->
                state.copy(
                    queueItems = state.queueItems.map { item ->
                        if (item.id == reusableFailedBatchId) {
                            item.copy(
                                status = PersistedQueueItemStatus.PENDING,
                                retryFailedOnly = false,
                                lastErrorMessage = null,
                                lastOutputUri = null
                            )
                        } else {
                            item
                        }
                    }
                )
            }
            logRepository.info(
                "ProcessingViewModel",
                "Reused failed queued batch ${reusableFailedBatchId.take(8)} for source=$normalizedImagePath"
            )
            estimateQueuedBatchExpectedItems(reusableFailedBatchId, normalizedImagePath)
            return reusableFailedBatchId
        }

        val queueItem = ProcessingQueueItemSummary(
            id = UUID.randomUUID().toString(),
            source = normalizedImagePath,
            modelName = normalizedModelName,
            distributedMode = distributedMode,
            outputDirectoryUri = normalizedOutputDirectoryUri,
            overwriteOriginal = overwriteOriginal,
            backupOriginal = backupOriginal,
            processNestedFolders = processNestedFolders,
            createdAtEpochMs = System.currentTimeMillis()
        )
        host.updateUiState { state ->
            state.copy(queueItems = state.queueItems + queueItem)
        }
        logRepository.info(
            "ProcessingViewModel",
            "Enqueued batch ${queueItem.id.take(8)} source=$normalizedImagePath model=$normalizedModelName"
        )

        estimateQueuedBatchExpectedItems(queueItem.id, normalizedImagePath)
        return queueItem.id
    }

    private fun estimateQueuedBatchExpectedItems(
        queueItemId: String,
        source: String
    ) {
        scope.launch {
            val expectedItems = withContext(ioDispatcher) {
                val task = host.currentUiState().task
                if (task != null) {
                    inputSourceRepository.resolveImageInputs(source = source, nested = task.processNestedFolders).size
                } else {
                    inputSourceRepository.resolveImageInputs(source = source, nested = true).size // Default to nested for queue estimation if no active task
                }
            }
            host.updateUiState { state ->
                state.copy(
                    queueItems = state.queueItems.map { item ->
                        if (item.id == queueItemId) {
                            item.copy(expectedItems = expectedItems)
                        } else {
                            item
                        }
                    }
                )
            }
        }
    }

    fun startBatchQueue(preferredBatchId: String? = null) {
        val normalizedPreferredBatchId = preferredBatchId?.trim()?.ifBlank { null }
        val hasPending = host.currentUiState().queueItems.any { item ->
            item.status == PersistedQueueItemStatus.PENDING
        }
        if (!hasPending) {
            logRepository.warning("ProcessingViewModel", "No pending batch in queue")
            return
        }
        host.updateUiState { state ->
            state.copy(isQueueRunning = true)
        }
        runNextQueuedBatchIfNeeded(preferredBatchId = normalizedPreferredBatchId)
    }

    fun syncPendingQueueDefaults(
        distributedMode: Boolean,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean
    ) {
        val normalizedOutputDirectoryUri = outputDirectoryUri?.trim()?.ifBlank { null }
        var updatedCount = 0
        host.updateUiState { state ->
            state.copy(
                queueItems = state.queueItems.map { item ->
                    if (item.status != PersistedQueueItemStatus.PENDING) {
                        item
                    } else {
                        val updatedItem = item.copy(
                            distributedMode = distributedMode,
                            outputDirectoryUri = normalizedOutputDirectoryUri,
                            overwriteOriginal = overwriteOriginal,
                            backupOriginal = backupOriginal,
                            processNestedFolders = processNestedFolders
                        )
                        if (updatedItem != item) {
                            updatedCount += 1
                        }
                        updatedItem
                    }
                }
            )
        }
        if (updatedCount > 0) {
            logRepository.info(
                "ProcessingViewModel",
                "Updated $updatedCount pending queued batch(es) with latest run options"
            )
        }
    }

    fun pauseBatchQueue() {
        if (!host.currentUiState().isQueueRunning) {
            return
        }
        host.updateUiState { state ->
            state.copy(isQueueRunning = false)
        }
        logRepository.info("ProcessingViewModel", "Paused batch queue")
    }

    fun retryQueuedBatch(batchId: String) {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return
        }
        var updated = false
        host.updateUiState { state ->
            state.copy(
                queueItems = state.queueItems.map { item ->
                    if (item.id == normalizedBatchId && item.status != PersistedQueueItemStatus.RUNNING) {
                        updated = true
                        item.copy(
                            status = PersistedQueueItemStatus.PENDING,
                            retryFailedOnly = false,
                            lastErrorMessage = null,
                            lastOutputUri = null
                        )
                    } else {
                        item
                    }
                }
            )
        }
        if (updated) {
            logRepository.info("ProcessingViewModel", "Reset queued batch for retry: $normalizedBatchId")
            if (host.currentUiState().isQueueRunning && !host.isProcessingJobActive()) {
                runNextQueuedBatchIfNeeded()
            }
        }
    }

    fun retryQueuedBatchFailedOnly(batchId: String) {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return
        }
        val queueItem = host.currentUiState().queueItems.firstOrNull { item ->
            item.id == normalizedBatchId
        } ?: return
        if (queueItem.status == PersistedQueueItemStatus.RUNNING) {
            return
        }

        val checkpoint = findMatchingCheckpointForQueuedBatch(queueItem)
        if (checkpoint == null || !checkpoint.canRetryFailedOnly()) {
            logRepository.warning(
                "ProcessingViewModel",
                "Cannot configure failed-only retry for batch $normalizedBatchId: no matching checkpoint with failed items"
            )
            return
        }

        var updated = false
        host.updateUiState { state ->
            state.copy(
                queueItems = state.queueItems.map { item ->
                    if (item.id == normalizedBatchId && item.status != PersistedQueueItemStatus.RUNNING) {
                        updated = true
                        item.copy(
                            status = PersistedQueueItemStatus.PENDING,
                            retryFailedOnly = true,
                            lastErrorMessage = null,
                            lastOutputUri = null
                        )
                    } else {
                        item
                    }
                }
            )
        }
        if (updated) {
            logRepository.info(
                "ProcessingViewModel",
                "Configured queued batch for failed-only retry: $normalizedBatchId"
            )
            if (host.currentUiState().isQueueRunning && !host.isProcessingJobActive()) {
                runNextQueuedBatchIfNeeded()
            }
        }
    }

    fun removeQueuedBatch(batchId: String) {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return
        }
        if (host.currentActiveQueueItemId() == normalizedBatchId && host.isProcessingJobActive()) {
            logRepository.warning("ProcessingViewModel", "Cannot remove running batch from queue")
            return
        }
        val beforeSize = host.currentUiState().queueItems.size
        host.updateUiState { state ->
            state.copy(
                queueItems = state.queueItems.filterNot { item ->
                    item.id == normalizedBatchId
                }
            )
        }
        if (host.currentUiState().queueItems.size != beforeSize) {
            logRepository.info("ProcessingViewModel", "Removed queued batch: $normalizedBatchId")
        }
    }

    fun sourceForQueuedBatch(batchId: String): String? {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return null
        }
        return host.currentUiState().queueItems.firstOrNull { item ->
            item.id == normalizedBatchId
        }?.source
    }

    fun hasQueuedBatchForSource(source: String): Boolean {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return false
        }
        return host.currentUiState().queueItems.any { item ->
            item.source == normalizedSource
        }
    }

    fun queuedSources(): Set<String> {
        return host.currentUiState().queueItems
            .map { item -> item.source }
            .toSet()
    }

    fun removeQueuedBatchesBySource(source: String): Int {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return 0
        }
        val removedBatchIds = mutableListOf<String>()
        host.currentUiState().queueItems
            .filter { item ->
                item.source == normalizedSource &&
                    !(item.id == host.currentActiveQueueItemId() && host.isProcessingJobActive())
            }
            .forEach { item ->
                val beforeSize = host.currentUiState().queueItems.size
                removeQueuedBatch(item.id)
                if (host.currentUiState().queueItems.size < beforeSize) {
                    removedBatchIds += item.id
                }
            }
        if (removedBatchIds.isNotEmpty()) {
            logRepository.info(
                "ProcessingViewModel",
                "Removed ${removedBatchIds.size} queued batch(es) for source=$normalizedSource"
            )
        }
        return removedBatchIds.size
    }

    fun clearBatchQueue() {
        if (host.currentUiState().queueItems.isEmpty()) {
            return
        }
        val runningBatchId = host.currentActiveQueueItemId()
        host.updateUiState { state ->
            val remaining = if (runningBatchId == null) {
                emptyList()
            } else {
                state.queueItems.filter { item ->
                    item.id == runningBatchId && item.status == PersistedQueueItemStatus.RUNNING
                }
            }
            state.copy(
                queueItems = remaining,
                isQueueRunning = false
            )
        }
        logRepository.info("ProcessingViewModel", "Cleared queued batches")
    }

    fun outputUriForQueuedBatch(batchId: String): String? {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return null
        }
        return host.currentUiState().queueItems.firstOrNull { item ->
            item.id == normalizedBatchId && item.status == PersistedQueueItemStatus.COMPLETED
        }?.lastOutputUri
    }

    fun pauseBatchQueueForManualRun() {
        if (!host.currentUiState().isQueueRunning) {
            return
        }
        host.updateUiState { state ->
            state.copy(isQueueRunning = false)
        }
        logRepository.info("ProcessingViewModel", "Paused batch queue for manual run")
    }

    fun runNextQueuedBatchIfNeeded(preferredBatchId: String? = null) {
        if (!host.currentUiState().isQueueRunning) {
            return
        }
        if (host.isProcessingJobActive()) {
            return
        }

        val normalizedPreferredBatchId = preferredBatchId?.trim()?.ifBlank { null }
        val nextBatch = if (normalizedPreferredBatchId == null) {
            host.currentUiState().queueItems.firstOrNull { item ->
                item.status == PersistedQueueItemStatus.PENDING
            }
        } else {
            host.currentUiState().queueItems.firstOrNull { item ->
                item.id == normalizedPreferredBatchId && item.status == PersistedQueueItemStatus.PENDING
            } ?: host.currentUiState().queueItems.firstOrNull { item ->
                item.status == PersistedQueueItemStatus.PENDING
            }
        }
        if (nextBatch == null) {
            host.updateUiState { state ->
                state.copy(isQueueRunning = false)
            }
            logRepository.info("ProcessingViewModel", "Batch queue finished")
            return
        }

        val resumeCheckpoint = findMatchingCheckpointForQueuedBatch(nextBatch)
        if (nextBatch.retryFailedOnly) {
            if (resumeCheckpoint == null || !resumeCheckpoint.canRetryFailedOnly()) {
                handleQueuedRunFinished(
                    queueItemId = nextBatch.id,
                    status = ProcessingStatus.FAILED,
                    lastErrorMessage = "没有可重试的失败项"
                )
                return
            }
        }
        val effectiveDistributedMode = coordinatorClient?.isConnected() == true
        val updatedQueueItems = host.currentUiState().queueItems.map { item ->
            if (item.id == nextBatch.id) {
                item.copy(
                    distributedMode = effectiveDistributedMode,
                    status = PersistedQueueItemStatus.RUNNING,
                    attempts = item.attempts + 1,
                    lastErrorMessage = null,
                    lastOutputUri = null
                )
            } else {
                item
            }
        }
        host.updateUiState { state ->
            state.copy(queueItems = updatedQueueItems)
        }
        logRepository.info(
            "ProcessingViewModel",
            "Starting queued batch ${nextBatch.id.take(8)} model=${nextBatch.modelName} retryFailedOnly=${nextBatch.retryFailedOnly}"
        )
        if (resumeCheckpoint != null) {
            logRepository.info(
                "ProcessingViewModel",
                "Queued batch ${nextBatch.id.take(8)} will resume from checkpoint ${resumeCheckpoint.taskId}"
            )
        }
        val started = host.startProcessingInternal(
            imagePath = nextBatch.source,
            modelName = nextBatch.modelName,
            distributedMode = effectiveDistributedMode,
            outputDirectoryUri = nextBatch.outputDirectoryUri,
            overwriteOriginal = nextBatch.overwriteOriginal,
            backupOriginal = nextBatch.backupOriginal,
            processNestedFolders = nextBatch.processNestedFolders,
            resumeCheckpoint = resumeCheckpoint,
            retryFailedOnly = nextBatch.retryFailedOnly,
            resumePendingUris = null,
            queueItemId = nextBatch.id
        )
        if (!started) {
            handleQueuedRunFinished(
                queueItemId = nextBatch.id,
                status = ProcessingStatus.FAILED,
                lastErrorMessage = "无法启动该批次"
            )
            return
        }
        if (!host.isProcessingJobActive()) {
            handleQueuedRunFinished(
                queueItemId = nextBatch.id,
                status = host.currentUiState().task?.status,
                lastErrorMessage = host.currentUiState().task?.lastErrorMessage
            )
        }
    }

    fun handleQueuedRunFinished(
        queueItemId: String,
        status: ProcessingStatus?,
        lastErrorMessage: String?
    ) {
        val normalizedStatus = status ?: ProcessingStatus.FAILED
        when (normalizedStatus) {
            ProcessingStatus.COMPLETED -> {
                host.updateUiState { state ->
                    state.copy(
                        queueItems = state.queueItems.map { item ->
                            if (item.id == queueItemId) {
                                item.copy(
                                    status = PersistedQueueItemStatus.COMPLETED,
                                    retryFailedOnly = false,
                                    lastErrorMessage = null
                                )
                            } else {
                                item
                            }
                        }
                    )
                }
                logRepository.info(
                    "ProcessingViewModel",
                    "Queued batch finished: ${queueItemId.take(8)}"
                )
                if (host.currentUiState().isQueueRunning) {
                    runNextQueuedBatchIfNeeded()
                }
            }

            ProcessingStatus.FAILED,
            ProcessingStatus.CANCELED,
            ProcessingStatus.IDLE -> {
                val failureMessage = lastErrorMessage ?: when (normalizedStatus) {
                    ProcessingStatus.CANCELED -> "已取消"
                    ProcessingStatus.IDLE -> "批次未完成"
                    else -> "批次失败"
                }
                host.updateUiState { state ->
                    state.copy(
                        queueItems = state.queueItems.filterNot { item ->
                            item.id == queueItemId
                        },
                        isQueueRunning = false
                    )
                }
                logRepository.warning(
                    "ProcessingViewModel",
                    "Queued batch stopped and removed: ${queueItemId.take(8)} status=$normalizedStatus message=$failureMessage"
                )
            }

            ProcessingStatus.RUNNING -> {
                // Ignore transitional status.
            }
        }
    }

    private fun findMatchingCheckpointForQueuedBatch(
        queueItem: ProcessingQueueItemSummary
    ): ProcessingCheckpoint? {
        val checkpoint = checkpointRepository.loadCheckpoint() ?: return null
        if (checkpoint.pendingItems.isEmpty()) {
            return null
        }

        val sameSource = checkpoint.source.trim() == queueItem.source.trim()
        val sameModel = checkpoint.modelName.trim() == queueItem.modelName.trim()
        val sameMode = checkpoint.distributedMode == queueItem.distributedMode
        val sameOverwrite = checkpoint.overwriteOriginal == queueItem.overwriteOriginal
        val sameBackup = checkpoint.backupOriginal == queueItem.backupOriginal
        val sameOutputUri = checkpoint.outputDirectoryUri?.trim().orEmpty() ==
            queueItem.outputDirectoryUri?.trim().orEmpty()
        if (!sameSource || !sameModel || !sameMode || !sameOverwrite || !sameBackup || !sameOutputUri) {
            return null
        }
        return checkpoint
    }
}
