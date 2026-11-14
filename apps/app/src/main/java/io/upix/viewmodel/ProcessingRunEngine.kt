package io.upix.viewmodel

import io.upix.R
import io.upix.data.model.ProcessingStatus
import io.upix.data.model.ProcessingTask
import io.upix.data.model.RemoteItemPhase
import io.upix.data.repository.BatchOutputStore
import io.upix.data.repository.ImageProcessingSession
import io.upix.data.repository.ImageProcessingStore
import io.upix.data.repository.LogRepository
import io.upix.data.repository.ModelProvider
import io.upix.data.repository.NativeProcessResult
import io.upix.data.repository.OutputImageFormat
import io.upix.data.repository.ProcessingCheckpoint
import io.upix.data.repository.ProcessingCheckpointStore
import io.upix.data.repository.ProcessingFailureDetail
import io.upix.data.repository.ResolvedImageInput
import io.upix.data.repository.outputFormatFor
import io.upix.network.CoordinatorClient
import io.upix.nativelib.NativeCapabilities
import io.upix.nativelib.PeerCapabilities
import io.upix.util.StringProvider
import java.util.LinkedHashMap
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.isActive
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext

// One queued output-commit job: the inference result is already in the
// staging file; the commit (SAF copy / MediaStore insert / filesystem move)
// runs asynchronously and its bookkeeping is applied at a drain point.
private data class QueuedCommit(
    val item: ResolvedImageInput,
    val outputFile: java.io.File,
    val elapsedMs: Long,
    val job: Deferred<Result<String>>
)

