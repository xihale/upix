package io.upix.viewmodel

import androidx.compose.runtime.Stable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import io.upix.AppContainer
import io.upix.data.model.ProcessingStatus
import io.upix.data.repository.BatchOutputStore
import io.upix.data.repository.ImageProcessingStore
import io.upix.data.repository.InputSourceResolver
import io.upix.data.repository.LogRepository
import io.upix.data.repository.ModelProvider
import io.upix.data.repository.ProcessingCheckpoint
import io.upix.data.repository.ProcessingCheckpointStore
import io.upix.data.repository.ProcessingQueueStore
import io.upix.data.repository.ResolvedImageInput
import io.upix.data.repository.SrMarkerInfo
import io.upix.data.repository.canRetryFailedOnly
import io.upix.data.repository.probeSrMarker
import io.upix.network.CoordinatorClient
import io.upix.util.StringProvider
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// Observable state lives in StateFlow properties; the class itself never
// mutates through public API, so it is safe to mark @Stable.
//
// The run engine and queue controller (both internal, constructed only here)
// operate on this ViewModel directly through the internal hooks below; they
// never hold _uiState, processingJob or activeQueueItemId themselves.
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
    private val stringProvider: StringProvider,
    private val probeSrMarker: (ResolvedImageInput) -> SrMarkerInfo?
) : ViewModel() {
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

    private val runPlanner = ProcessingRunPlanner(
        inputSourceRepository = inputSourceRepository,
        batchOutputRepository = batchOutputRepository,
        checkpointRepository = checkpointRepository,
        logRepository = logRepository,
        probeSrMarker = probeSrMarker
    )

    private val runEngine = ProcessingRunEngine(
        viewModel = this,
        planner = runPlanner,
        modelRepository = modelRepository,
        nativeImageProcessingRepository = nativeImageProcessingRepository,
        batchOutputRepository = batchOutputRepository,
        checkpointRepository = checkpointRepository,
        coordinatorClient = coordinatorClient,
        logRepository = logRepository,
        ioDispatcher = ioDispatcher,
        defaultDispatcher = defaultDispatcher,
        stringProvider = stringProvider
    )

    private val queueController = ProcessingQueueController(
        viewModel = this,
        inputSourceRepository = inputSourceRepository,
        checkpointRepository = checkpointRepository,
        logRepository = logRepository,
        coordinatorClient = coordinatorClient,
        stringProvider = stringProvider,
        scope = viewModelScope,
        ioDispatcher = ioDispatcher
    )

    private var processingJob: Job? = null
    private var activeQueueItemId: String? = null

    // run 代际令牌：cancelProcessing 只取消不清引用，旧 run 的收尾回调只在
    // “它仍是当前代际”时才清理 job/队列，避免清掉新 run 的状态或用错批次 id。
    private var runGenerationCounter = 0L
    private var activeRunGeneration = 0L

    // 启动恢复完成门闩：收集器在恢复完成前不持久化，否则“初始空队列”会先
    // 落盘并把 processing_queue.json 删掉，随后恢复读取拿到空快照（启动竞态）。
    private val queueRestoreCompleted = MutableStateFlow(false)

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
                        summary = checkpoint?.toUiSummary(stringProvider),
                        canRetryFailedOnly = checkpoint?.canRetryFailedOnly() == true
                    )
                }
                .distinctUntilChanged()
                .conflate()
                .collect { projection ->
                    _uiState.update { state ->
                        state.copy(
                            checkpointSummary = projection.summary,
                            canRetryFailedOnly = projection.canRetryFailedOnly
                        )
                    }
                }
        }

        viewModelScope.launch {
            // 先等恢复完成，再开始持久化（修复启动竞态）。
            queueRestoreCompleted
                .filter { it }
                .take(1)
                .collectLatest {
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
                                queueRepository.persistQueue(snapshot)
                            }
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
        processNestedFolders: Boolean = true,
        skipSrMarkedInputs: Boolean = true
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
            skipSrMarkedInputs = skipSrMarkedInputs,
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
        processNestedFolders: Boolean = true,
        skipSrMarkedInputs: Boolean = true
    ): String? = queueController.enqueueBatch(
        imagePath = imagePath,
        modelName = modelName,
        distributedMode = distributedMode,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        processNestedFolders = processNestedFolders,
        skipSrMarkedInputs = skipSrMarkedInputs
    )

    fun startBatchQueue(preferredBatchId: String? = null) = queueController.startBatchQueue(preferredBatchId)

    fun syncPendingQueueDefaults(
        distributedMode: Boolean,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean,
        skipSrMarkedInputs: Boolean
    ) = queueController.syncPendingQueueDefaults(
        distributedMode = distributedMode,
        outputDirectoryUri = outputDirectoryUri,
        overwriteOriginal = overwriteOriginal,
        backupOriginal = backupOriginal,
        processNestedFolders = processNestedFolders,
        skipSrMarkedInputs = skipSrMarkedInputs
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
            if (restoredSnapshot.items.isNotEmpty() || restoredSnapshot.isQueueRunning) {
                _uiState.update { state ->
                    // 合并而非整体覆盖：恢复完成前新入队的项必须存活。
                    val restoredItems = restoredSnapshot.items.map { item -> item.toUiQueueItem() }
                    val existingIds = state.queueItems.mapTo(hashSetOf()) { item -> item.id }
                    state.copy(
                        queueItems = state.queueItems + restoredItems.filterNot { item ->
                            item.id in existingIds
                        },
                        isQueueRunning = state.isQueueRunning || restoredSnapshot.isQueueRunning
                    )
                }
                logRepository.info(
                    "ProcessingViewModel",
                    "Restored ${restoredSnapshot.items.size} queued batch(es)"
                )
            }
            queueRestoreCompleted.value = true
        }
    }

    private fun restorePersistedCheckpointAsync() {
        viewModelScope.launch {
            withContext(ioDispatcher) {
                checkpointRepository.loadCheckpoint()
            }
        }
    }

    fun resumeLastProcessing() {
        viewModelScope.launch {
            runFromCheckpoint(retryFailedOnly = false)
        }
    }

    fun retryFailedItems() {
        if (isProcessingJobActive()) {
            return
        }
        // checkpoint 读取是磁盘 IO：入口保持非挂起，内部起协程切 IO。
        viewModelScope.launch {
            val checkpoint = withContext(ioDispatcher) {
                checkpointRepository.loadCheckpoint()
            }
            if (checkpoint != null) {
                if (!checkpoint.canRetryFailedOnly()) {
                    logRepository.warning("ProcessingViewModel", "No failed items available in checkpoint")
                    return@launch
                }
                runFromCheckpoint(retryFailedOnly = true)
                return@launch
            }
            // No checkpoint: fall back to rerunning the whole source of the
            // in-memory task.
            queueController.pauseBatchQueueForManualRun()
            val task = _uiState.value.task
            if (task == null || task.failedItems <= 0) {
                logRepository.warning("ProcessingViewModel", "No failed items available for retry")
                return@launch
            }
            startProcessing(
                imagePath = task.imagePath,
                modelName = task.modelName,
                distributedMode = task.distributedMode,
                outputDirectoryUri = task.outputDirectoryUri,
                overwriteOriginal = task.overwriteOriginal,
                backupOriginal = task.backupOriginal,
                processNestedFolders = task.processNestedFolders,
                skipSrMarkedInputs = task.skipSrMarkedInputs
            )
        }
    }

    fun resumePendingItem(uri: String) {
        val normalizedUri = uri.trim()
        if (normalizedUri.isBlank()) {
            return
        }
        viewModelScope.launch {
            runFromCheckpoint(retryFailedOnly = false, resumePendingUris = setOf(normalizedUri))
        }
    }

    fun retryFailedItem(uri: String) {
        val normalizedUri = uri.trim()
        if (normalizedUri.isBlank()) {
            return
        }
        viewModelScope.launch {
            runFromCheckpoint(retryFailedOnly = true, resumePendingUris = setOf(normalizedUri))
        }
    }

    // Shared body of the manual resume/retry entry points: refuses to start
    // while a run is active, pauses a running batch queue, loads the persisted
    // checkpoint and (re)starts it.
    private suspend fun runFromCheckpoint(
        retryFailedOnly: Boolean,
        resumePendingUris: Set<String>? = null
    ) {
        if (isProcessingJobActive()) {
            return
        }
        queueController.pauseBatchQueueForManualRun()
        val checkpoint = withContext(ioDispatcher) {
            checkpointRepository.loadCheckpoint()
        }
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
            skipSrMarkedInputs = checkpoint.skipSrMarkedInputs,
            resumeCheckpoint = checkpoint,
            retryFailedOnly = retryFailedOnly,
            resumePendingUris = resumePendingUris,
            queueItemId = checkpoint.taskId
        )
    }

    fun discardCheckpoint() {
        if (isProcessingJobActive()) {
            logRepository.warning("ProcessingViewModel", "Cannot discard checkpoint while processing is running")
            return
        }
        // 读/删 checkpoint 都是磁盘 IO：内部起协程，公开 API 保持非挂起。
        viewModelScope.launch {
            if (checkpointRepository.loadCheckpoint() == null) {
                logRepository.info("ProcessingViewModel", "No checkpoint to discard")
                return@launch
            }
            checkpointRepository.clearCheckpoint()
            logRepository.info("ProcessingViewModel", "Discarded processing checkpoint")
        }
    }

    suspend fun hasBackupFor(imagePath: String, nested: Boolean): Boolean = withContext(ioDispatcher) {
        inputSourceRepository.resolveImageInputs(source = imagePath, nested = nested)
            .any { input -> batchOutputRepository.hasBackup(input) }
    }

    fun restoreBackup(imagePath: String, nested: Boolean, onComplete: (Int, Int) -> Unit) {
        viewModelScope.launch {
            var restored = 0
            var failed = 0
            withContext(ioDispatcher) {
                val inputs = inputSourceRepository.resolveImageInputs(source = imagePath, nested = nested)
                for (input in inputs) {
                    if (batchOutputRepository.hasBackup(input)) {
                        batchOutputRepository.restoreBackup(input).fold(
                            onSuccess = { restored++ },
                            onFailure = {
                                failed++
                                logRepository.error("ProcessingViewModel", "Failed to restore backup for ${input.displayName}: ${it.message}")
                            }
                        )
                    }
                }
            }
            if (restored > 0 || failed > 0) {
                logRepository.info("ProcessingViewModel", "Restore completed. Restored: $restored, Failed: $failed")
            }
            onComplete(restored, failed)
        }
    }

    internal fun startProcessingInternal(
        imagePath: String,
        modelName: String,
        distributedMode: Boolean,
        outputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean,
        skipSrMarkedInputs: Boolean,
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
        skipSrMarkedInputs = skipSrMarkedInputs,
        resumeCheckpoint = resumeCheckpoint,
        retryFailedOnly = retryFailedOnly,
        resumePendingUris = resumePendingUris,
        queueItemId = queueItemId
    )

    // ---- Internal hooks for ProcessingRunEngine / ProcessingQueueController ----
    // The ViewModel keeps owning _uiState, processingJob, activeQueueItemId and
    // the queue chain; the run engine and queue controller touch them only
    // through these hooks.

    internal fun updateUiState(transform: (ProcessingUiState) -> ProcessingUiState) {
        _uiState.update(transform)
    }

    internal fun currentUiState(): ProcessingUiState = _uiState.value

    // 一个 run 在“已取消但尚未收尾完成”期间也算活跃：新的 run 会被拒绝启动，
    // 直到旧 run 的 finally（含 NonCancellable 的 checkpoint 落盘）跑完。
    // 选择“视为忙碌”而不是“取消并 join”：后者需要把入口变挂起函数，
    // 与非挂起的公开 API 冲突，且实现更复杂。
    internal fun isProcessingJobActive(): Boolean = processingJob?.isCompleted == false

    internal fun currentActiveQueueItemId(): String? = activeQueueItemId

    internal fun emitRemoteFallbackMessage(message: String) {
        _remoteFallbackMessage.value = message
    }

    internal fun launchRun(queueItemId: String?, block: suspend CoroutineScope.() -> Unit) {
        runGenerationCounter += 1
        val generation = runGenerationCounter
        activeRunGeneration = generation
        activeQueueItemId = queueItemId
        processingJob = viewModelScope.launch {
            try {
                block()
            } finally {
                // 收尾统一在这里：引擎的 finally 不再直接调用 onRunFinished。
                onRunFinished(generation)
            }
        }
    }

    internal suspend fun onRunFinished(finishedRunGeneration: Long) {
        if (finishedRunGeneration != activeRunGeneration) {
            // 旧 run 被新 run 取代后才收尾：不动当前 run 的 job/活跃批次。
            return
        }
        val completedQueueItemId = activeQueueItemId
        processingJob = null
        activeQueueItemId = null
        if (completedQueueItemId != null) {
            // run 可能是被取消的：NonCancellable 保证队列收尾（含链式推进的
            // 磁盘读取）不会在取消的协程里被 CancellationException 打断。
            withContext(NonCancellable) {
                queueController.handleQueuedRunFinished(
                    queueItemId = completedQueueItemId,
                    status = _uiState.value.task?.status,
                    lastErrorMessage = _uiState.value.task?.lastErrorMessage
                )
            }
        }
    }

    fun cancelProcessing() {
        // 只取消不清引用：job 由该 run 自己的 finally（校验代际后）清理。
        // 取消期间 isProcessingJobActive() 仍为 true，新 run 不会与收尾交错。
        processingJob?.cancel()
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
                    stringProvider = container.stringProvider,
                    probeSrMarker = { input ->
                        probeSrMarker(
                            resolver = container.appContext.contentResolver,
                            uri = input.uri,
                            displayName = input.displayName
                        ) { message ->
                            container.logRepository.warning("ProcessingViewModel", message)
                        }
                    }
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
    val summary: ProcessingCheckpointSummary?,
    val canRetryFailedOnly: Boolean
)
