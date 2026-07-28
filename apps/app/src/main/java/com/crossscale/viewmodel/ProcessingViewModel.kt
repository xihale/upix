package com.crossscale.viewmodel

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.crossscale.AppContainer
import com.crossscale.data.repository.BatchOutputStore
import com.crossscale.data.repository.ImageProcessingSession
import com.crossscale.data.repository.ImageProcessingStore
import com.crossscale.data.repository.InputSourceResolver
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.repository.PersistedQueueItem
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.data.repository.ProcessingQueueSnapshot
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.data.model.ProcessingTask
import com.crossscale.data.repository.BatchOutputDirectory
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.ProcessingFailureDetail
import com.crossscale.data.repository.ProcessingPendingItem
import com.crossscale.data.repository.ProcessingQueueStore
import com.crossscale.data.repository.ResolvedImageInput
import com.crossscale.data.repository.canRetryFailedOnly
import com.crossscale.data.repository.failedPendingCount
import com.crossscale.network.CoordinatorClient
import kotlinx.coroutines.CoroutineDispatcher
import java.util.LinkedHashMap
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class ProcessingViewModel(
    private val logRepository: LogRepository,
    private val modelRepository: ModelProvider,
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val queueRepository: ProcessingQueueStore,
    private val nativeImageProcessingRepository: ImageProcessingStore,
    private val coordinatorClient: CoordinatorClient? = null,
    private val deviceRepository: DeviceRepository? = null,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
    private val defaultDispatcher: CoroutineDispatcher = Dispatchers.Default
) : ViewModel() {
    private val _uiState = MutableStateFlow(ProcessingUiState())
    val uiState: StateFlow<ProcessingUiState> = _uiState.asStateFlow()

    private val backupRestore = BackupRestoreManager(
        inputSourceRepository = inputSourceRepository,
        batchOutputRepository = batchOutputRepository,
        logRepository = logRepository,
        scope = viewModelScope,
        ioDispatcher = ioDispatcher
    )

    private val inputCountEstimator = InputCountEstimator(
        inputSourceRepository = inputSourceRepository,
        scope = viewModelScope,
        ioDispatcher = ioDispatcher,
        onEstimatingStarted = {
            _uiState.update { state -> state.copy(isEstimatingInputCount = true) }
        },
        onResult = { expectedTotal, estimating ->
            _uiState.update { state ->
                state.copy(expectedTotalItems = expectedTotal, isEstimatingInputCount = estimating)
            }
        }
    )

    private var processingJob: Job? = null
    private var activeQueueItemId: String? = null

    init {
        restorePersistedQueueAsync()
        restorePersistedCheckpointAsync()

        viewModelScope.launch {
            checkpointRepository.checkpoint.collectLatest { checkpoint ->
                _uiState.update { state ->
                    state.copy(
                        canResume = checkpoint != null,
                        checkpointSummary = checkpoint?.toUiSummary(),
                        canRetryFailedOnly = checkpoint?.canRetryFailedOnly() == true
                    )
                }
            }
        }

        viewModelScope.launch {
            uiState
                .map { state -> state.toQueueSnapshot() }
                .distinctUntilChanged()
                .collectLatest { snapshot ->
                    queueRepository.saveQueue(snapshot)
                }
        }
    }

    fun startProcessing(
        imagePath: String,
        modelName: String,
        distributedMode: Boolean,
        outputDirectoryUri: String? = null,
        overwriteOriginal: Boolean = false,
        backupOriginal: Boolean = true,
        processNestedFolders: Boolean = true
    ) {
        pauseBatchQueueForManualRun()
        startProcessingInternal(
            imagePath = imagePath,
            modelName = modelName,
            distributedMode = distributedMode,
            outputDirectoryUri = outputDirectoryUri,
            overwriteOriginal = overwriteOriginal,
            backupOriginal = backupOriginal,
            processNestedFolders = processNestedFolders,
            resumeCheckpoint = null,
            retryFailedOnly = false,
            resumePendingUris = null,
            queueItemId = null
        )
    }

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
        val reusablePendingOrRunningBatchId = _uiState.value.queueItems.firstOrNull { item ->
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
            _uiState.update { state ->
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

        val reusableFailedBatchId = _uiState.value.queueItems.firstOrNull { item ->
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
            _uiState.update { state ->
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
        _uiState.update { state ->
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
        viewModelScope.launch {
            val expectedItems = withContext(ioDispatcher) {
                val task = _uiState.value.task
                if (task != null) {
                    inputSourceRepository.resolveImageInputs(source = source, nested = task.processNestedFolders).size
                } else {
                    inputSourceRepository.resolveImageInputs(source = source, nested = true).size // Default to nested for queue estimation if no active task
                }
            }
            _uiState.update { state ->
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
        val hasPending = _uiState.value.queueItems.any { item ->
            item.status == PersistedQueueItemStatus.PENDING
        }
        if (!hasPending) {
            logRepository.warning("ProcessingViewModel", "No pending batch in queue")
            return
        }
        _uiState.update { state ->
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
        _uiState.update { state ->
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
        if (!_uiState.value.isQueueRunning) {
            return
        }
        _uiState.update { state ->
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
        _uiState.update { state ->
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
            if (_uiState.value.isQueueRunning && processingJob?.isActive != true) {
                runNextQueuedBatchIfNeeded()
            }
        }
    }

    fun retryQueuedBatchFailedOnly(batchId: String) {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return
        }
        val queueItem = _uiState.value.queueItems.firstOrNull { item ->
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
        _uiState.update { state ->
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
            if (_uiState.value.isQueueRunning && processingJob?.isActive != true) {
                runNextQueuedBatchIfNeeded()
            }
        }
    }

    fun removeQueuedBatch(batchId: String) {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return
        }
        if (activeQueueItemId == normalizedBatchId && processingJob?.isActive == true) {
            logRepository.warning("ProcessingViewModel", "Cannot remove running batch from queue")
            return
        }
        val beforeSize = _uiState.value.queueItems.size
        _uiState.update { state ->
            state.copy(
                queueItems = state.queueItems.filterNot { item ->
                    item.id == normalizedBatchId
                }
            )
        }
        if (_uiState.value.queueItems.size != beforeSize) {
            logRepository.info("ProcessingViewModel", "Removed queued batch: $normalizedBatchId")
        }
    }

    fun sourceForQueuedBatch(batchId: String): String? {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return null
        }
        return _uiState.value.queueItems.firstOrNull { item ->
            item.id == normalizedBatchId
        }?.source
    }

    fun hasQueuedBatchForSource(source: String): Boolean {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return false
        }
        return _uiState.value.queueItems.any { item ->
            item.source == normalizedSource
        }
    }

    fun queuedSources(): Set<String> {
        return _uiState.value.queueItems
            .map { item -> item.source }
            .toSet()
    }

    fun removeQueuedBatchesBySource(source: String): Int {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return 0
        }
        val removedBatchIds = mutableListOf<String>()
        _uiState.value.queueItems
            .filter { item ->
                item.source == normalizedSource &&
                    !(item.id == activeQueueItemId && processingJob?.isActive == true)
            }
            .forEach { item ->
                val beforeSize = _uiState.value.queueItems.size
                removeQueuedBatch(item.id)
                if (_uiState.value.queueItems.size < beforeSize) {
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
        if (_uiState.value.queueItems.isEmpty()) {
            return
        }
        val runningBatchId = activeQueueItemId
        _uiState.update { state ->
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

    private fun restorePersistedQueueAsync() {
        viewModelScope.launch {
            val restoredSnapshot = withContext(ioDispatcher) {
                queueRepository.loadQueue()
            }
            if (restoredSnapshot.items.isEmpty() && !restoredSnapshot.isQueueRunning) {
                return@launch
            }
            _uiState.update { state ->
                state.copy(
                    queueItems = restoredSnapshot.items.map { item ->
                        item.toUiQueueItem()
                    },
                    isQueueRunning = restoredSnapshot.isQueueRunning
                )
            }
            logRepository.info(
                "ProcessingViewModel",
                "Restored ${restoredSnapshot.items.size} queued batch(es)"
            )
        }
    }

    private fun restorePersistedCheckpointAsync() {
        viewModelScope.launch {
            withContext(ioDispatcher) {
                checkpointRepository.loadCheckpoint()
            }
        }
    }

    fun refreshExpectedTotal(imagePath: String?) =
        inputCountEstimator.refreshExpectedTotal(imagePath)

    fun resumeLastProcessing() {
        if (processingJob?.isActive == true) {
            return
        }
        pauseBatchQueueForManualRun()
        val checkpoint = checkpointRepository.loadCheckpoint()
        if (checkpoint == null) {
            logRepository.warning("ProcessingViewModel", "No checkpoint found to resume")
            return
        }
        startProcessingInternal(
            imagePath = checkpoint.source,
            modelName = checkpoint.modelName,
            distributedMode = checkpoint.distributedMode,
            outputDirectoryUri = checkpoint.outputDirectoryUri,
            overwriteOriginal = checkpoint.overwriteOriginal,
            backupOriginal = checkpoint.backupOriginal,
            processNestedFolders = checkpoint.processNestedFolders,
            resumeCheckpoint = checkpoint,
            retryFailedOnly = false,
            resumePendingUris = null,
            queueItemId = checkpoint.taskId
        )
    }

    fun retryFailedItems() {
        if (processingJob?.isActive == true) {
            return
        }
        pauseBatchQueueForManualRun()
        val checkpoint = checkpointRepository.loadCheckpoint()
        if (checkpoint != null) {
            if (!checkpoint.canRetryFailedOnly()) {
                logRepository.warning("ProcessingViewModel", "No failed items available in checkpoint")
                return
            }
            startProcessingInternal(
                imagePath = checkpoint.source,
                modelName = checkpoint.modelName,
                distributedMode = checkpoint.distributedMode,
                outputDirectoryUri = checkpoint.outputDirectoryUri,
                overwriteOriginal = checkpoint.overwriteOriginal,
                backupOriginal = checkpoint.backupOriginal,
                processNestedFolders = checkpoint.processNestedFolders,
                resumeCheckpoint = checkpoint,
                retryFailedOnly = true,
                resumePendingUris = null,
                queueItemId = checkpoint.taskId
            )
            return
        }
        val task = _uiState.value.task
        if (task == null || task.failedItems <= 0) {
            logRepository.warning("ProcessingViewModel", "No failed items available for retry")
            return
        }
        startProcessing(
            imagePath = task.imagePath,
            modelName = task.modelName,
            distributedMode = task.distributedMode,
            outputDirectoryUri = task.outputDirectoryUri,
            overwriteOriginal = task.overwriteOriginal,
            backupOriginal = task.backupOriginal,
            processNestedFolders = task.processNestedFolders
        )
    }

    fun resumePendingItem(uri: String) {
        if (processingJob?.isActive == true) {
            return
        }
        pauseBatchQueueForManualRun()
        val normalizedUri = uri.trim()
        if (normalizedUri.isBlank()) {
            return
        }
        val checkpoint = checkpointRepository.loadCheckpoint()
        if (checkpoint == null) {
            logRepository.warning("ProcessingViewModel", "No checkpoint found to resume item")
            return
        }
        startProcessingInternal(
            imagePath = checkpoint.source,
            modelName = checkpoint.modelName,
            distributedMode = checkpoint.distributedMode,
            outputDirectoryUri = checkpoint.outputDirectoryUri,
            overwriteOriginal = checkpoint.overwriteOriginal,
            backupOriginal = checkpoint.backupOriginal,
            processNestedFolders = checkpoint.processNestedFolders,
            resumeCheckpoint = checkpoint,
            retryFailedOnly = false,
            resumePendingUris = setOf(normalizedUri),
            queueItemId = checkpoint.taskId
        )
    }

    fun retryFailedItem(uri: String) {
        if (processingJob?.isActive == true) {
            return
        }
        pauseBatchQueueForManualRun()
        val normalizedUri = uri.trim()
        if (normalizedUri.isBlank()) {
            return
        }
        val checkpoint = checkpointRepository.loadCheckpoint()
        if (checkpoint == null) {
            logRepository.warning("ProcessingViewModel", "No checkpoint found to retry failed item")
            return
        }
        startProcessingInternal(
            imagePath = checkpoint.source,
            modelName = checkpoint.modelName,
            distributedMode = checkpoint.distributedMode,
            outputDirectoryUri = checkpoint.outputDirectoryUri,
            overwriteOriginal = checkpoint.overwriteOriginal,
            backupOriginal = checkpoint.backupOriginal,
            processNestedFolders = checkpoint.processNestedFolders,
            resumeCheckpoint = checkpoint,
            retryFailedOnly = true,
            resumePendingUris = setOf(normalizedUri),
            queueItemId = checkpoint.taskId
        )
    }

    fun discardCheckpoint() {
        if (processingJob?.isActive == true) {
            logRepository.warning("ProcessingViewModel", "Cannot discard checkpoint while processing is running")
            return
        }
        if (checkpointRepository.loadCheckpoint() == null) {
            logRepository.info("ProcessingViewModel", "No checkpoint to discard")
            return
        }
        checkpointRepository.clearCheckpoint()
        logRepository.info("ProcessingViewModel", "Discarded processing checkpoint")
    }

    suspend fun hasBackupFor(imagePath: String, nested: Boolean) =
        backupRestore.hasBackupFor(imagePath, nested)

    fun restoreBackup(imagePath: String, nested: Boolean, onComplete: (Int, Int) -> Unit) =
        backupRestore.restoreBackup(imagePath, nested, onComplete)

    private fun startProcessingInternal(
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
    ): Boolean {
        if (imagePath.isBlank()) {
            logRepository.error("ProcessingViewModel", "Cannot start processing: no image selected")
            return false
        }
        if (retryFailedOnly && resumeCheckpoint == null) {
            logRepository.warning(
                "ProcessingViewModel",
                "Cannot run failed-only retry without checkpoint context"
            )
            return false
        }
        if (!resumePendingUris.isNullOrEmpty() && resumeCheckpoint == null) {
            logRepository.warning(
                "ProcessingViewModel",
                "Cannot resume selected pending items without checkpoint context"
            )
            return false
        }
        if (processingJob?.isActive == true) {
            return false
        }

        val coordinatorConnected = coordinatorClient?.isConnected() == true
        val localConnected = deviceRepository?.isLocalDeviceConnected() ?: true
        val effectiveDistributedMode = distributedMode || (coordinatorConnected && !localConnected)
        if (!effectiveDistributedMode && !localConnected) {
            logRepository.warning(
                "ProcessingViewModel",
                "Local client is disconnected. Connect local client or remote coordinator before processing."
            )
            return false
        }

        val modelInfo = modelRepository.getModelByName(modelName)
        if (modelInfo == null) {
            val errorMessage = "Model not found: $modelName"
            logRepository.error("ProcessingViewModel", errorMessage)
            _uiState.update { state ->
                state.copy(
                    task = ProcessingTask(
                        id = resumeCheckpoint?.taskId ?: UUID.randomUUID().toString(),
                        imagePath = imagePath,
                        modelName = modelName,
                        distributedMode = effectiveDistributedMode,
                        overwriteOriginal = overwriteOriginal,
                        backupOriginal = backupOriginal,
                        processNestedFolders = processNestedFolders,
                        pendingItems = 0,
                        progress = 0.0f,
                        status = ProcessingStatus.FAILED,
                        lastErrorMessage = errorMessage
                    ),
                    isProcessing = false
                )
            }
            return false
        }

        val setup = buildRunSetup(
            imagePath = imagePath,
            modelName = modelName,
            requestedOutputDirectoryUri = outputDirectoryUri,
            overwriteOriginal = overwriteOriginal,
            backupOriginal = backupOriginal,
            processNestedFolders = processNestedFolders,
            resumeCheckpoint = resumeCheckpoint,
            retryFailedOnly = retryFailedOnly,
            resumePendingUris = resumePendingUris
        )
        if (setup == null) {
            logRepository.warning("ProcessingViewModel", "Unable to prepare processing inputs: $imagePath")
            return false
        }

        val taskId = resumeCheckpoint?.taskId ?: UUID.randomUUID().toString()
        val createdAt = resumeCheckpoint?.createdAtEpochMs ?: System.currentTimeMillis()
        val outputDirectory = setup.outputDirectory
        val runImages = setup.runImages
        val allImages = setup.allImages
        val pendingImages = setup.pendingImages
        val totalItems = setup.totalItems
        val completedCount = setup.completedItems
        val initialFailures = setup.failureDetails
        val initialFailureList = orderedFailureDetails(initialFailures)
        val initialLastError = resumeCheckpoint?.lastErrorMessage

        _uiState.update { state ->
            state.copy(
                expectedTotalItems = totalItems,
                isEstimatingInputCount = false
            )
        }

        if (pendingImages.isEmpty()) {
            checkpointRepository.clearCheckpoint()
            if (!queueItemId.isNullOrBlank()) {
                val existingOutputUri = batchOutputRepository.firstReadyOutputUri(
                    outputDirectory = outputDirectory,
                    inputs = allImages
                )
                if (!existingOutputUri.isNullOrBlank()) {
                    _uiState.update { state ->
                        state.copy(
                            queueItems = state.queueItems.map { item ->
                                if (item.id == queueItemId) {
                                    item.copy(lastOutputUri = existingOutputUri)
                                } else {
                                    item
                                }
                            }
                        )
                    }
                }
            }
            _uiState.update { state ->
                state.copy(
                    task = ProcessingTask(
                        id = taskId,
                        imagePath = imagePath,
                        modelName = modelName,
                        distributedMode = effectiveDistributedMode,
                        outputDirectory = outputDirectory.displayPath,
                        outputDirectoryUri = outputDirectory.outputDirectoryUri,
                        overwriteOriginal = outputDirectory.overwriteOriginal,
                        backupOriginal = outputDirectory.backupOriginal,
                        processNestedFolders = outputDirectory.processNestedFolders,
                        completedItems = totalItems,
                        pendingItems = 0,
                        totalItems = totalItems,
                        progress = 1.0f,
                        status = ProcessingStatus.COMPLETED
                    ),
                    isProcessing = false
                )
            }
            logRepository.info(
                "ProcessingViewModel",
                "All outputs already exist for source=$imagePath, outputDir=${outputDirectory.displayPath}"
            )
            return true
        }

        saveCheckpointSnapshot(
            taskId = taskId,
            imagePath = imagePath,
            modelName = modelName,
            distributedMode = effectiveDistributedMode,
            totalItems = totalItems,
            completedItems = completedCount,
            outputDirectory = outputDirectory.displayPath,
            outputDirectoryUri = outputDirectory.outputDirectoryUri,
            overwriteOriginal = outputDirectory.overwriteOriginal,
            backupOriginal = outputDirectory.backupOriginal,
            createdAtEpochMs = createdAt,
            pendingImages = pendingImages,
            failureDetails = initialFailures,
            lastErrorMessage = initialLastError
        )

        _uiState.update { state ->
            state.copy(
                    task = ProcessingTask(
                        id = taskId,
                        imagePath = imagePath,
                        modelName = modelName,
                        distributedMode = effectiveDistributedMode,
                        outputDirectory = outputDirectory.displayPath,
                        outputDirectoryUri = outputDirectory.outputDirectoryUri,
                        overwriteOriginal = outputDirectory.overwriteOriginal,
                        backupOriginal = outputDirectory.backupOriginal,
                        currentItemName = runImages.firstOrNull()?.displayName,
                    completedItems = completedCount,
                    pendingItems = pendingImages.size,
                    failedItems = initialFailures.size,
                    failedItemNames = failureNamesFromOrdered(initialFailureList),
                    failureDetails = initialFailureList,
                    totalItems = totalItems,
                    progress = progressOf(completedCount, totalItems),
                    status = ProcessingStatus.RUNNING,
                    lastErrorMessage = initialLastError
                ),
                isProcessing = true
            )
        }
        logRepository.info(
            "ProcessingViewModel",
            "Started task $taskId with model=$modelName, total=$totalItems, completed=$completedCount, pending=${pendingImages.size}, run=${runImages.size}, retryFailedOnly=$retryFailedOnly, outputDir=${outputDirectory.displayPath}"
        )

        val modelBasePath = modelBasePathFor(modelInfo)

        processingJob = viewModelScope.launch {
            var successfulCount = completedCount
            var lastError: String? = initialLastError
            val pendingByUri = LinkedHashMap<String, ResolvedImageInput>()
            pendingImages.forEach { pendingByUri[it.uri.toString()] = it }
            val failureDetailsByUri = initialFailures.toMutableMap()
            val distributedRemoteEnabled = effectiveDistributedMode && coordinatorConnected
            var processingSession: ImageProcessingSession? = null

            try {
                if (!distributedMode && distributedRemoteEnabled) {
                    logRepository.info(
                        "ProcessingViewModel",
                        "Remote coordinator connected, local processing disabled by default"
                    )
                } else if (distributedMode && !distributedRemoteEnabled) {
                    logRepository.warning(
                        "ProcessingViewModel",
                        "Distributed mode requested but remote coordinator is unavailable; fallback to local processing"
                    )
                }

                if (distributedRemoteEnabled) {
                    val endpoint = coordinatorClient.activeDeviceId().orEmpty()
                    logRepository.info(
                        "ProcessingViewModel",
                        "Distributed execution enabled, routing tasks to coordinator $endpoint"
                    )
                } else {
                    processingSession = withContext(defaultDispatcher) {
                        nativeImageProcessingRepository.openSession(
                            modelName = modelName,
                            modelBasePath = modelBasePath,
                            preferGpu = true
                        )
                    }

                    if (processingSession == null) {
                        val errorMessage = "Failed to initialize native session for model $modelName"
                        lastError = errorMessage
                        logRepository.error("ProcessingViewModel", errorMessage)
                        saveCheckpointSnapshot(
                            taskId = taskId,
                            imagePath = imagePath,
                            modelName = modelName,
                            distributedMode = effectiveDistributedMode,
                            totalItems = totalItems,
                            completedItems = successfulCount,
                            outputDirectory = outputDirectory.displayPath,
                            outputDirectoryUri = outputDirectory.outputDirectoryUri,
                            overwriteOriginal = outputDirectory.overwriteOriginal,
                            backupOriginal = outputDirectory.backupOriginal,
                            createdAtEpochMs = createdAt,
                            pendingImages = pendingByUri.values.toList(),
                            failureDetails = failureDetailsByUri,
                            lastErrorMessage = lastError
                        )
                        _uiState.update { state ->
                            val currentTask = state.task ?: return@update state
                            val orderedFailures = orderedFailureDetails(failureDetailsByUri)
                            state.copy(
                                task = currentTask.copy(
                                    status = ProcessingStatus.FAILED,
                                    currentItemName = null,
                                    pendingItems = pendingByUri.size,
                                    failedItems = orderedFailures.size,
                                    failedItemNames = failureNamesFromOrdered(orderedFailures),
                                    failureDetails = orderedFailures,
                                    lastErrorMessage = errorMessage
                                ),
                                isProcessing = false
                            )
                        }
                        return@launch
                    }
                }
                val session = processingSession

                for (item in runImages) {
                    if (!isActive) {
                        return@launch
                    }

                    _uiState.update { state ->
                        val currentTask = state.task ?: return@update state
                        state.copy(
                            task = currentTask.copy(
                                currentItemName = item.displayName,
                                pendingItems = pendingByUri.size,
                                progress = progressOf(successfulCount, totalItems)
                            )
                        )
                    }

                    val outputFile = batchOutputRepository.outputFileFor(outputDirectory = outputDirectory, input = item)

                    val startedAt = System.currentTimeMillis()
                    val result = withContext(defaultDispatcher) {
                        if (distributedRemoteEnabled) {
                            coordinatorClient.processInputRemotely(
                                input = item,
                                modelName = modelName,
                                tileSize = DISTRIBUTED_TILE_SIZE,
                                tileOverlap = DISTRIBUTED_TILE_OVERLAP,
                                outputFile = outputFile
                            )
                        } else {
                            checkNotNull(session).process(
                                input = item,
                                outputFile = outputFile
                            )
                        }
                    }
                    val elapsedMs = System.currentTimeMillis() - startedAt
                    val itemUri = item.uri.toString()
                    var latestProcessorLabel: String? = null
                    var latestAttributionEntry: String? = null

                    if (result.isSuccess) {
                        val commitResult = batchOutputRepository.commitOutput(
                            outputDirectory = outputDirectory,
                            input = item,
                            outputFile = outputFile
                        )
                        if (commitResult.isSuccess) {
                            successfulCount += 1
                            pendingByUri.remove(itemUri)
                            failureDetailsByUri.remove(itemUri)
                            val destination = commitResult.getOrNull()
                                ?: result.getOrNull()?.outputFile?.absolutePath
                                ?: outputFile.absolutePath
                            latestProcessorLabel = resolveProcessorLabel(
                                explicitProcessor = result.getOrNull()?.processorLabel,
                                distributedRemoteEnabled = distributedRemoteEnabled
                            )
                            latestAttributionEntry = "${item.displayName} · ${latestProcessorLabel}"
                            logRepository.info(
                                "ProcessingViewModel",
                                "Processed ${item.displayName} by ${latestProcessorLabel} -> $destination (${elapsedMs}ms, done=$successfulCount/$totalItems, pending=${pendingByUri.size})"
                            )
                            result.getOrNull()?.let { nativeResult ->
                                updateDeviceThroughputSample(
                                    distributedRemoteEnabled = distributedRemoteEnabled,
                                    elapsedMs = elapsedMs,
                                    inputWidth = nativeResult.inputWidth,
                                    inputHeight = nativeResult.inputHeight
                                )
                            }
                            if (!queueItemId.isNullOrBlank()) {
                                _uiState.update { state ->
                                    state.copy(
                                        queueItems = state.queueItems.map { queueItem ->
                                            if (queueItem.id == queueItemId) {
                                                queueItem.copy(lastOutputUri = destination)
                                            } else {
                                                queueItem
                                            }
                                        }
                                    )
                                }
                            }
                        } else {
                            lastError = commitResult.exceptionOrNull()?.message ?: "Failed to save output file"
                            val previousAttempts = failureDetailsByUri[itemUri]?.attempts ?: 0
                            failureDetailsByUri[itemUri] = ProcessingFailureDetail(
                                uri = itemUri,
                                displayName = item.displayName,
                                errorMessage = lastError,
                                attempts = previousAttempts + 1
                            )
                            batchOutputRepository.discardOutputFile(outputFile)
                            logRepository.error(
                                "ProcessingViewModel",
                                "Failed to store output for ${item.displayName}: $lastError (pending=${pendingByUri.size})"
                            )
                        }
                    } else {
                        lastError = result.exceptionOrNull()?.message ?: "Unknown processing error"
                        val previousAttempts = failureDetailsByUri[itemUri]?.attempts ?: 0
                        failureDetailsByUri[itemUri] = ProcessingFailureDetail(
                            uri = itemUri,
                            displayName = item.displayName,
                            errorMessage = lastError,
                            attempts = previousAttempts + 1
                        )
                        batchOutputRepository.discardOutputFile(outputFile)
                        logRepository.error(
                            "ProcessingViewModel",
                            "Failed to process ${item.displayName}: $lastError (pending=${pendingByUri.size})"
                        )
                    }

                    saveCheckpointSnapshot(
                        taskId = taskId,
                        imagePath = imagePath,
                        modelName = modelName,
                        distributedMode = effectiveDistributedMode,
                        totalItems = totalItems,
                        completedItems = successfulCount,
                        outputDirectory = outputDirectory.displayPath,
                        outputDirectoryUri = outputDirectory.outputDirectoryUri,
                        overwriteOriginal = outputDirectory.overwriteOriginal,
                        backupOriginal = outputDirectory.backupOriginal,
                        createdAtEpochMs = createdAt,
                        pendingImages = pendingByUri.values.toList(),
                        failureDetails = failureDetailsByUri,
                        lastErrorMessage = lastError
                    )

                    _uiState.update { state ->
                        val currentTask = state.task ?: return@update state
                        val orderedFailures = orderedFailureDetails(failureDetailsByUri)
                        val updatedAttributions = if (latestAttributionEntry == null) {
                            currentTask.recentProcessorAttributions
                        } else {
                            (currentTask.recentProcessorAttributions + latestAttributionEntry).takeLast(MAX_RECENT_ATTRIBUTIONS)
                        }
                        state.copy(
                            task = currentTask.copy(
                                progress = progressOf(successfulCount, totalItems),
                                currentItemName = item.displayName,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = failureNamesFromOrdered(orderedFailures),
                                failureDetails = orderedFailures,
                                lastProcessedItemName = if (latestProcessorLabel == null) {
                                    currentTask.lastProcessedItemName
                                } else {
                                    item.displayName
                                },
                                lastProcessorLabel = latestProcessorLabel ?: currentTask.lastProcessorLabel,
                                recentProcessorAttributions = updatedAttributions,
                                lastErrorMessage = lastError
                            )
                        )
                    }
                }

                if (pendingByUri.isEmpty()) {
                    checkpointRepository.clearCheckpoint()
                    _uiState.update { state ->
                        val currentTask = state.task ?: return@update state
                        state.copy(
                            task = currentTask.copy(
                                status = ProcessingStatus.COMPLETED,
                                progress = 1.0f,
                                completedItems = totalItems,
                                pendingItems = 0,
                                failedItems = 0,
                                failedItemNames = emptyList(),
                                failureDetails = emptyList(),
                                currentItemName = null,
                                lastErrorMessage = null
                            ),
                            isProcessing = false
                        )
                    }
                    logRepository.info(
                        "ProcessingViewModel",
                        "Task $taskId completed, outputDir=${outputDirectory.displayPath}"
                    )
                } else {
                    val hasFailedPending = failureDetailsByUri.isNotEmpty()
                    val terminalStatus = if (hasFailedPending) {
                        ProcessingStatus.FAILED
                    } else {
                        ProcessingStatus.IDLE
                    }
                    _uiState.update { state ->
                        val currentTask = state.task ?: return@update state
                        val orderedFailures = orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                status = terminalStatus,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = failureNamesFromOrdered(orderedFailures),
                                failureDetails = orderedFailures,
                                progress = progressOf(successfulCount, totalItems),
                                currentItemName = null,
                                lastErrorMessage = if (hasFailedPending) lastError else null
                            ),
                            isProcessing = false
                        )
                    }
                    if (hasFailedPending) {
                        logRepository.warning(
                            "ProcessingViewModel",
                            "Task $taskId finished with failed pending items (${successfulCount}/$totalItems success, pending=${pendingByUri.size}, failed=${failureDetailsByUri.size}). Resume is available."
                        )
                    } else {
                        logRepository.info(
                            "ProcessingViewModel",
                            "Task $taskId finished with deferred pending items only (${successfulCount}/$totalItems success, pending=${pendingByUri.size}). Resume is available."
                        )
                    }
                }
            } catch (cancellationException: CancellationException) {
                saveCheckpointSnapshot(
                    taskId = taskId,
                    imagePath = imagePath,
                    modelName = modelName,
                    distributedMode = effectiveDistributedMode,
                    totalItems = totalItems,
                    completedItems = successfulCount,
                    outputDirectory = outputDirectory.displayPath,
                    outputDirectoryUri = outputDirectory.outputDirectoryUri,
                    overwriteOriginal = outputDirectory.overwriteOriginal,
                    backupOriginal = outputDirectory.backupOriginal,
                    createdAtEpochMs = createdAt,
                    pendingImages = pendingByUri.values.toList(),
                    failureDetails = failureDetailsByUri,
                    lastErrorMessage = lastError
                )
                logRepository.warning("ProcessingViewModel", "Task $taskId canceled")
                throw cancellationException
            } catch (throwable: Throwable) {
                val errorMessage = throwable.message ?: "Unknown task failure"
                lastError = errorMessage
                saveCheckpointSnapshot(
                    taskId = taskId,
                    imagePath = imagePath,
                    modelName = modelName,
                    distributedMode = effectiveDistributedMode,
                    totalItems = totalItems,
                    completedItems = successfulCount,
                    outputDirectory = outputDirectory.displayPath,
                    outputDirectoryUri = outputDirectory.outputDirectoryUri,
                    overwriteOriginal = outputDirectory.overwriteOriginal,
                    backupOriginal = outputDirectory.backupOriginal,
                    createdAtEpochMs = createdAt,
                    pendingImages = pendingByUri.values.toList(),
                    failureDetails = failureDetailsByUri,
                    lastErrorMessage = lastError
                )
                    _uiState.update { state ->
                        val currentTask = state.task ?: return@update state
                        val orderedFailures = orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                status = ProcessingStatus.FAILED,
                                currentItemName = null,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = failureNamesFromOrdered(orderedFailures),
                                failureDetails = orderedFailures,
                                progress = progressOf(successfulCount, totalItems),
                                lastErrorMessage = errorMessage
                            ),
                            isProcessing = false
                        )
                    }
                logRepository.error(
                    "ProcessingViewModel",
                    "Task $taskId failed unexpectedly: $errorMessage"
                )
            } finally {
                processingSession?.close()
                if (_uiState.value.isProcessing) {
                    _uiState.update { state ->
                        val currentTask = state.task ?: return@update state
                        state.copy(
                            task = currentTask.copy(
                                currentItemName = null,
                                pendingItems = (currentTask.totalItems - currentTask.completedItems).coerceAtLeast(0)
                            ),
                            isProcessing = false
                        )
                    }
                }
                val completedQueueItemId = activeQueueItemId
                processingJob = null
                activeQueueItemId = null
                if (completedQueueItemId != null) {
                    handleQueuedRunFinished(
                        queueItemId = completedQueueItemId,
                        status = _uiState.value.task?.status,
                        lastErrorMessage = _uiState.value.task?.lastErrorMessage
                    )
                }
            }
        }
        activeQueueItemId = queueItemId
        return true
    }

    fun cancelProcessing() {
        processingJob?.cancel()
        processingJob = null
        _uiState.update { state ->
            val currentTask = state.task ?: return@update state.copy(isProcessing = false)
            state.copy(
                task = currentTask.copy(
                    status = ProcessingStatus.CANCELED,
                    currentItemName = null,
                    pendingItems = (currentTask.totalItems - currentTask.completedItems).coerceAtLeast(0)
                ),
                isProcessing = false
            )
        }
        val task = _uiState.value.task
        if (task != null) {
            logRepository.warning(
                "ProcessingViewModel",
                "Processing canceled, resume available (done=${task.completedItems}/${task.totalItems}, pending=${task.pendingItems})"
            )
        } else {
            logRepository.warning("ProcessingViewModel", "Processing canceled")
        }
    }

    fun outputUriForQueuedBatch(batchId: String): String? {
        val normalizedBatchId = batchId.trim()
        if (normalizedBatchId.isBlank()) {
            return null
        }
        return _uiState.value.queueItems.firstOrNull { item ->
            item.id == normalizedBatchId && item.status == PersistedQueueItemStatus.COMPLETED
        }?.lastOutputUri
    }

    private fun pauseBatchQueueForManualRun() {
        if (!_uiState.value.isQueueRunning) {
            return
        }
        _uiState.update { state ->
            state.copy(isQueueRunning = false)
        }
        logRepository.info("ProcessingViewModel", "Paused batch queue for manual run")
    }

    private fun runNextQueuedBatchIfNeeded(preferredBatchId: String? = null) {
        if (!_uiState.value.isQueueRunning) {
            return
        }
        if (processingJob?.isActive == true) {
            return
        }

        val normalizedPreferredBatchId = preferredBatchId?.trim()?.ifBlank { null }
        val nextBatch = if (normalizedPreferredBatchId == null) {
            _uiState.value.queueItems.firstOrNull { item ->
                item.status == PersistedQueueItemStatus.PENDING
            }
        } else {
            _uiState.value.queueItems.firstOrNull { item ->
                item.id == normalizedPreferredBatchId && item.status == PersistedQueueItemStatus.PENDING
            } ?: _uiState.value.queueItems.firstOrNull { item ->
                item.status == PersistedQueueItemStatus.PENDING
            }
        }
        if (nextBatch == null) {
            _uiState.update { state ->
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
                    lastErrorMessage = "No matching checkpoint with failed items for failed-only retry"
                )
                return
            }
        }
        val coordinatorConnected = coordinatorClient?.isConnected() == true
        val localConnected = deviceRepository?.isLocalDeviceConnected() ?: true
        val effectiveDistributedMode = nextBatch.distributedMode || (coordinatorConnected && !localConnected)
        val updatedQueueItems = _uiState.value.queueItems.map { item ->
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
        _uiState.update { state ->
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
        val started = startProcessingInternal(
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
                lastErrorMessage = "Unable to start queued batch"
            )
            return
        }
        if (processingJob?.isActive != true) {
            handleQueuedRunFinished(
                queueItemId = nextBatch.id,
                status = _uiState.value.task?.status,
                lastErrorMessage = _uiState.value.task?.lastErrorMessage
            )
        }
    }

    private fun handleQueuedRunFinished(
        queueItemId: String,
        status: ProcessingStatus?,
        lastErrorMessage: String?
    ) {
        val normalizedStatus = status ?: ProcessingStatus.FAILED
        when (normalizedStatus) {
            ProcessingStatus.COMPLETED -> {
                _uiState.update { state ->
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
                if (_uiState.value.isQueueRunning) {
                    runNextQueuedBatchIfNeeded()
                }
            }

            ProcessingStatus.FAILED,
            ProcessingStatus.CANCELED,
            ProcessingStatus.IDLE -> {
                val failureMessage = lastErrorMessage ?: when (normalizedStatus) {
                    ProcessingStatus.CANCELED -> "Canceled by user"
                    ProcessingStatus.IDLE -> "Batch not fully completed"
                    else -> "Queued batch failed"
                }
                _uiState.update { state ->
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

    private fun buildRunSetup(
        imagePath: String,
        modelName: String,
        requestedOutputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean,
        resumeCheckpoint: ProcessingCheckpoint?,
        retryFailedOnly: Boolean,
        resumePendingUris: Set<String>?
    ): ProcessingRunSetup? {
        val outputDirectory = batchOutputRepository.prepareOutputDirectory(
            source = imagePath,
            modelName = modelName,
            preferredTreeUri = requestedOutputDirectoryUri,
            overwriteOriginal = overwriteOriginal,
            backupOriginal = backupOriginal,
            processNestedFolders = processNestedFolders,
            resumeOutputDirectory = resumeCheckpoint?.outputDirectory,
            resumeOutputDirectoryUri = resumeCheckpoint?.outputDirectoryUri,
            resumeOverwriteOriginal = resumeCheckpoint?.overwriteOriginal,
            resumeBackupOriginal = resumeCheckpoint?.backupOriginal
        )
        if (!requestedOutputDirectoryUri.isNullOrBlank() && outputDirectory.outputDirectoryUri.isNullOrBlank()) {
            logRepository.warning(
                "ProcessingViewModel",
                "Unable to use selected output folder, fallback to app internal directory: ${outputDirectory.displayPath}"
            )
        }

        val task = _uiState.value.task
        val processNestedFolders = task?.processNestedFolders ?: true // Default to true if no active task

        if (resumeCheckpoint != null && resumeCheckpoint.pendingItems.isNotEmpty()) {
            val pendingImagesFromCheckpoint = resumeCheckpoint.pendingItems.mapNotNull { pendingItem ->
                val parsedUri = runCatching { Uri.parse(pendingItem.uri) }.getOrNull()
                if (parsedUri == null || parsedUri.toString().isBlank()) {
                    logRepository.warning(
                        "ProcessingViewModel",
                        "Skip invalid checkpoint URI: ${pendingItem.uri}"
                    )
                    null
                } else {
                    ResolvedImageInput(
                        uri = parsedUri,
                        displayName = pendingItem.displayName.ifBlank {
                            parsedUri.lastPathSegment ?: parsedUri.toString()
                        }
                    )
                }
            }

            if (pendingImagesFromCheckpoint.isEmpty()) {
                logRepository.warning(
                    "ProcessingViewModel",
                    "Checkpoint pending list is unusable, fallback to source scan"
                )
            } else {
                val pendingImages = pendingImagesFromCheckpoint.filterNot { input ->
                    batchOutputRepository.isOutputReady(outputDirectory = outputDirectory, input = input)
                }

                val autoRecoveredCount = pendingImagesFromCheckpoint.size - pendingImages.size
                val baseCompleted = resumeCheckpoint.completedItems.coerceAtLeast(0)
                val completedItems = (baseCompleted + autoRecoveredCount).coerceAtLeast(0)
                val inferredTotal = completedItems + pendingImages.size
                val totalItems = maxOf(resumeCheckpoint.totalItems, inferredTotal)

                val pendingUris = pendingImages.map { it.uri.toString() }.toSet()
                val failureDetails = resumeCheckpoint.failureDetails
                    .associateBy { it.uri }
                    .filterKeys { it in pendingUris }
                    .toMutableMap()

                val requestedPendingUris = resumePendingUris
                    ?.map { uri -> uri.trim() }
                    ?.filter { uri -> uri.isNotBlank() }
                    ?.toSet()
                    .orEmpty()
                val selectedPendingImages = if (requestedPendingUris.isEmpty()) {
                    pendingImages
                } else {
                    pendingImages.filter { input ->
                        input.uri.toString() in requestedPendingUris
                    }
                }
                if (requestedPendingUris.isNotEmpty() && selectedPendingImages.isEmpty()) {
                    logRepository.warning(
                        "ProcessingViewModel",
                        "Requested checkpoint items are no longer pending; nothing to resume"
                    )
                    return null
                }

                val runImages = if (retryFailedOnly) {
                    if (selectedPendingImages.isEmpty()) {
                        emptyList()
                    } else {
                        val failedPendingUris = failureDetails.keys
                        if (failedPendingUris.isEmpty()) {
                            logRepository.warning(
                                "ProcessingViewModel",
                                "Retry failed requested but no failed pending inputs remain in checkpoint"
                            )
                            return null
                        }
                        selectedPendingImages.filter { input ->
                            input.uri.toString() in failedPendingUris
                        }
                    }
                } else {
                    selectedPendingImages
                }
                if (retryFailedOnly && runImages.isEmpty()) {
                    logRepository.warning(
                        "ProcessingViewModel",
                        "Requested checkpoint items do not include failed entries"
                    )
                    return null
                }

                if (pendingImages.isEmpty()) {
                    return ProcessingRunSetup(
                        allImages = pendingImagesFromCheckpoint,
                        runImages = emptyList(),
                        pendingImages = emptyList(),
                        completedItems = totalItems,
                        totalItems = totalItems,
                        outputDirectory = outputDirectory,
                        failureDetails = mutableMapOf()
                    )
                }

                return ProcessingRunSetup(
                    allImages = pendingImagesFromCheckpoint,
                    runImages = runImages,
                    pendingImages = pendingImages,
                    completedItems = completedItems.coerceAtMost(totalItems),
                    totalItems = totalItems,
                    outputDirectory = outputDirectory,
                    failureDetails = failureDetails
                )
            }
        }

        val inputImages = inputSourceRepository.resolveImageInputs(source = imagePath, nested = processNestedFolders)
        val validImages = inputImages.filter { input ->
            // Add any validation logic here if needed
            true
        }
        if (validImages.isEmpty()) {
            return null
        }

        val pendingImages = validImages.filterNot { item ->
            batchOutputRepository.isOutputReady(outputDirectory = outputDirectory, input = item)
        }
        val totalItems = inputImages.size
        val completedItems = totalItems - pendingImages.size
        val pendingUris = pendingImages.map { it.uri.toString() }.toSet()
        val failureDetails = resumeCheckpoint?.failureDetails
            ?.associateBy { it.uri }
            ?.filterKeys { key -> key in pendingUris }
            ?.toMutableMap()
            ?: mutableMapOf()

        return ProcessingRunSetup(
            allImages = inputImages,
            runImages = pendingImages,
            pendingImages = pendingImages,
            completedItems = completedItems,
            totalItems = totalItems,
            outputDirectory = outputDirectory,
            failureDetails = failureDetails
        )
    }

    private fun saveCheckpointSnapshot(
        taskId: String,
        imagePath: String,
        modelName: String,
        distributedMode: Boolean,
        totalItems: Int,
        completedItems: Int,
        outputDirectory: String,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        createdAtEpochMs: Long,
        pendingImages: List<ResolvedImageInput>,
        failureDetails: Map<String, ProcessingFailureDetail>,
        lastErrorMessage: String?
    ) {
        val orderedPendingItems = pendingImages.map { item ->
            val uri = item.uri.toString()
            ProcessingPendingItem(
                uri = uri,
                displayName = item.displayName,
                attempts = failureDetails[uri]?.attempts ?: 0
            )
        }
        val orderedFailures = failureDetails.values
            .sortedWith(compareBy({ it.displayName.lowercase() }, { it.uri }))

        checkpointRepository.saveCheckpoint(
            ProcessingCheckpoint(
                taskId = taskId,
                source = imagePath,
                modelName = modelName,
                distributedMode = distributedMode,
                totalItems = totalItems,
                completedItems = completedItems,
                outputDirectory = outputDirectory,
                outputDirectoryUri = outputDirectoryUri,
                overwriteOriginal = overwriteOriginal,
                backupOriginal = backupOriginal,
                createdAtEpochMs = createdAtEpochMs,
                updatedAtEpochMs = System.currentTimeMillis(),
                lastErrorMessage = lastErrorMessage,
                pendingItems = orderedPendingItems,
                failureDetails = orderedFailures
            )
        )
    }

    private fun orderedFailureDetails(failureDetails: Map<String, ProcessingFailureDetail>): List<ProcessingFailureDetail> {
        if (failureDetails.isEmpty()) {
            return emptyList()
        }
        return failureDetails.values
            .sortedWith(compareBy({ it.displayName.lowercase() }, { it.uri }))
    }

    private fun failureNamesFromOrdered(failureDetails: List<ProcessingFailureDetail>): List<String> {
        if (failureDetails.isEmpty()) {
            return emptyList()
        }
        return failureDetails.map { detail ->
            detail.displayName.ifBlank { detail.uri }
        }
    }

    private fun modelBasePathFor(modelInfo: ModelInfo): String {
        return if (modelInfo.paramPath.endsWith(PARAM_FILE_SUFFIX, ignoreCase = true)) {
            modelInfo.paramPath.dropLast(PARAM_FILE_SUFFIX.length)
        } else {
            modelInfo.paramPath
        }
    }

    private fun progressOf(completedItems: Int, totalItems: Int): Float {
        if (totalItems <= 0) {
            return 0.0f
        }
        return (completedItems.toFloat() / totalItems.toFloat()).coerceIn(0.0f, 1.0f)
    }

    private fun updateDeviceThroughputSample(
        distributedRemoteEnabled: Boolean,
        elapsedMs: Long,
        inputWidth: Int,
        inputHeight: Int
    ) {
        val sample = estimateTilesPerSecond(
            elapsedMs = elapsedMs,
            inputWidth = inputWidth,
            inputHeight = inputHeight,
            tileSize = DISTRIBUTED_TILE_SIZE
        )
        if (sample <= 0.0f) {
            return
        }

        if (distributedRemoteEnabled) {
            val remoteDeviceId = coordinatorClient?.activeDeviceId() ?: return
            deviceRepository?.recordThroughputSample(remoteDeviceId, sample)
        } else {
            deviceRepository?.recordLocalThroughputSample(sample)
        }
    }

    private fun estimateTilesPerSecond(
        elapsedMs: Long,
        inputWidth: Int,
        inputHeight: Int,
        tileSize: Int
    ): Float {
        if (elapsedMs <= 0L || inputWidth <= 0 || inputHeight <= 0 || tileSize <= 0) {
            return 0.0f
        }
        val tilesX = (inputWidth + tileSize - 1) / tileSize
        val tilesY = (inputHeight + tileSize - 1) / tileSize
        val totalTiles = (tilesX * tilesY).coerceAtLeast(1)
        return totalTiles * 1000.0f / elapsedMs.toFloat()
    }

    private fun resolveProcessorLabel(
        explicitProcessor: String?,
        distributedRemoteEnabled: Boolean
    ): String {
        val normalizedExplicitProcessor = explicitProcessor?.trim().orEmpty()
        if (normalizedExplicitProcessor.isNotBlank()) {
            return normalizedExplicitProcessor
        }
        if (!distributedRemoteEnabled) {
            return "本机"
        }
        return coordinatorClient?.activeDeviceId().orEmpty().ifBlank { "远端节点" }
    }

    override fun onCleared() {
        inputCountEstimator.cancel()
        super.onCleared()
    }

    companion object {
        private const val PARAM_FILE_SUFFIX = ".param"
        private const val DISTRIBUTED_TILE_SIZE = 256
        private const val DISTRIBUTED_TILE_OVERLAP = 16
        private const val MAX_RECENT_ATTRIBUTIONS = 8

        fun factory(container: AppContainer): ViewModelProvider.Factory {
            return object : ViewModelProvider.Factory {
                @Suppress("UNCHECKED_CAST")
                override fun <T : ViewModel> create(modelClass: Class<T>): T {
                    return ProcessingViewModel(
                        logRepository = container.logRepository,
                        modelRepository = container.modelRepository,
                        inputSourceRepository = container.inputSourceRepository,
                        batchOutputRepository = container.batchOutputRepository,
                        checkpointRepository = container.processingCheckpointRepository,
                        queueRepository = container.processingQueueRepository,
                        nativeImageProcessingRepository = container.nativeImageProcessingRepository,
                        coordinatorClient = container.coordinatorClient,
                        deviceRepository = container.deviceRepository
                    ) as T
                }
            }
        }
    }
}
