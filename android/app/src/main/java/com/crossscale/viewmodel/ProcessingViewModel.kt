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
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.ProcessingQueueSnapshot
import com.crossscale.data.repository.ProcessingQueueStore
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.data.model.ProcessingTask
import com.crossscale.data.repository.BatchOutputDirectory
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingFailureDetail
import com.crossscale.data.repository.ProcessingPendingItem
import com.crossscale.data.repository.ResolvedImageInput
import com.crossscale.data.repository.canRetryFailedOnly
import com.crossscale.data.repository.failedPendingCount
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

enum class ProcessingQueueItemStatus {
    PENDING,
    RUNNING,
    FAILED
}

data class ProcessingQueueItemSummary(
    val id: String,
    val source: String,
    val modelName: String,
    val distributedMode: Boolean,
    val retryFailedOnly: Boolean = false,
    val outputDirectoryUri: String? = null,
    val createdAtEpochMs: Long,
    val expectedItems: Int? = null,
    val status: ProcessingQueueItemStatus = ProcessingQueueItemStatus.PENDING,
    val attempts: Int = 0,
    val lastErrorMessage: String? = null
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

private data class ProcessingRunSetup(
    val runImages: List<ResolvedImageInput>,
    val pendingImages: List<ResolvedImageInput>,
    val completedItems: Int,
    val totalItems: Int,
    val outputDirectory: BatchOutputDirectory,
    val failureDetails: MutableMap<String, ProcessingFailureDetail>
)

class ProcessingViewModel(
    private val logRepository: LogRepository,
    private val modelRepository: ModelProvider,
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val queueRepository: ProcessingQueueStore,
    private val nativeImageProcessingRepository: ImageProcessingStore,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
    private val defaultDispatcher: CoroutineDispatcher = Dispatchers.Default
) : ViewModel() {
    private val _uiState = MutableStateFlow(ProcessingUiState())
    val uiState: StateFlow<ProcessingUiState> = _uiState.asStateFlow()

    private var processingJob: Job? = null
    private var previewCountJob: Job? = null
    private var previewRequestId: Long = 0L
    private var activeQueueItemId: String? = null

    init {
        restorePersistedQueue()

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
        outputDirectoryUri: String? = null
    ) {
        pauseBatchQueueForManualRun()
        startProcessingInternal(
            imagePath = imagePath,
            modelName = modelName,
            distributedMode = distributedMode,
            outputDirectoryUri = outputDirectoryUri,
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
        outputDirectoryUri: String?
    ) {
        val normalizedImagePath = imagePath.trim()
        val normalizedModelName = modelName.trim()
        if (normalizedImagePath.isBlank()) {
            logRepository.warning("ProcessingViewModel", "Cannot enqueue batch: no input source")
            return
        }
        if (normalizedModelName.isBlank()) {
            logRepository.warning("ProcessingViewModel", "Cannot enqueue batch: no model selected")
            return
        }

        val queueItem = ProcessingQueueItemSummary(
            id = UUID.randomUUID().toString(),
            source = normalizedImagePath,
            modelName = normalizedModelName,
            distributedMode = distributedMode,
            outputDirectoryUri = outputDirectoryUri?.ifBlank { null },
            createdAtEpochMs = System.currentTimeMillis()
        )
        _uiState.update { state ->
            state.copy(queueItems = state.queueItems + queueItem)
        }
        logRepository.info(
            "ProcessingViewModel",
            "Enqueued batch ${queueItem.id.take(8)} source=$normalizedImagePath model=$normalizedModelName"
        )

        viewModelScope.launch {
            val expectedItems = withContext(ioDispatcher) {
                inputSourceRepository.resolveImageInputs(source = normalizedImagePath).size
            }
            _uiState.update { state ->
                state.copy(
                    queueItems = state.queueItems.map { item ->
                        if (item.id == queueItem.id) {
                            item.copy(expectedItems = expectedItems)
                        } else {
                            item
                        }
                    }
                )
            }
        }
    }

    fun startBatchQueue() {
        val hasPending = _uiState.value.queueItems.any { item ->
            item.status == ProcessingQueueItemStatus.PENDING
        }
        if (!hasPending) {
            logRepository.warning("ProcessingViewModel", "No pending batch in queue")
            return
        }
        _uiState.update { state ->
            state.copy(isQueueRunning = true)
        }
        runNextQueuedBatchIfNeeded()
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
                    if (item.id == normalizedBatchId && item.status != ProcessingQueueItemStatus.RUNNING) {
                        updated = true
                        item.copy(
                            status = ProcessingQueueItemStatus.PENDING,
                            retryFailedOnly = false,
                            lastErrorMessage = null
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
        if (queueItem.status == ProcessingQueueItemStatus.RUNNING) {
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
                    if (item.id == normalizedBatchId && item.status != ProcessingQueueItemStatus.RUNNING) {
                        updated = true
                        item.copy(
                            status = ProcessingQueueItemStatus.PENDING,
                            retryFailedOnly = true,
                            lastErrorMessage = null
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
                    item.id == runningBatchId && item.status == ProcessingQueueItemStatus.RUNNING
                }
            }
            state.copy(
                queueItems = remaining,
                isQueueRunning = false
            )
        }
        logRepository.info("ProcessingViewModel", "Cleared queued batches")
    }

    private fun restorePersistedQueue() {
        val restoredSnapshot = queueRepository.loadQueue()
        if (restoredSnapshot.items.isEmpty() && !restoredSnapshot.isQueueRunning) {
            return
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

    fun refreshExpectedTotal(imagePath: String?) {
        val normalizedImagePath = imagePath?.trim().orEmpty()
        previewRequestId += 1
        val requestId = previewRequestId
        previewCountJob?.cancel()

        if (normalizedImagePath.isBlank()) {
            _uiState.update { state ->
                state.copy(
                    expectedTotalItems = null,
                    isEstimatingInputCount = false
                )
            }
            return
        }

        _uiState.update { state ->
            state.copy(isEstimatingInputCount = true)
        }

        previewCountJob = viewModelScope.launch {
            val expectedTotal = withContext(ioDispatcher) {
                inputSourceRepository.resolveImageInputs(source = normalizedImagePath).size
            }
            if (!isActive || requestId != previewRequestId) {
                return@launch
            }
            _uiState.update { state ->
                state.copy(
                    expectedTotalItems = expectedTotal,
                    isEstimatingInputCount = false
                )
            }
        }
    }

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
            resumeCheckpoint = checkpoint,
            retryFailedOnly = false,
            resumePendingUris = null,
            queueItemId = null
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
                resumeCheckpoint = checkpoint,
                retryFailedOnly = true,
                resumePendingUris = null,
                queueItemId = null
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
            outputDirectoryUri = task.outputDirectoryUri
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
            resumeCheckpoint = checkpoint,
            retryFailedOnly = false,
            resumePendingUris = setOf(normalizedUri),
            queueItemId = null
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
            resumeCheckpoint = checkpoint,
            retryFailedOnly = true,
            resumePendingUris = setOf(normalizedUri),
            queueItemId = null
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

    private fun startProcessingInternal(
        imagePath: String,
        modelName: String,
        distributedMode: Boolean,
        outputDirectoryUri: String?,
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
                        distributedMode = distributedMode,
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
            _uiState.update { state ->
                state.copy(
                    task = ProcessingTask(
                        id = taskId,
                        imagePath = imagePath,
                        modelName = modelName,
                        distributedMode = distributedMode,
                        outputDirectory = outputDirectory.displayPath,
                        outputDirectoryUri = outputDirectory.outputDirectoryUri,
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
            distributedMode = distributedMode,
            totalItems = totalItems,
            completedItems = completedCount,
            outputDirectory = outputDirectory.displayPath,
            outputDirectoryUri = outputDirectory.outputDirectoryUri,
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
                    distributedMode = distributedMode,
                    outputDirectory = outputDirectory.displayPath,
                    outputDirectoryUri = outputDirectory.outputDirectoryUri,
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
            var processingSession: ImageProcessingSession? = null

            try {
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
                        distributedMode = distributedMode,
                        totalItems = totalItems,
                        completedItems = successfulCount,
                        outputDirectory = outputDirectory.displayPath,
                        outputDirectoryUri = outputDirectory.outputDirectoryUri,
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
                        session.process(
                            input = item,
                            outputFile = outputFile
                        )
                    }
                    val elapsedMs = System.currentTimeMillis() - startedAt
                    val itemUri = item.uri.toString()

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
                            logRepository.info(
                                "ProcessingViewModel",
                                "Processed ${item.displayName} -> $destination (${elapsedMs}ms, done=$successfulCount/$totalItems, pending=${pendingByUri.size})"
                            )
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
                        distributedMode = distributedMode,
                        totalItems = totalItems,
                        completedItems = successfulCount,
                        outputDirectory = outputDirectory.displayPath,
                        outputDirectoryUri = outputDirectory.outputDirectoryUri,
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
                                progress = progressOf(successfulCount, totalItems),
                                currentItemName = item.displayName,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = failureNamesFromOrdered(orderedFailures),
                                failureDetails = orderedFailures,
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
                    distributedMode = distributedMode,
                    totalItems = totalItems,
                    completedItems = successfulCount,
                    outputDirectory = outputDirectory.displayPath,
                    outputDirectoryUri = outputDirectory.outputDirectoryUri,
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
                    distributedMode = distributedMode,
                    totalItems = totalItems,
                    completedItems = successfulCount,
                    outputDirectory = outputDirectory.displayPath,
                    outputDirectoryUri = outputDirectory.outputDirectoryUri,
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

    private fun pauseBatchQueueForManualRun() {
        if (!_uiState.value.isQueueRunning) {
            return
        }
        _uiState.update { state ->
            state.copy(isQueueRunning = false)
        }
        logRepository.info("ProcessingViewModel", "Paused batch queue for manual run")
    }

    private fun runNextQueuedBatchIfNeeded() {
        if (!_uiState.value.isQueueRunning) {
            return
        }
        if (processingJob?.isActive == true) {
            return
        }

        val nextBatch = _uiState.value.queueItems.firstOrNull { item ->
            item.status == ProcessingQueueItemStatus.PENDING
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
        val updatedQueueItems = _uiState.value.queueItems.map { item ->
            if (item.id == nextBatch.id) {
                item.copy(
                    status = ProcessingQueueItemStatus.RUNNING,
                    attempts = item.attempts + 1,
                    lastErrorMessage = null
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
            distributedMode = nextBatch.distributedMode,
            outputDirectoryUri = nextBatch.outputDirectoryUri,
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
                        queueItems = state.queueItems.filterNot { item ->
                            item.id == queueItemId
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
                        queueItems = state.queueItems.map { item ->
                            if (item.id == queueItemId) {
                                item.copy(
                                    status = ProcessingQueueItemStatus.FAILED,
                                    retryFailedOnly = false,
                                    lastErrorMessage = failureMessage
                                )
                            } else {
                                item
                            }
                        },
                        isQueueRunning = false
                    )
                }
                logRepository.warning(
                    "ProcessingViewModel",
                    "Queued batch stopped: ${queueItemId.take(8)} status=$normalizedStatus message=$failureMessage"
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
        val sameOutputUri = checkpoint.outputDirectoryUri?.trim().orEmpty() ==
            queueItem.outputDirectoryUri?.trim().orEmpty()
        if (!sameSource || !sameModel || !sameMode || !sameOutputUri) {
            return null
        }
        return checkpoint
    }

    private fun buildRunSetup(
        imagePath: String,
        modelName: String,
        requestedOutputDirectoryUri: String?,
        resumeCheckpoint: ProcessingCheckpoint?,
        retryFailedOnly: Boolean,
        resumePendingUris: Set<String>?
    ): ProcessingRunSetup? {
        val outputDirectory = batchOutputRepository.prepareOutputDirectory(
            source = imagePath,
            modelName = modelName,
            preferredTreeUri = requestedOutputDirectoryUri,
            resumeOutputDirectory = resumeCheckpoint?.outputDirectory,
            resumeOutputDirectoryUri = resumeCheckpoint?.outputDirectoryUri
        )
        if (!requestedOutputDirectoryUri.isNullOrBlank() && outputDirectory.outputDirectoryUri.isNullOrBlank()) {
            logRepository.warning(
                "ProcessingViewModel",
                "Unable to use selected output folder, fallback to app internal directory: ${outputDirectory.displayPath}"
            )
        }

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
                        runImages = emptyList(),
                        pendingImages = emptyList(),
                        completedItems = totalItems,
                        totalItems = totalItems,
                        outputDirectory = outputDirectory,
                        failureDetails = mutableMapOf()
                    )
                }

                return ProcessingRunSetup(
                    runImages = runImages,
                    pendingImages = pendingImages,
                    completedItems = completedItems.coerceAtMost(totalItems),
                    totalItems = totalItems,
                    outputDirectory = outputDirectory,
                    failureDetails = failureDetails
                )
            }
        }

        val inputImages = inputSourceRepository.resolveImageInputs(source = imagePath)
        if (inputImages.isEmpty()) {
            return null
        }

        val pendingImages = inputImages.filterNot { item ->
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

    private fun failureNames(failureDetails: Map<String, ProcessingFailureDetail>): List<String> {
        return failureNamesFromOrdered(orderedFailureDetails(failureDetails))
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

    private fun ProcessingUiState.toQueueSnapshot(): ProcessingQueueSnapshot {
        return ProcessingQueueSnapshot(
            items = queueItems.map { queueItem ->
                queueItem.toPersistedQueueItem()
            },
            isQueueRunning = isQueueRunning
        )
    }

    private fun ProcessingQueueItemSummary.toPersistedQueueItem(): PersistedQueueItem {
        return PersistedQueueItem(
            id = id,
            source = source,
            modelName = modelName,
            distributedMode = distributedMode,
            retryFailedOnly = retryFailedOnly,
            outputDirectoryUri = outputDirectoryUri,
            createdAtEpochMs = createdAtEpochMs,
            expectedItems = expectedItems,
            status = status.toPersistedStatus(),
            attempts = attempts,
            lastErrorMessage = lastErrorMessage
        )
    }

    private fun PersistedQueueItem.toUiQueueItem(): ProcessingQueueItemSummary {
        return ProcessingQueueItemSummary(
            id = id,
            source = source,
            modelName = modelName,
            distributedMode = distributedMode,
            retryFailedOnly = retryFailedOnly,
            outputDirectoryUri = outputDirectoryUri,
            createdAtEpochMs = createdAtEpochMs,
            expectedItems = expectedItems,
            status = status.toUiStatus(),
            attempts = attempts,
            lastErrorMessage = lastErrorMessage
        )
    }

    private fun ProcessingQueueItemStatus.toPersistedStatus(): PersistedQueueItemStatus {
        return when (this) {
            ProcessingQueueItemStatus.PENDING -> PersistedQueueItemStatus.PENDING
            ProcessingQueueItemStatus.RUNNING -> PersistedQueueItemStatus.RUNNING
            ProcessingQueueItemStatus.FAILED -> PersistedQueueItemStatus.FAILED
        }
    }

    private fun PersistedQueueItemStatus.toUiStatus(): ProcessingQueueItemStatus {
        return when (this) {
            PersistedQueueItemStatus.PENDING -> ProcessingQueueItemStatus.PENDING
            PersistedQueueItemStatus.RUNNING -> ProcessingQueueItemStatus.RUNNING
            PersistedQueueItemStatus.FAILED -> ProcessingQueueItemStatus.FAILED
        }
    }

    private fun ProcessingCheckpoint.toUiSummary(): ProcessingCheckpointSummary {
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
            pendingCount <= 0 && failedCount <= 0 -> "已完成"
            failedCount > 0 -> "可继续（含失败项）"
            else -> "可继续"
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

    override fun onCleared() {
        previewCountJob?.cancel()
        super.onCleared()
    }

    companion object {
        private const val PARAM_FILE_SUFFIX = ".param"

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
                        nativeImageProcessingRepository = container.nativeImageProcessingRepository
                    ) as T
                }
            }
        }
    }
}
