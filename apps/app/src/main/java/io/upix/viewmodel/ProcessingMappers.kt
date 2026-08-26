package io.upix.viewmodel

import io.upix.R
import io.upix.data.repository.PersistedQueueItem
import io.upix.data.repository.ProcessingCheckpoint
import io.upix.data.repository.ProcessingQueueSnapshot
import io.upix.data.repository.failedPendingCount
import io.upix.util.StringProvider

// Pure mappers extracted from ProcessingViewModel. No state, no I/O.
// internal = visible to the :app module only (UI/data layers do not call these).

internal fun ProcessingQueueItemSummary.matchesBatchConfiguration(
    source: String,
    modelName: String,
    distributedMode: Boolean,
    outputDirectoryUri: String?,
    overwriteOriginal: Boolean,
    backupOriginal: Boolean,
    processNestedFolders: Boolean,
    skipSrMarkedInputs: Boolean = true
): Boolean {
    return this.source == source &&
        this.modelName == modelName &&
        this.distributedMode == distributedMode &&
        this.outputDirectoryUri == outputDirectoryUri &&
        this.overwriteOriginal == overwriteOriginal &&
        this.backupOriginal == backupOriginal &&
        this.processNestedFolders == processNestedFolders &&
        this.skipSrMarkedInputs == skipSrMarkedInputs
}

internal fun ProcessingUiState.toQueueSnapshot(): ProcessingQueueSnapshot {
    return ProcessingQueueSnapshot(
        items = queueItems.map { queueItem ->
            queueItem.toPersistedQueueItem()
        },
        isQueueRunning = isQueueRunning
    )
}

internal fun ProcessingQueueItemSummary.toPersistedQueueItem(): PersistedQueueItem {
    return PersistedQueueItem(
        id = id,
        source = source,
        modelName = modelName,
        distributedMode = distributedMode,
        retryFailedOnly = retryFailedOnly,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        // 子目录开关必须随持久化往返，否则重启后丢失（旧文件缺该键时反序列化为默认 true）。
        processNestedFolders = processNestedFolders,
        skipSrMarkedInputs = skipSrMarkedInputs,
        createdAtEpochMs = createdAtEpochMs,
        expectedItems = expectedItems,
        status = status,
        attempts = attempts,
        lastErrorMessage = lastErrorMessage,
        lastOutputUri = lastOutputUri
    )
}

internal fun PersistedQueueItem.toUiQueueItem(): ProcessingQueueItemSummary {
    return ProcessingQueueItemSummary(
        id = id,
        source = source,
        modelName = modelName,
        distributedMode = distributedMode,
        retryFailedOnly = retryFailedOnly,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        processNestedFolders = processNestedFolders,
        skipSrMarkedInputs = skipSrMarkedInputs,
        createdAtEpochMs = createdAtEpochMs,
        expectedItems = expectedItems,
        status = status,
        attempts = attempts,
        lastErrorMessage = lastErrorMessage,
        lastOutputUri = lastOutputUri
    )
}

internal fun ProcessingCheckpoint.toUiSummary(stringProvider: StringProvider): ProcessingCheckpointSummary {
    val pendingCount = pendingItems.size.coerceAtLeast(0)
    val failedCount = failedPendingCount().coerceAtLeast(0)
    val deferredCount = (pendingCount - failedCount).coerceAtLeast(0)
    val completedCount = completedItems.coerceAtLeast(0)
    val totalCount = maxOf(totalItems, completedCount + pendingCount)
    val failureByUri = failureDetails.associateBy { detail -> detail.uri }
    val pendingItemDetails = pendingItems
        .map { pendingItem ->
            val failure = failureByUri[pendingItem.uri]
            val displayName = pendingItem.displayName.ifBlank {
                pendingItem.uri.substringAfterLast('/', pendingItem.uri)
            }
            ProcessingCheckpointItemSummary(
                uri = pendingItem.uri,
                displayName = displayName,
                attempts = maxOf(
                    pendingItem.attempts,
                    failure?.attempts ?: 0
                ),
                isFailed = failure != null,
                lastErrorMessage = failure?.errorMessage
            )
        }
        .sortedWith(
            compareByDescending<ProcessingCheckpointItemSummary> { item -> item.isFailed }
                .thenByDescending { item -> item.attempts }
                .thenBy { item -> item.displayName.lowercase() }
                .thenBy { item -> item.uri }
        )
    val checkpointStatus = when {
        pendingCount <= 0 && failedCount <= 0 ->
            stringProvider.getString(R.string.common_status_completed)
        failedCount > 0 ->
            stringProvider.getString(R.string.checkpoint_status_resumable_with_failures)
        else ->
            stringProvider.getString(R.string.checkpoint_status_resumable)
    }
    return ProcessingCheckpointSummary(
        taskId = taskId,
        source = source,
        modelName = modelName,
        distributedMode = distributedMode,
        outputDirectory = outputDirectory,
        outputDirectoryUri = outputDirectoryUri,
        updatedAtEpochMs = updatedAtEpochMs,
        totalItems = totalCount,
        completedItems = completedCount.coerceAtMost(totalCount),
        pendingItems = pendingCount,
        failedItems = failedCount,
        deferredItems = deferredCount,
        statusLabel = checkpointStatus,
        lastErrorMessage = lastErrorMessage,
        pendingItemDetails = pendingItemDetails
    )
}
