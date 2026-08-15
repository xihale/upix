package com.crossscale.viewmodel

import androidx.compose.runtime.Stable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.crossscale.AppContainer
import com.crossscale.data.repository.BatchOutputStore
import com.crossscale.data.repository.ImageProcessingStore
import com.crossscale.data.repository.InputSourceResolver
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.ProcessingQueueStore
import com.crossscale.data.repository.canRetryFailedOnly
import com.crossscale.network.CoordinatorClient
import com.crossscale.util.StringProvider
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// Observable state lives in StateFlow properties; the class itself never
// mutates through public API, so it is safe to mark @Stable.
@Stable
class ProcessingViewModel(
    private val logRepository: LogRepository,
    private val modelRepository: ModelProvider,
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val queueRepository: ProcessingQueueStore,
    private val nativeImageProcessingRepository: ImageProcessingStore,
    private val coordinatorClient: CoordinatorClient? = null,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
    private val defaultDispatcher: CoroutineDispatcher = Dispatchers.Default,
    private val stringProvider: StringProvider
) : ViewModel(), ProcessingRunEngineHost, ProcessingQueueControllerHost {
    private val _uiState = MutableStateFlow(ProcessingUiState())
    val uiState: StateFlow<ProcessingUiState> = _uiState.asStateFlow()

    // One-shot signal emitted when the PC becomes unreachable mid-run and
    // processing fell back to on-device for the current (and remaining) images.
    // UI shows a snackbar/banner; null when idle. Clear via clearRemoteFallbackMessage().
    private val _remoteFallbackMessage = MutableStateFlow<String?>(null)
    val remoteFallbackMessage: StateFlow<String?> = _remoteFallbackMessage.asStateFlow()

    fun clearRemoteFallbackMessage() {
        _remoteFallbackMessage.value = null
    }

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

    private val runPlanner = ProcessingRunPlanner(
        inputSourceRepository = inputSourceRepository,
        batchOutputRepository = batchOutputRepository,
        checkpointRepository = checkpointRepository,
        logRepository = logRepository,
        stringProvider = stringProvider,
        coordinatorClient = coordinatorClient
    )

    private val runEngine = ProcessingRunEngine(
        host = this,
        planner = runPlanner,
        modelRepository = modelRepository,
        nativeImageProcessingRepository = nativeImageProcessingRepository,
        batchOutputRepository = batchOutputRepository,
        checkpointRepository = checkpointRepository,
        coordinatorClient = coordinatorClient,
        logRepository = logRepository,
        defaultDispatcher = defaultDispatcher,
        stringProvider = stringProvider
    )

    private val queueController = ProcessingQueueController(
        host = this,
        inputSourceRepository = inputSourceRepository,
        checkpointRepository = checkpointRepository,
        logRepository = logRepository,
        coordinatorClient = coordinatorClient,
        scope = viewModelScope,
        ioDispatcher = ioDispatcher
    )

    private var processingJob: Job? = null
    private var activeQueueItemId: String? = null

    init {
        restorePersistedQueueAsync()
        restorePersistedCheckpointAsync()

        viewModelScope.launch {
            // Project the raw checkpoint into UI-facing values first so that
            // data-class equality (ProcessingCheckpointSummary is a data class
            // with data-class elements) can dedupe unchanged snapshots before
            // they ever touch _uiState; conflate drops intermediate values if
            // the collector falls behind.
            checkpointRepository.checkpoint
                .map { checkpoint ->
                    CheckpointUiProjection(
                        canResume = checkpoint != null,
                        summary = checkpoint?.toUiSummary(stringProvider),
                        canRetryFailedOnly = checkpoint?.canRetryFailedOnly() == true
                    )
                }
                .distinctUntilChanged()
                .conflate()
                .collect { projection ->
                    _uiState.update { state ->
                        state.copy(
                            canResume = projection.canResume,
                            checkpointSummary = projection.summary,
                            canRetryFailedOnly = projection.canRetryFailedOnly
                        )
                    }
                }
        }

        viewModelScope.launch {
            uiState
                .map { state -> state.toQueueSnapshot() }
                .distinctUntilChanged()
                .conflate()
                .collectLatest { snapshot ->
                    // Runs on the injected IO dispatcher (test dispatcher in
                    // unit tests, Dispatchers.IO in production); inside the
                    // repository the serialization + write are additionally
                    // wrapped in withContext(Dispatchers.IO). collectLatest
                    // cancels an in-flight stale save so only the newest
                    // snapshot hits disk.
                    withContext(ioDispatcher) {
                        queueRepository.saveQueue(snapshot)
                    }
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
        queueController.pauseBatchQueueForManualRun()
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
    ): String? = queueController.enqueueBatch(
        imagePath = imagePath,
        modelName = modelName,
        distributedMode = distributedMode,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        processNestedFolders = processNestedFolders
    )

    fun startBatchQueue(preferredBatchId: String? = null) = queueController.startBatchQueue(preferredBatchId)

    fun syncPendingQueueDefaults(
        distributedMode: Boolean,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean
    ) = queueController.syncPendingQueueDefaults(
        distributedMode = distributedMode,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        processNestedFolders = processNestedFolders
    )

    fun pauseBatchQueue() = queueController.pauseBatchQueue()

    fun retryQueuedBatch(batchId: String) = queueController.retryQueuedBatch(batchId)

    fun retryQueuedBatchFailedOnly(batchId: String) = queueController.retryQueuedBatchFailedOnly(batchId)

    fun removeQueuedBatch(batchId: String) = queueController.removeQueuedBatch(batchId)

    fun sourceForQueuedBatch(batchId: String): String? = queueController.sourceForQueuedBatch(batchId)

    fun hasQueuedBatchForSource(source: String): Boolean = queueController.hasQueuedBatchForSource(source)

    fun queuedSources(): Set<String> = queueController.queuedSources()

    fun removeQueuedBatchesBySource(source: String): Int = queueController.removeQueuedBatchesBySource(source)

    fun clearBatchQueue() = queueController.clearBatchQueue()

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
        queueController.pauseBatchQueueForManualRun()
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
        queueController.pauseBatchQueueForManualRun()
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
        queueController.pauseBatchQueueForManualRun()
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
        queueController.pauseBatchQueueForManualRun()
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

    override fun startProcessingInternal(
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
    ): Boolean = runEngine.startProcessingInternal(
        imagePath = imagePath,
        modelName = modelName,
        distributedMode = distributedMode,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        processNestedFolders = processNestedFolders,
        resumeCheckpoint = resumeCheckpoint,
        retryFailedOnly = retryFailedOnly,
        resumePendingUris = resumePendingUris,
        queueItemId = queueItemId
    )

    // ---- ProcessingRunEngineHost ----
    // The ViewModel keeps owning _uiState, processingJob, activeQueueItemId and
    // the queue chain; the run engine touches them only through these hooks.

    override fun updateUiState(transform: (ProcessingUiState) -> ProcessingUiState) {
        _uiState.update(transform)
    }

    override fun currentUiState(): ProcessingUiState = _uiState.value

    override fun isProcessingJobActive(): Boolean = processingJob?.isActive == true

    override fun currentActiveQueueItemId(): String? = activeQueueItemId

    override fun emitRemoteFallbackMessage(message: String) {
        _remoteFallbackMessage.value = message
    }

    override fun launch(queueItemId: String?, block: suspend CoroutineScope.() -> Unit) {
        processingJob = viewModelScope.launch(block = block)
        activeQueueItemId = queueItemId
    }

    override fun onRunFinished() {
        val completedQueueItemId = activeQueueItemId
        processingJob = null
        activeQueueItemId = null
        if (completedQueueItemId != null) {
            queueController.handleQueuedRunFinished(
                queueItemId = completedQueueItemId,
                status = _uiState.value.task?.status,
                lastErrorMessage = _uiState.value.task?.lastErrorMessage
            )
        }
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

    fun outputUriForQueuedBatch(batchId: String): String? = queueController.outputUriForQueuedBatch(batchId)

    override fun onCleared() {
        inputCountEstimator.cancel()
        super.onCleared()
    }

    companion object {
        fun factory(container: AppContainer): ViewModelProvider.Factory = viewModelFactory {
            initializer {
                ProcessingViewModel(
                    logRepository = container.logRepository,
                    modelRepository = container.modelRepository,
                    inputSourceRepository = container.inputSourceRepository,
                    batchOutputRepository = container.batchOutputRepository,
                    checkpointRepository = container.processingCheckpointRepository,
                    queueRepository = container.processingQueueRepository,
                    nativeImageProcessingRepository = container.nativeImageProcessingRepository,
                    coordinatorClient = container.coordinatorClient,
                    stringProvider = container.stringProvider
                )
            }
        }
    }
}

// Dedup key for the checkpoint -> UI projection: data-class equality over the
// summary (itself a data class of data-class elements) makes
// distinctUntilChanged effective even when the repository emits fresh
// ProcessingCheckpoint instances with identical UI content.
private data class CheckpointUiProjection(
    val canResume: Boolean,
    val summary: ProcessingCheckpointSummary?,
    val canRetryFailedOnly: Boolean
)
