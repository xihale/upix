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

// Batch-queue state machine extracted from ProcessingViewModel: queue CRUD with
// PENDING/RUNNING dedup and FAILED reuse, default syncing, retry configuration,
// and the run-next/handle-finished chaining that drives queued batches one at
// a time. All view-state mutations and run kickoff go through the owning
// ProcessingViewModel, so the recursive coupling with the run engine (engine
// finally -> handleQueuedRunFinished -> runNextQueuedBatchIfNeeded ->
// startProcessingInternal) still resolves inside the ViewModel, exactly as
// before the extraction.
internal class ProcessingQueueController(
    private val viewModel: ProcessingViewModel,
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
        // Reuse an existing non-completed batch with the same configuration:
        // a PENDING/RUNNING one short-circuits the duplicate enqueue, a FAILED
        // one is reset to PENDING.
        val reusableBatch = viewModel.currentUiState().queueItems.firstOrNull { item ->
            item.status != PersistedQueueItemStatus.COMPLETED &&
                item.matchesBatchConfiguration(
                    source = normalizedImagePath,
                    modelName = normalizedModelName,
                    distributedMode = distributedMode,
                    outputDirectoryUri = normalizedOutputDirectoryUri,
                    overwriteOriginal = overwriteOriginal,
                    backupOriginal = backupOriginal,
                    processNestedFolders = processNestedFolders
                )
        }

        if (reusableBatch != null && reusableBatch.status != PersistedQueueItemStatus.FAILED) {
            updateItem(reusableBatch.id) { item ->
                if (item.status == PersistedQueueItemStatus.PENDING && item.retryFailedOnly) {
                    item.copy(
                        retryFailedOnly = false,
                        lastErrorMessage = null,
                        lastOutputUri = null
                    )
                } else {
                    item
                }
            }
            logRepository.info(
                "ProcessingViewModel",
                "Skipped duplicate enqueue for active batch ${reusableBatch.id.take(8)} source=$normalizedImagePath"
            )
            estimateQueuedBatchExpectedItems(reusableBatch.id, normalizedImagePath)
            return reusableBatch.id
        }

        if (reusableBatch != null) {
            updateItem(reusableBatch.id) { item ->
                item.copy(
                    status = PersistedQueueItemStatus.PENDING,
                    retryFailedOnly = false,
                    lastErrorMessage = null,
                    lastOutputUri = null
                )
            }
            logRepository.info(
                "ProcessingViewModel",
                "Reused failed queued batch ${reusableBatch.id.take(8)} for source=$normalizedImagePath"
            )
            estimateQueuedBatchExpectedItems(reusableBatch.id, normalizedImagePath)
            return reusableBatch.id
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
        viewModel.updateUiState { state ->
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
                val task = viewModel.currentUiState().task
                if (task != null) {
                    inputSourceRepository.resolveImageInputs(source = source, nested = task.processNestedFolders).size
                } else {
                    inputSourceRepository.resolveImageInputs(source = source, nested = true).size // Default to nested for queue estimation if no active task
                }
            }
            updateItem(queueItemId) { item ->
                item.copy(expectedItems = expectedItems)
            }
        }
    }

    fun startBatchQueue(preferredBatchId: String? = null) {
        val normalizedPreferredBatchId = preferredBatchId?.trim()?.ifBlank { null }
        val hasPending = viewModel.currentUiState().queueItems.any { item ->
            item.status == PersistedQueueItemStatus.PENDING
        }
        if (!hasPending) {
            logRepository.warning("ProcessingViewModel", "No pending batch in queue")
            return
        }
        viewModel.updateUiState { state ->
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
        viewModel.updateUiState { state ->
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
        if (!viewModel.currentUiState().isQueueRunning) {
            return
        }
        viewModel.updateUiState { state ->
            state.copy(isQueueRunning = false)
        }
        logRepository.info("ProcessingViewModel", "Paused batch queue")
    }

    fun retryQueuedBatch(batchId: String) {
        val normalizedBatchId = normalizeBatchId(batchId) ?: return
        var updated = false
        updateItem(normalizedBatchId) { item ->
            if (item.status != PersistedQueueItemStatus.RUNNING) {
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
        if (updated) {
            logRepository.info("ProcessingViewModel", "Reset queued batch for retry: $normalizedBatchId")
            if (viewModel.currentUiState().isQueueRunning && !viewModel.isProcessingJobActive()) {
                runNextQueuedBatchIfNeeded()
            }
        }
    }

    fun retryQueuedBatchFailedOnly(batchId: String) {
        val normalizedBatchId = normalizeBatchId(batchId) ?: return
        val queueItem = viewModel.currentUiState().queueItems.firstOrNull { item ->
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
        updateItem(normalizedBatchId) { item ->
            if (item.status != PersistedQueueItemStatus.RUNNING) {
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
        if (updated) {
            logRepository.info(
                "ProcessingViewModel",
                "Configured queued batch for failed-only retry: $normalizedBatchId"
            )
            if (viewModel.currentUiState().isQueueRunning && !viewModel.isProcessingJobActive()) {
                runNextQueuedBatchIfNeeded()
            }
        }
    }

    fun removeQueuedBatch(batchId: String) {
        val normalizedBatchId = normalizeBatchId(batchId) ?: return
        if (viewModel.currentActiveQueueItemId() == normalizedBatchId && viewModel.isProcessingJobActive()) {
            logRepository.warning("ProcessingViewModel", "Cannot remove running batch from queue")
            return
        }
        val beforeSize = viewModel.currentUiState().queueItems.size
        viewModel.updateUiState { state ->
            state.copy(
                queueItems = state.queueItems.filterNot { item ->
                    item.id == normalizedBatchId
                }
            )
        }
        if (viewModel.currentUiState().queueItems.size != beforeSize) {
            logRepository.info("ProcessingViewModel", "Removed queued batch: $normalizedBatchId")
        }
    }

    fun sourceForQueuedBatch(batchId: String): String? {
        val normalizedBatchId = normalizeBatchId(batchId) ?: return null
        return viewModel.currentUiState().queueItems.firstOrNull { item ->
            item.id == normalizedBatchId
        }?.source
    }

    fun hasQueuedBatchForSource(source: String): Boolean {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return false
        }
        return viewModel.currentUiState().queueItems.any { item ->
            item.source == normalizedSource
        }
    }

    fun queuedSources(): Set<String> {
        return viewModel.currentUiState().queueItems
            .map { item -> item.source }
            .toSet()
    }

    fun removeQueuedBatchesBySource(source: String): Int {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return 0
        }
        val removedBatchIds = mutableListOf<String>()
        viewModel.currentUiState().queueItems
            .filter { item ->
                item.source == normalizedSource &&
                    !(item.id == viewModel.currentActiveQueueItemId() && viewModel.isProcessingJobActive())
            }
            .forEach { item ->
                val beforeSize = viewModel.currentUiState().queueItems.size
                removeQueuedBatch(item.id)
                if (viewModel.currentUiState().queueItems.size < beforeSize) {
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
        if (viewModel.currentUiState().queueItems.isEmpty()) {
            return
        }
        val runningBatchId = viewModel.currentActiveQueueItemId()
        viewModel.updateUiState { state ->
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
        val normalizedBatchId = normalizeBatchId(batchId) ?: return null
        return viewModel.currentUiState().queueItems.firstOrNull { item ->
            item.id == normalizedBatchId && item.status == PersistedQueueItemStatus.COMPLETED
        }?.lastOutputUri
    }

    fun pauseBatchQueueForManualRun() {
        if (!viewModel.currentUiState().isQueueRunning) {
            return
        }
        viewModel.updateUiState { state ->
            state.copy(isQueueRunning = false)
        }
        logRepository.info("ProcessingViewModel", "Paused batch queue for manual run")
    }

    fun runNextQueuedBatchIfNeeded(preferredBatchId: String? = null) {
        if (!viewModel.currentUiState().isQueueRunning) {
            return
        }
        if (viewModel.isProcessingJobActive()) {
            return
        }

        val normalizedPreferredBatchId = preferredBatchId?.trim()?.ifBlank { null }
        val nextBatch = if (normalizedPreferredBatchId == null) {
            viewModel.currentUiState().queueItems.firstOrNull { item ->
                item.status == PersistedQueueItemStatus.PENDING
            }
        } else {
            viewModel.currentUiState().queueItems.firstOrNull { item ->
                item.id == normalizedPreferredBatchId && item.status == PersistedQueueItemStatus.PENDING
            } ?: viewModel.currentUiState().queueItems.firstOrNull { item ->
                item.status == PersistedQueueItemStatus.PENDING
            }
        }
        if (nextBatch == null) {
            viewModel.updateUiState { state ->
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
        updateItem(nextBatch.id) { item ->
            item.copy(
                distributedMode = effectiveDistributedMode,
                status = PersistedQueueItemStatus.RUNNING,
                attempts = item.attempts + 1,
                lastErrorMessage = null,
                lastOutputUri = null
            )
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
        val started = viewModel.startProcessingInternal(
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
        if (!viewModel.isProcessingJobActive()) {
            handleQueuedRunFinished(
                queueItemId = nextBatch.id,
                status = viewModel.currentUiState().task?.status,
                lastErrorMessage = viewModel.currentUiState().task?.lastErrorMessage
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
                updateItem(queueItemId) { item ->
                    item.copy(
                        status = PersistedQueueItemStatus.COMPLETED,
                        retryFailedOnly = false,
                        lastErrorMessage = null
                    )
                }
                logRepository.info(
                    "ProcessingViewModel",
                    "Queued batch finished: ${queueItemId.take(8)}"
                )
                if (viewModel.currentUiState().isQueueRunning) {
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
                viewModel.updateUiState { state ->
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

    // Applies transform to the queue item with the given id (no-op if absent).
    private fun updateItem(id: String, transform: (ProcessingQueueItemSummary) -> ProcessingQueueItemSummary) {
        viewModel.updateUiState { state ->
            state.copy(
                queueItems = state.queueItems.map { item ->
                    if (item.id == id) transform(item) else item
                }
            )
        }
    }

    // Trims a caller-supplied batch id; null when blank.
    private fun normalizeBatchId(batchId: String): String? =
        batchId.trim().ifBlank { null }

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
