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
import kotlinx.coroutines.async
import kotlinx.coroutines.isActive
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

// Execution engine for a single processing run, extracted verbatim from
// ProcessingViewModel.startProcessingInternal. It validates inputs, asks the
// planner for a ProcessingRunSetup, then drives the per-item loop (remote or
// native routing with mid-run remote fallback), output commit/discard,
// checkpointing and terminal state transitions — reporting every state change
// back to the owning ProcessingViewModel.
internal class ProcessingRunEngine(
    private val viewModel: ProcessingViewModel,
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
            // 远程流水线状态：remoteInFlight 是"已上传、待收结果"的任务。
            // 深度1（旧 PC）：读下一张字节与推理重叠，收完结果后才上传下一张。
            // 深度2（CONNECT_RESPONSE.pipeline_depth ≥ 2，PeerCapabilities.
            // deepPipeline）：上传窗口扩到 2——等结果 N 时就把 N+2 也推上链路，
            // 本侧"收 N + 写盘 N + SAF 提交 N"与"PC 推理 N+1、收 N+2 上传"
            // 全双工并行，逐张周期收敛到 max(PC 推理, PC 编码+回传)，
            // GPU 不再在图间断粮。窗口约束：wire 上同时至多一个未完成的
            // submission（单 upload 队列串行发射，DATA 帧永不交错，PC 按
            // FIFO 匹配 staging）。remoteSendAheadError 非空 = 发前上传失败
            // （发前路径不做任何会话恢复），循环下一迭代按当前图失败上报并
            // 终止批次。
            var remoteInFlight: CoordinatorClient.RemoteTask? = null
            val remoteUploads = ArrayDeque<
                kotlinx.coroutines.Deferred<Result<CoordinatorClient.RemoteTask>>>()
            var uploadsLaunched = 0
            var remoteSendAheadError: Exception? = null

            // Persists a checkpoint reflecting the run's current progress. The
            // run-invariant fields are captured here; the mutable progress
            // (successfulCount / pendingByUri / failureDetailsByUri / lastError)
            // is read at invocation time, so every call site is a one-liner.
            suspend fun saveCheckpointSnapshot() {
                planner.saveCheckpointSnapshot(
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
                    skipSrMarkedInputs = effectiveSkipSrMarked,
                    createdAtEpochMs = createdAt,
                    pendingImages = pendingByUri.values.toList(),
                    failureDetails = failureDetailsByUri,
                    lastErrorMessage = lastError
                )
            }

            // Initial checkpoint (moved into the run coroutine so the disk
            // write happens off the main thread via persistCheckpoint).
            saveCheckpointSnapshot()

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
                    processingSession = withContext(defaultDispatcher) {
                        nativeImageProcessingRepository.openSession(
                            modelName = modelName,
                            modelBasePath = modelBasePath,
                            preferGpu = true
                        )
                    }

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

                for ((itemIndex, item) in runImages.withIndex()) {
                    if (!isActive) {
                        return@launchRun
                    }

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
                                    ?: IllegalStateException("Remote upload failed")
                            )
                        } else {
                    withContext(ioDispatcher) {
                        val submitAheadActive = PeerCapabilities.submitAhead
                        // 深度1（旧 PC）：预取下一张原图字节，与 PC 处理当前张
                        // 重叠。深度2+：读取挪进上传作业内做。
                        if (!submitAheadActive && itemIndex + 1 < runImages.size) {
                            coordinatorClient!!.prefetchRemoteInput(runImages[itemIndex + 1])
                        }
                        // 发前上传窗口补满：保持 uploaded-awaiting ≤ 深度。
                        // PC depth 1 → 提前 1 张（N+1）；depth ≥ 2 → 提前
                        // 2 张（本轮补发 N+2）。全部经由同一个异步队列串行
                        // 入线（队列 head 尚未完成时新作业也不会发送——
                        // kotlinx 的 async 只调度，真正的 socket 写在各作业
                        // 线程内完成；深度窗口 = 队列长度约束本身保证至多
                        // 一个未完成 submission 在 wire 上）。
                        val aheadDepth = when {
                            PeerCapabilities.deepPipeline -> 2
                            submitAheadActive -> 1
                            else -> 0
                        }
                        while (uploadsLaunched - itemIndex < aheadDepth &&
                            uploadsLaunched < runImages.size
                        ) {
                            val k = uploadsLaunched++
                            val kOutputFile = batchOutputRepository.outputFileFor(
                                outputDirectory = outputDirectory,
                                input = runImages[k]
                            )
                            remoteUploads.addLast(
                                async(ioDispatcher) {
                                    coordinatorClient!!.beginRemoteTaskAhead(
                                        input = runImages[k],
                                        modelName = modelName,
                                        tileSize = DISTRIBUTED_TILE_SIZE,
                                        tileOverlap = DISTRIBUTED_TILE_OVERLAP,
                                        outputFormat = outputFormatFor(kOutputFile)
                                    )
                                }
                            )
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
                        // 收完结果 N：把队首已传完的 submission 换成下一轮的
                        // 待收任务（先落地发前结果再写盘：窗口状态在本轮末尾
                        // 收敛，失败尽早进入 remoteSendAheadError 供下轮上报）。
                        if (remoteUploads.isNotEmpty()) {
                            val begun = remoteUploads.removeFirst().await()
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
                        lastError = "PC 处理失败：$reason"
                        val failedItemUri = item.uri.toString()
                        val previousAttempts = failureDetailsByUri[failedItemUri]?.attempts ?: 0
                        failureDetailsByUri[failedItemUri] = ProcessingFailureDetail(
                            uri = failedItemUri,
                            displayName = item.displayName,
                            errorMessage = lastError,
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
                        viewModel.emitRemoteFallbackMessage(lastError)
                        logRepository.error(
                            "ProcessingViewModel",
                            "Remote processing failed for ${item.displayName}, aborting batch: $lastError"
                        )
                        break
                    }
                    val elapsedMs = System.currentTimeMillis() - startedAt
                    val itemUri = item.uri.toString()

                    if (result.isSuccess) {
                        // 提交输出（SAF/MediaStore 写入）是文件 IO，切到 IO 线程。
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
                            lastError = commitResult.exceptionOrNull()?.message
                                ?: stringProvider.getString(R.string.processing_error_save_output)
                            val previousAttempts = failureDetailsByUri[itemUri]?.attempts ?: 0
                            failureDetailsByUri[itemUri] = ProcessingFailureDetail(
                                uri = itemUri,
                                displayName = item.displayName,
                                errorMessage = lastError,
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
                    } else {
                        lastError = result.exceptionOrNull()?.message
                            ?: stringProvider.getString(R.string.processing_error_unknown_processing)
                        val previousAttempts = failureDetailsByUri[itemUri]?.attempts ?: 0
                        failureDetailsByUri[itemUri] = ProcessingFailureDetail(
                            uri = itemUri,
                            displayName = item.displayName,
                            errorMessage = lastError,
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

                    saveCheckpointSnapshot()

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
                    saveCheckpointSnapshot()
                }
                logRepository.warning("ProcessingViewModel", "Task $taskId canceled")
                throw cancellationException
            } catch (throwable: Throwable) {
                val errorMessage = throwable.message
                    ?: stringProvider.getString(R.string.processing_error_unknown_failure)
                lastError = errorMessage
                saveCheckpointSnapshot()
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
                processingSession?.close()
                if (remoteRouting) {
                    coordinatorClient?.endRemoteSession()
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
