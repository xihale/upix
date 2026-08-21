package com.crossscale.viewmodel

import com.crossscale.R
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.data.model.ProcessingTask
import com.crossscale.data.repository.BatchOutputStore
import com.crossscale.data.repository.ImageProcessingSession
import com.crossscale.data.repository.ImageProcessingStore
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.ProcessingFailureDetail
import com.crossscale.data.repository.ResolvedImageInput
import com.crossscale.network.CoordinatorClient
import com.crossscale.util.StringProvider
import java.util.LinkedHashMap
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
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

        val setup = planner.buildRunSetup(
            imagePath = imagePath,
            modelName = modelName,
            requestedOutputDirectoryUri = outputDirectoryUri,
            overwriteOriginal = overwriteOriginal,
            backupOriginal = backupOriginal,
            processNestedFolders = processNestedFolders,
            resumeCheckpoint = resumeCheckpoint,
            retryFailedOnly = retryFailedOnly,
            resumePendingUris = resumePendingUris,
            activeTask = viewModel.currentUiState().task
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
        val initialFailureList = planner.orderedFailureDetails(initialFailures)
        val initialLastError = resumeCheckpoint?.lastErrorMessage

        if (pendingImages.isEmpty()) {
            checkpointRepository.clearCheckpoint()
            if (!queueItemId.isNullOrBlank()) {
                val existingOutputUri = batchOutputRepository.firstReadyOutputUri(
                    outputDirectory = outputDirectory,
                    inputs = allImages
                )
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
            return true
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
                        currentItemName = runImages.firstOrNull()?.displayName,
                    completedItems = completedCount,
                    pendingItems = pendingImages.size,
                    failedItems = initialFailures.size,
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
            "Started task $taskId with model=$modelName, total=$totalItems, completed=$completedCount, pending=${pendingImages.size}, run=${runImages.size}, retryFailedOnly=$retryFailedOnly, outputDir=${outputDirectory.displayPath}"
        )

        val modelBasePath = planner.modelBasePathFor(modelInfo)

        viewModel.launchRun(queueItemId) {
            var successfulCount = completedCount
            var lastError: String? = initialLastError
            val pendingByUri = LinkedHashMap<String, ResolvedImageInput>()
            pendingImages.forEach { pendingByUri[it.uri.toString()] = it }
            val failureDetailsByUri = initialFailures.toMutableMap()
            // Per-image routing mode, fixed for the whole run: a PC failure
            // aborts the batch with an error instead of silently falling back.
            val remoteRouting = effectiveDistributedMode
            var processingSession: ImageProcessingSession? = null

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

                for (item in runImages) {
                    if (!isActive) {
                        return@launchRun
                    }

                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        state.copy(
                            task = currentTask.copy(
                                currentItemName = item.displayName,
                                pendingItems = pendingByUri.size,
                                progress = planner.progressOf(successfulCount, totalItems)
                            )
                        )
                    }

                    val outputFile = batchOutputRepository.outputFileFor(outputDirectory = outputDirectory, input = item)

                    val startedAt = System.currentTimeMillis()
                    val currentSession = processingSession
                    var result = withContext(defaultDispatcher) {
                        if (remoteRouting) {
                            coordinatorClient!!.processInputRemotely(
                                input = item,
                                modelName = modelName,
                                tileSize = DISTRIBUTED_TILE_SIZE,
                                tileOverlap = DISTRIBUTED_TILE_OVERLAP,
                                outputFile = outputFile
                            )
                        } else {
                            checkNotNull(currentSession).process(
                                input = item,
                                outputFile = outputFile
                            )
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
                        batchOutputRepository.discardOutputFile(outputFile)
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
                            batchOutputRepository.discardOutputFile(outputFile)
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
                        batchOutputRepository.discardOutputFile(outputFile)
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
                                lastErrorMessage = lastError
                            )
                        )
                    }
                }

                if (pendingByUri.isEmpty()) {
                    checkpointRepository.clearCheckpoint()
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
                if (viewModel.currentUiState().isProcessing) {
                    viewModel.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        state.copy(
                            task = currentTask.copy(
                                currentItemName = null,
                                pendingItems = (currentTask.totalItems - currentTask.completedItems).coerceAtLeast(0)
                            ),
                            isProcessing = false
                        )
                    }
                }
                viewModel.onRunFinished()
            }
        }
        return true
    }

    companion object {
        private const val DISTRIBUTED_TILE_SIZE = 256
        private const val DISTRIBUTED_TILE_OVERLAP = 16
    }
}