// Execution engine for a single processing run, extracted verbatim from
// ProcessingViewModel.startProcessingInternal. It validates inputs, asks the
// planner for a ProcessingRunSetup, then drives the per-item loop (remote or
// native routing with mid-run remote fallback), output commit/discard,
// checkpointing and terminal state transitions — reporting every state change
// back to the owning ProcessingViewModel.
internal class ProcessingRunEngine(    private val viewModel: ProcessingViewModel,
    private val planner: ProcessingRunPlanner,
    private val modelRepository: ModelProvider,
    private val nativeImageProcessingRepository: ImageProcessingStore,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val coordinatorClient: CoordinatorClient?,
    private val logRepository: LogRepository,
    private val ioDispatcher: CoroutineDispatcher,
    private val defaultDispatcher: CoroutineDispatcher,
    private val stringProvider: StringProvider
) {
    fun startProcessingInternal(
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
        if (viewModel.isProcessingJobActive()) {
            return false
        }

        // Single source of truth for remote-vs-native routing: the coordinator is
        // "connected" iff a paired PC endpoint is set. The user "distributed"
        // toggle no longer overrides this — if the PC is reachable we use it,
        // otherwise we process on-device. Local-device-as-worker is gone.
        val useRemote = coordinatorClient?.isConnected() == true
        val effectiveDistributedMode = useRemote
        // AVIF 可写由真实能力决定：本地看构建期可选依赖（-Pupix.avifEncoder=true），
        // 远端看配对握手学到的 PC 编码能力位（CONNECT_RESPONSE），不再无脑假设。
        val pcAvifEncode = useRemote && PeerCapabilities.avifEncode
        batchOutputRepository.setWriteAvif(pcAvifEncode || NativeCapabilities.avifEncode)
        // 跳过开关与目录扫描同规则：resume 还原 checkpoint 记录的值。
        val effectiveSkipSrMarked = resumeCheckpoint?.skipSrMarkedInputs ?: skipSrMarkedInputs

        // 快速失败路径保留在主线程：模型查表是内存操作。
        val modelInfo = modelRepository.getModelByName(modelName)
        if (modelInfo == null) {
            val errorMessage = stringProvider.getString(
                R.string.processing_error_model_not_found, modelName
            )
            logRepository.error("ProcessingViewModel", errorMessage)
            viewModel.updateUiState { state ->
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

        if (!pcAvifEncode && !NativeCapabilities.avifEncode &&
            batchOutputRepository.outputFormatSetting() == OutputImageFormat.AVIF
        ) {
            val errorMessage = stringProvider.getString(R.string.processing_error_local_avif)
            logRepository.error("ProcessingViewModel", errorMessage)
            viewModel.updateUiState { state ->
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

        val taskId = resumeCheckpoint?.taskId ?: UUID.randomUUID().toString()
        val createdAt = resumeCheckpoint?.createdAtEpochMs ?: System.currentTimeMillis()
        val modelBasePath = planner.modelBasePathFor(modelInfo)

        // 先亮出 RUNNING 占位任务：点击后立即反馈“进行中”，也让取消按钮可用。
        viewModel.updateUiState { state ->
            state.copy(
                task = ProcessingTask(
                    id = taskId,
                    imagePath = imagePath,
                    modelName = modelName,
                    distributedMode = effectiveDistributedMode,
                    overwriteOriginal = overwriteOriginal,
                    backupOriginal = backupOriginal,
                    processNestedFolders = processNestedFolders,
                    progress = 0.0f,
                    status = ProcessingStatus.RUNNING
                ),
                isProcessing = true
            )
        }

        viewModel.launchRun(queueItemId) {
            // 规划阶段（整棵目录遍历 + 逐图解码探测 + 逐图 SAF 查询 + 输出目录
            // 准备）可能很慢，移到 run 协程内用 IO 调度器执行，绝不阻塞主线程。
            val setup = withContext(ioDispatcher) {
                planner.buildRunSetup(
                    imagePath = imagePath,
                    modelName = modelName,
                    requestedOutputDirectoryUri = outputDirectoryUri,
                    overwriteOriginal = overwriteOriginal,
                    backupOriginal = backupOriginal,
                    processNestedFolders = processNestedFolders,
                    skipSrMarkedInputs = skipSrMarkedInputs,
                    resumeCheckpoint = resumeCheckpoint,
                    retryFailedOnly = retryFailedOnly,
                    resumePendingUris = resumePendingUris
                )
            }
            if (setup == null) {
                logRepository.warning("ProcessingViewModel", "Unable to prepare processing inputs: $imagePath")
                viewModel.updateUiState { state ->
                    val currentTask = state.task
                    state.copy(
                        task = (currentTask ?: ProcessingTask(
                            id = taskId,
                            imagePath = imagePath,
                            modelName = modelName,
                            distributedMode = effectiveDistributedMode,
                            progress = 0.0f,
                            status = ProcessingStatus.IDLE
                        )).copy(
                            status = ProcessingStatus.FAILED,
                            currentItemName = null,
                            lastErrorMessage = stringProvider.getString(R.string.queue_cannot_start_batch)
                        ),
                        isProcessing = false
                    )
                }
                return@launchRun
            }

            val outputDirectory = setup.outputDirectory
            val runImages = setup.runImages
            val allImages = setup.allImages
            val pendingImages = setup.pendingImages
            val totalItems = setup.totalItems
            val completedCount = setup.completedItems
            val srSkippedCount = setup.skippedItems
            val initialFailures = setup.failureDetails
            val initialFailureList = planner.orderedFailureDetails(initialFailures)
            val initialLastError = resumeCheckpoint?.lastErrorMessage

            if (pendingImages.isEmpty()) {
                if (srSkippedCount > 0) {
                    logRepository.info(
                        "ProcessingViewModel",
                        "Nothing to do: $srSkippedCount input(s) already super-resolved (marker), $totalItems total for source=$imagePath"
                    )
                }
                checkpointRepository.clearCheckpoint()
                if (!queueItemId.isNullOrBlank()) {
                    val existingOutputUri = withContext(ioDispatcher) {
                        batchOutputRepository.firstReadyOutputUri(
                            outputDirectory = outputDirectory,
                            inputs = allImages
                        )
                    }
                    if (!existingOutputUri.isNullOrBlank()) {
                        viewModel.updateUiState { state ->
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
                viewModel.updateUiState { state ->
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
                return@launchRun
            }

            viewModel.updateUiState { state ->
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
                        skipSrMarkedInputs = effectiveSkipSrMarked,
                        currentItemName = runImages.firstOrNull()?.displayName,
                        completedItems = completedCount,
                        pendingItems = pendingImages.size,
                        failedItems = initialFailures.size,
                        skippedItems = srSkippedCount,
                        failureDetails = initialFailureList,
                        totalItems = totalItems,
                        progress = planner.progressOf(completedCount, totalItems),
                        status = ProcessingStatus.RUNNING,
                        lastErrorMessage = initialLastError
                    ),
                    isProcessing = true
                )
            }
            logRepository.info(
                "ProcessingViewModel",
                "Started task $taskId with model=$modelName, total=$totalItems, completed=$completedCount, pending=${pendingImages.size}, run=${runImages.size}, skippedSr=$srSkippedCount, retryFailedOnly=$retryFailedOnly, outputDir=${outputDirectory.displayPath}"
            )

            var successfulCount = completedCount
            var lastError: String? = initialLastError
            val pendingByUri = LinkedHashMap<String, ResolvedImageInput>()
            pendingImages.forEach { pendingByUri[it.uri.toString()] = it }
            val failureDetailsByUri = initialFailures.toMutableMap()
            // Per-image routing mode, fixed for the whole run: a PC failure
            // aborts the batch with an error instead of silently falling back.
            val remoteRouting = effectiveDistributedMode
            var processingSession: ImageProcessingSession? = null
            // 远程流水线状态：remoteInFlight 是"已上传、待收结果"的任务，
            // remoteUploads 是按提交顺序排队的提前上传作业。索引 ↔ 输入
            // 的对应关系由 RemoteUploadWindow 唯一决定（PC 按提交顺序回帧，
            // 收错一张就会把 A 的成果写进 B 的文件）。深度 1 上传 N+1、深度 2
            // 再上传 N+2，与"收 N + 写盘 N + SAF 提交 N"并行，逐张周期收敛到
            // max(PC 推理, PC 编码+回传)。上传链串行（每个作业先 join 上一个），
            // 保证 wire 顺序 == 队列顺序且 DATA 帧永不交错（同一条 TCP 流上
            // 并发的两个 CLIENT_SUBMIT 会交错分块帧、颠倒 FIFO staging）。
            // remoteSendAheadError 非空 = 上传失败（发前路径不做任何会话恢复），
            // 循环下一迭代按当前图失败上报并终止批次。
            var remoteInFlight: CoordinatorClient.RemoteTask? = null
            val remoteUploads = ArrayDeque<
                kotlinx.coroutines.Deferred<Result<CoordinatorClient.RemoteTask>>>()
            var lastAheadUpload: kotlinx.coroutines.Deferred<Result<CoordinatorClient.RemoteTask>>? = null
            var remoteSendAheadError: Exception? = null

            // 提交（落盘）流水线：commit 是纯文件 IO（SAF copy/MediaStore/
            // rename），不在 GPU/网络关键路径上。远程路径把它异步入串行通道
            // （Semaphore(1)，SAF 慢时也不与推理收流抢 IO），成功计数/
            // pending 移除/checkpoint 一并延后到收割点；批次终态判定前必须
            // 全量收割，保证 checkpoint 与磁盘一致。取消时未启动的 job 随
            // scope 终止，暂存文件名是确定性的，下次 run 原地覆盖。
            val pendingCommits = mutableListOf<QueuedCommit>()
            val commitLimiter = Semaphore(1)

            // Persists a checkpoint reflecting the run's current progress. The
            // run-invariant fields are captured here; the mutable progress
            // (successfulCount / pendingByUri / failureDetailsByUri / lastError)
            // is read at invocation time, so every call site is a one-liner.
            suspend fun saveCheckpointSnapshot() {
                planner.saveCheckpointSnapshot(
                    taskId = taskId,
                    queueItemId = queueItemId,
                    imagePath = imagePath,
                    modelName = modelName,
                    distributedMode = effectiveDistributedMode,
                    totalItems = totalItems,
                    completedItems = successfulCount,
                    outputDirectory = outputDirectory.displayPath,
                    outputDirectoryUri = outputDirectory.outputDirectoryUri,
                    overwriteOriginal = outputDirectory.overwriteOriginal,
                    backupOriginal = outputDirectory.backupOriginal,
                    processNestedFolders = outputDirectory.processNestedFolders,
                    skipSrMarkedInputs = effectiveSkipSrMarked,
                    createdAtEpochMs = createdAt,
                    pendingImages = pendingByUri.values.toList(),
                    failureDetails = failureDetailsByUri,
                    lastErrorMessage = lastError
                )
            }

            // Initial checkpoint (moved into the run coroutine so the disk
            // write happens off the main thread via persistCheckpoint).
            try {
                saveCheckpointSnapshot()
            } catch (cancellationException: CancellationException) {
                throw cancellationException
            } catch (exception: Exception) {
                val errorMessage = exception.message
                    ?: stringProvider.getString(R.string.processing_error_unknown_failure)
                logRepository.error(
                    "ProcessingViewModel",
                    "Failed to persist initial task checkpoint: $errorMessage"
                )
                viewModel.updateUiState { state ->
                    val currentTask = state.task ?: return@updateUiState state
                    state.copy(
                        task = currentTask.copy(
                            status = ProcessingStatus.FAILED,
                            lastErrorMessage = errorMessage,
                            currentItemName = null
                        ),
                        isProcessing = false
                    )
                }
                return@launchRun
            }
            // Apply the bookkeeping of finished commit jobs: success bumps the
            // counter and shrinks the pending set (checkpoint + UI follow),
            // failure records the item and discards its staging file. In FIFO
            // order — the commit channel is serial, so completion order equals
            // enqueue order. waitAll=false only reaps what already finished
            // (per-round opportunity); waitAll=true suspends until every
            // queued commit is settled (PC-failure exit and terminal state).
            suspend fun drainCompletedCommits(waitAll: Boolean) {
                var changed = false
                while (pendingCommits.isNotEmpty()) {
                    val head = pendingCommits.first()
                    if (!waitAll && !head.job.isCompleted) {
                        break
                    }
                    pendingCommits.removeAt(0)
                    val commitResult = head.job.await()
                    val headUri = head.item.uri.toString()
                    if (commitResult.isSuccess) {
                        successfulCount += 1
                        pendingByUri.remove(headUri)
                        failureDetailsByUri.remove(headUri)
                        val destination = commitResult.getOrNull()
                            ?: head.outputFile.absolutePath
                        logRepository.info(
                            "ProcessingViewModel",
                            "Processed ${head.item.displayName} -> $destination (${head.elapsedMs}ms committed async, done=$successfulCount/$totalItems, pending=${pendingByUri.size})"
                        )
                        if (!queueItemId.isNullOrBlank()) {
                            val dest = destination
                            viewModel.updateUiState { state ->
                                state.copy(
                                    queueItems = state.queueItems.map { queueItem ->
                                        if (queueItem.id == queueItemId) {
                                            queueItem.copy(lastOutputUri = dest)
                                        } else {
                                            queueItem
                                        }
                                    }
                                )
                            }
                        }
                    } else {
                        val errText = commitResult.exceptionOrNull()?.message
                            ?: stringProvider.getString(R.string.processing_error_save_output)
                        lastError = errText
                        val previousAttempts =
                            failureDetailsByUri[headUri]?.attempts ?: 0
                        failureDetailsByUri[headUri] = ProcessingFailureDetail(
                            uri = headUri,
                            displayName = head.item.displayName,
                            errorMessage = errText,
                            attempts = previousAttempts + 1
                        )
                        withContext(ioDispatcher) {
                            batchOutputRepository.discardOutputFile(head.outputFile)
                        }
                        logRepository.error(
                            "ProcessingViewModel",
                            "Failed to store output for ${head.item.displayName}: $lastError (pending=${pendingByUri.size})"
                        )
                    }
                    changed = true
                }
                if (changed) {
                    saveCheckpointSnapshot()
                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                progress = planner.progressOf(successfulCount, totalItems),
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failureDetails = orderedFailures,
                                lastErrorMessage = lastError
                            )
                        )
                    }
                }
            }

            try {
                if (remoteRouting) {
                    // remoteRouting == true only when coordinatorClient != null
                    // (invariant: effectiveDistributedMode = coordinatorClient?.isConnected() == true).
                    val endpoint = coordinatorClient?.activeDeviceId().orEmpty()
                    logRepository.info(
                        "ProcessingViewModel",
                        "Remote mode: routing tasks to coordinator $endpoint"
                    )
                } else {
                    logRepository.info(
                        "ProcessingViewModel",
                        "Native mode: processing on device"
                    )
                }

                if (!remoteRouting) {
                    // openSession 里的 nativeInit/nativeLoadModel 是阻塞调用，
                    // 不响应协程取消；withContext 的 prompt cancellation 会在
                    // 返回处丢弃已赋值的 session，此时若不显式释放，native 引擎
                    // （含已加载模型/Vulkan 资源）就永久泄漏——finally 里的
                    // processingSession 还是 null。close() 自身幂等。
                    var openedSession: ImageProcessingSession? = null
                    try {
                        withContext(defaultDispatcher) {
                            openedSession = nativeImageProcessingRepository.openSession(
                                modelName = modelName,
                                modelBasePath = modelBasePath,
                                preferGpu = true
                            )
                        }
                    } catch (cancellationException: CancellationException) {
                        openedSession?.close()
                        throw cancellationException
                    }
                    processingSession = openedSession

                    if (processingSession == null) {
                        val errorMessage = stringProvider.getString(
                            R.string.processing_error_native_session, modelName
                        )
                        lastError = errorMessage
                        logRepository.error("ProcessingViewModel", errorMessage)
                        saveCheckpointSnapshot()
                        viewModel.updateUiState { state ->
                            val currentTask = state.task ?: return@updateUiState state
                            val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                            state.copy(
                                task = currentTask.copy(
                                    status = ProcessingStatus.FAILED,
                                    currentItemName = null,
                                    pendingItems = pendingByUri.size,
                                    failedItems = orderedFailures.size,
                                    failureDetails = orderedFailures,
                                    lastErrorMessage = errorMessage
                                ),
                                isProcessing = false
                            )
                        }
                        return@launchRun
                    }
                }

                // 远程模式先把第一张上传（握手/配对在这条会话上只发生一次），
                // 之后的循环里每轮：收结果 → 上传下一张 → 写盘当前张。
                if (remoteRouting) {
                    remoteInFlight = withContext(ioDispatcher) {
                        val first = runImages.first()
                        val firstOutputFile = batchOutputRepository.outputFileFor(
                            outputDirectory = outputDirectory,
                            input = first
                        )
                        val begun = coordinatorClient!!.beginRemoteTask(
                            input = first,
                            modelName = modelName,
                            tileSize = DISTRIBUTED_TILE_SIZE,
                            tileOverlap = DISTRIBUTED_TILE_OVERLAP,
                            outputFormat = outputFormatFor(firstOutputFile)
                        )
                        if (begun.isFailure) {
                            remoteSendAheadError = begun.exceptionOrNull() as? Exception
                                ?: IllegalStateException(begun.exceptionOrNull()?.message)
                            null
                        } else {
                            begun.getOrThrow()
                        }
                    }
                }

                // 流水线深度来自 PC 的 CONNECT_RESPONSE（握手/配对已随首发
                // 上传完成，PeerCapabilities 此刻已刷新）；窗口的索引↔输入
                // 对应关系在整个批次内固定。
                val remoteUploadWindow = RemoteUploadWindow(
                    aheadDepth = when {
                        PeerCapabilities.deepPipeline -> 2
                        PeerCapabilities.submitAhead -> 1
                        else -> 0
                    },
                    itemCount = runImages.size
                )

                for ((itemIndex, item) in runImages.withIndex()) {
                    if (!isActive) {
                        return@launchRun
                    }
                    // 先收割上一拍已完成的异步 commit:成功计数/进度及时推进。
                    drainCompletedCommits(waitAll = false)

                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        state.copy(
                            task = currentTask.copy(
                                currentItemName = item.displayName,
                                pendingItems = pendingByUri.size,
                                remotePhase = null,
                                remoteItemProgress = null,
                                remoteItemEtaSeconds = null,
                                progress = planner.progressOf(successfulCount, totalItems)
                            )
                        )
                    }

                    // 输出文件路径推导涉及临时目录创建，归入 IO 调度器。
                    val outputFile = withContext(ioDispatcher) {
                        batchOutputRepository.outputFileFor(outputDirectory = outputDirectory, input = item)
                    }

                    val startedAt = System.currentTimeMillis()
                    val currentSession = processingSession
                    // 远程路径是阻塞文件读 + 阻塞 JNI 网络，归 IO 调度器；
                    // 本地路径是 CPU 密集的 JNI 推理，保持在 defaultDispatcher。
                    val result = if (remoteRouting) {
                        val flight = remoteInFlight
                        if (flight == null) {
                            // send-ahead 已在上轮失败：按当前图失败上报并终止批次。
                            Result.failure<NativeProcessResult>(
                                remoteSendAheadError
                                    ?: IllegalStateException(
                                        stringProvider.getString(R.string.processing_error_remote_upload_failed)
                                    )
                            )
                        } else {
                    withContext(ioDispatcher) {
                        // 深度0（旧 PC，无发前窗口）：预取下一张原图字节，与
                        // PC 处理当前张重叠；上传在收完结果后串行进行。
                        if (remoteUploadWindow.aheadDepth == 0 && itemIndex + 1 < runImages.size) {
                            coordinatorClient!!.prefetchRemoteInput(runImages[itemIndex + 1])
                        }
                        // 补齐发前上传窗口：本轮要提前上传的索引由窗口决定
                        // （深度1 → N+1；深度2 → N+1、N+2）。每个作业先 join
                        // 上一个作业再发送，因此提交顺序 == 队列顺序，同一条
                        // 会话上不会有两个 CLIENT_SUBMIT 交错。
                        remoteUploadWindow.uploadsForRound(itemIndex).forEach { aheadIndex ->
                            val aheadOutputFile = batchOutputRepository.outputFileFor(
                                outputDirectory = outputDirectory,
                                input = runImages[aheadIndex]
                            )
                            val previousUpload = lastAheadUpload
                            val uploadJob = async(ioDispatcher) {
                                previousUpload?.join()
                                coordinatorClient!!.beginRemoteTaskAhead(
                                    input = runImages[aheadIndex],
                                    modelName = modelName,
                                    tileSize = DISTRIBUTED_TILE_SIZE,
                                    tileOverlap = DISTRIBUTED_TILE_OVERLAP,
                                    outputFormat = outputFormatFor(aheadOutputFile)
                                )
                            }
                            lastAheadUpload = uploadJob
                            remoteUploads.addLast(uploadJob)
                        }
                        val fetchResult = flight.awaitResult { stage, percent, eta ->
                            viewModel.updateUiState { state ->
                                val currentTask = state.task
                                    ?: return@updateUiState state
                                val phase: RemoteItemPhase
                                val itemFraction: Float
                                if (stage == 1) {
                                    phase = RemoteItemPhase.ENCODING
                                    itemFraction = 1.0f
                                } else {
                                    phase = RemoteItemPhase.INFERENCE
                                    itemFraction = (percent / 100.0f)
                                        .coerceIn(0.0f, 1.0f)
                                }
                                state.copy(
                                    task = currentTask.copy(
                                        remotePhase = phase,
                                        remoteItemProgress = itemFraction,
                                        remoteItemEtaSeconds =
                                            if (stage == 0 && eta > 0) eta else null,
                                        progress = combinedRemoteProgress(
                                            successfulCount, totalItems, itemFraction
                                        )
                                    )
                                )
                            }
                        }
                        // 收完结果 N：窗口推进到下一张待收任务——队首已传完
                        // 的 submission 直接接棒；没有提前上传时（深度0 或
                        // 窗口尾部）在这里同步补一次上传。先落地窗口状态再
                        // 写盘：失败尽早进入 remoteSendAheadError 供下轮上报。
                        val successor = remoteUploadWindow.advanceAfterResult(itemIndex)
                        if (successor != null) {
                            val begun = if (successor.uploadedAhead) {
                                remoteUploads.removeFirst().await()
                            } else {
                                val nextInput = runImages[successor.index]
                                val nextOutputFile = batchOutputRepository.outputFileFor(
                                    outputDirectory = outputDirectory,
                                    input = nextInput
                                )
                                coordinatorClient!!.beginRemoteTask(
                                    input = nextInput,
                                    modelName = modelName,
                                    tileSize = DISTRIBUTED_TILE_SIZE,
                                    tileOverlap = DISTRIBUTED_TILE_OVERLAP,
                                    outputFormat = outputFormatFor(nextOutputFile)
                                )
                            }
                            remoteInFlight = begun.getOrNull()
                            remoteSendAheadError = begun.exceptionOrNull()
                                as? Exception ?: begun.exceptionOrNull()
                                ?.let { IllegalStateException(it.message) }
                            if (begun.isFailure) {
                                logRepository.error(
                                    "ProcessingRunEngine",
                                    "Send-ahead upload failed: ${begun.exceptionOrNull()?.message}"
                                )
                            }
                        }
                        fetchResult.fold(
                            onSuccess = { encoded -> flight.persist(encoded, outputFile) },
                            onFailure = { Result.failure(it) }
                        )
                    }
                        }
                    } else {
                        withContext(defaultDispatcher) {
                            val sessionForAhead = checkNotNull(currentSession)
                            // 本地 depth-1 流水线：处理当前张的同时预解码下一
                            // 张（推理忙 GPU/CPU，解码走独立线程）。join 保证
                            // process() 时缓存已就绪；解码失败由会话内正常解
                            // 码路径兜底。
                            val decodeAheadJob = runImages.getOrNull(itemIndex + 1)?.let { next ->
                                async(ioDispatcher) { sessionForAhead.prefetch(next) }
                            }
                            val result = sessionForAhead.process(
                                input = item,
                                outputFile = outputFile
                            )
                            decodeAheadJob?.join()
                            result
                        }
                    }

                    // PC 处理失败：直接报错并终止批次，不回退手机本地。
                    // 静默回退会掩盖 PC 端问题，且手机 CPU 处理大图极慢。
                    if (remoteRouting && result.isFailure) {
                        val reason = result.exceptionOrNull()?.message
                            ?: stringProvider.getString(R.string.processing_error_unknown_processing)
                        val errText = stringProvider.getString(R.string.processing_error_pc_failed, reason)
                        lastError = errText
                        val failedItemUri = item.uri.toString()
                        val previousAttempts = failureDetailsByUri[failedItemUri]?.attempts ?: 0
                        failureDetailsByUri[failedItemUri] = ProcessingFailureDetail(
                            uri = failedItemUri,
                            displayName = item.displayName,
                            errorMessage = errText,
                            attempts = previousAttempts + 1
                        )
                        withContext(ioDispatcher) {
                            batchOutputRepository.discardOutputFile(outputFile)
                        }
                        saveCheckpointSnapshot()
                        viewModel.updateUiState { state ->
                            val currentTask = state.task ?: return@updateUiState state
                            val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                            state.copy(
                                task = currentTask.copy(
                                    progress = planner.progressOf(successfulCount, totalItems),
                                    completedItems = successfulCount,
                                    pendingItems = pendingByUri.size,
                                    failedItems = orderedFailures.size,
                                    failureDetails = orderedFailures,
                                    lastErrorMessage = lastError,
                                    remotePhase = null,
                                    remoteItemProgress = null,
                                    remoteItemEtaSeconds = null
                                )
                            )
                        }
                        viewModel.emitRemoteFallbackMessage(errText)
                        logRepository.error(
                            "ProcessingViewModel",
                            "Remote processing failed for ${item.displayName}, aborting batch: $errText"
                        )
                        // 已推理完成的结果仍尽力提交完再退(成果不因 PC 故障
                        // 丢弃);它们的成败同样进 failureDetails/checkpoint。
                        drainCompletedCommits(waitAll = true)
                        break
                    }
                    val elapsedMs = System.currentTimeMillis() - startedAt
                    val itemUri = item.uri.toString()

                    if (result.isSuccess) {
                        if (remoteRouting) {
                            // 远程路径:commit 异步入串行通道,与 PC 推理/
                            // 收流重叠;记账延后到收割点(循环头/终态前)。
                            val queued = QueuedCommit(
                                item = item,
                                outputFile = outputFile,
                                elapsedMs = elapsedMs,
                                job = async(ioDispatcher) {
                                    commitLimiter.withPermit {
                                        batchOutputRepository.commitOutput(
                                            outputDirectory = outputDirectory,
                                            input = item,
                                            outputFile = outputFile
                                        )
                                    }
                                }
                            )
                            pendingCommits.add(queued)
                        } else {
                            // 本地路径:推理就是瓶颈,保持同步提交。
                            val commitResult = withContext(ioDispatcher) {
                                batchOutputRepository.commitOutput(
                                    outputDirectory = outputDirectory,
                                    input = item,
                                    outputFile = outputFile
                                )
                            }
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
                                if (!queueItemId.isNullOrBlank()) {
                                    viewModel.updateUiState { state ->
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
                                val errText = commitResult.exceptionOrNull()?.message
                                    ?: stringProvider.getString(R.string.processing_error_save_output)
                                lastError = errText
                                val previousAttempts = failureDetailsByUri[itemUri]?.attempts ?: 0
                                failureDetailsByUri[itemUri] = ProcessingFailureDetail(
                                    uri = itemUri,
                                    displayName = item.displayName,
                                    errorMessage = errText,
                                    attempts = previousAttempts + 1
                                )
                                withContext(ioDispatcher) {
                                    batchOutputRepository.discardOutputFile(outputFile)
                                }
                                logRepository.error(
                                    "ProcessingViewModel",
                                    "Failed to store output for ${item.displayName}: $lastError (pending=${pendingByUri.size})"
                                )
                            }
                        }
                    } else {
                        val errText = result.exceptionOrNull()?.message
                            ?: stringProvider.getString(R.string.processing_error_unknown_processing)
                        lastError = errText
                        val previousAttempts = failureDetailsByUri[itemUri]?.attempts ?: 0
                        failureDetailsByUri[itemUri] = ProcessingFailureDetail(
                            uri = itemUri,
                            displayName = item.displayName,
                            errorMessage = errText,
                            attempts = previousAttempts + 1
                        )
                        withContext(ioDispatcher) {
                            batchOutputRepository.discardOutputFile(outputFile)
                        }
                        logRepository.error(
                            "ProcessingViewModel",
                            "Failed to process ${item.displayName}: $lastError (pending=${pendingByUri.size})"
                        )
                    }
                    // 远程路径的 checkpoint 已随 commit 收割落盘(有变化才写),
                    // 不再每轮同步写;本地路径没有收割点,保持每轮快照。
                    if (!remoteRouting) {
                        saveCheckpointSnapshot()
                    }

                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                progress = planner.progressOf(successfulCount, totalItems),
                                currentItemName = item.displayName,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failureDetails = orderedFailures,
                                lastErrorMessage = lastError,
                                remotePhase = null,
                                remoteItemProgress = null,
                                remoteItemEtaSeconds = null
                            )
                        )
                    }
                }

                // 终态前全量收割:最后几张的 commit(以及任意仍在飞的任务)
                // 必须全部落账,pendingByUri/失败清单才反映磁盘真实状态。
                drainCompletedCommits(waitAll = true)

                if (pendingByUri.isEmpty()) {
                    checkpointRepository.clearCheckpoint()
                    // Record the first ready output so the queue sheet can open
                    // the result with the system image viewer on tap.
                    if (!queueItemId.isNullOrBlank()) {
                        val outputUri = withContext(ioDispatcher) {
                            batchOutputRepository.firstReadyOutputUri(
                                outputDirectory = outputDirectory,
                                inputs = allImages
                            )
                        }
                        if (outputUri != null) {
                            viewModel.updateUiState { state ->
                                state.copy(
                                    queueItems = state.queueItems.map { item ->
                                        if (item.id == queueItemId) {
                                            item.copy(lastOutputUri = outputUri)
                                        } else {
                                            item
                                        }
                                    }
                                )
                            }
                        }
                    }
                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        state.copy(
                            task = currentTask.copy(
                                status = ProcessingStatus.COMPLETED,
                                progress = 1.0f,
                                completedItems = totalItems,
                                pendingItems = 0,
                                failedItems = 0,
                                failureDetails = emptyList(),
                                currentItemName = null,
                                lastErrorMessage = null,
                                remotePhase = null,
                                remoteItemProgress = null,
                                remoteItemEtaSeconds = null
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
                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                status = terminalStatus,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failureDetails = orderedFailures,
                                progress = planner.progressOf(successfulCount, totalItems),
                                currentItemName = null,
                                lastErrorMessage = if (hasFailedPending) lastError else null,
                                remotePhase = null,
                                remoteItemProgress = null,
                                remoteItemEtaSeconds = null
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
                // The coroutine is already cancelled; wrap in NonCancellable so
                // the final checkpoint still reaches disk before rethrowing.
                withContext(NonCancellable) {
                    try {
                        saveCheckpointSnapshot()
                    } catch (checkpointFailure: Exception) {
                        logRepository.error(
                            "ProcessingViewModel",
                            "Failed to persist canceled task checkpoint: ${checkpointFailure.message}"
                        )
                    }
                }
                logRepository.warning("ProcessingViewModel", "Task $taskId canceled")
                throw cancellationException
            } catch (throwable: Throwable) {
                val errorMessage = throwable.message
                    ?: stringProvider.getString(R.string.processing_error_unknown_failure)
                lastError = errorMessage
                try {
                    saveCheckpointSnapshot()
                } catch (checkpointFailure: Exception) {
                    throwable.addSuppressed(checkpointFailure)
                    logRepository.error(
                        "ProcessingViewModel",
                        "Failed to persist failed task checkpoint: ${checkpointFailure.message}"
                    )
                }
                viewModel.updateUiState { state ->
                    val currentTask = state.task ?: return@updateUiState state
                    val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                    state.copy(
                        task = currentTask.copy(
                            status = ProcessingStatus.FAILED,
                            currentItemName = null,
                            completedItems = successfulCount,
                            pendingItems = pendingByUri.size,
                            failedItems = orderedFailures.size,
                            failureDetails = orderedFailures,
                            progress = planner.progressOf(successfulCount, totalItems),
                            lastErrorMessage = errorMessage,
                            remotePhase = null,
                            remoteItemProgress = null,
                            remoteItemEtaSeconds = null
                        ),
                        isProcessing = false
                    )
                }
                logRepository.error(
                    "ProcessingViewModel",
                    "Task $taskId failed unexpectedly: $errorMessage"
                )
            } finally {
                // 收尾的 native 调用都在主线程之外的调度器上做：run 体跑在
                // viewModelScope（Main.immediate），而 close() 会销毁 ncnn Net
                // 与 Vulkan 设备、endRemoteSession() 是阻塞 socket close——放在
                // 主线程上会卡顿（慢设备上可能 ANR）。NonCancellable 保证取消
                // 后也一定执行（Session.close() 本身幂等）。
                withContext(NonCancellable + defaultDispatcher) {
                    processingSession?.close()
                    if (remoteRouting) {
                        coordinatorClient?.endRemoteSession()
                    }
                }
                if (viewModel.currentUiState().isProcessing) {
                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        state.copy(
                            task = currentTask.copy(
                                currentItemName = null,
                                pendingItems = (currentTask.totalItems - currentTask.completedItems).coerceAtLeast(0),
                                remotePhase = null,
                                remoteItemProgress = null,
                                remoteItemEtaSeconds = null
                            ),
                            isProcessing = false
                        )
                    }
                }
            }
        }
        return true
    }

    /** 整批进度 = 已完成张数占比 + 当前图进行中的占比（远程百分比透传用）。 */
    private fun combinedRemoteProgress(
        completedItems: Int,
        totalItems: Int,
        itemFraction: Float
    ): Float {
        val base = planner.progressOf(completedItems, totalItems)
        if (totalItems <= 0) {
            return base
        }
        return (base + itemFraction / totalItems).coerceIn(0.0f, 1.0f)
    }

    companion object {
        // 0 = protocol "auto" sentinel: the PC picks the tile size with its
        // TileSizeOptimizer (probe cached per model, far better informed
        // than the phone could be about the PC's GPU/VRAM).
        private const val DISTRIBUTED_TILE_SIZE = 0
        private const val DISTRIBUTED_TILE_OVERLAP = 16
    }
}
