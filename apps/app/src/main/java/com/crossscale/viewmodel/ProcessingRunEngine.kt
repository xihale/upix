package com.crossscale.viewmodel

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
import java.util.LinkedHashMap
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

// State-exchange surface between ProcessingRunEngine and the ViewModel that
// owns the run. The engine never holds _uiState, processingJob or
// activeQueueItemId — those stay in the ViewModel and are read/written through
// this interface so ownership and ordering stay exactly as they were when the
// code lived inline in ProcessingViewModel.
internal interface ProcessingRunEngineHost {
    // Equivalent of MutableStateFlow.update on the VM's fused ui-state flow.
    fun updateUiState(transform: (ProcessingUiState) -> ProcessingUiState)

    // Snapshot read of the current fused ui-state.
    fun currentUiState(): ProcessingUiState

    // Whether a run coroutine is currently active (VM: processingJob?.isActive == true).
    fun isProcessingJobActive(): Boolean

    // One-shot signal for the remote-fallback snackbar/banner (VM: _remoteFallbackMessage).
    fun emitRemoteFallbackMessage(message: String)

    // Starts the run coroutine in the host scope and records it as the active
    // processing job / active queue item (VM: processingJob = viewModelScope.launch
    // followed by activeQueueItemId = queueItemId, in that order).
    fun launch(queueItemId: String?, block: suspend CoroutineScope.() -> Unit)

    // End-of-run bookkeeping: clears the processing job and active queue item
    // and re-invokes the queue chain (handleQueuedRunFinished) if a queued
    // batch was running. Queue chaining itself stays in the ViewModel.
    fun onRunFinished()
}

// Execution engine for a single processing run, extracted verbatim from
// ProcessingViewModel.startProcessingInternal. It validates inputs, asks the
// planner for a ProcessingRunSetup, then drives the per-item loop (remote or
// native routing with mid-run remote fallback), output commit/discard,
// checkpointing and terminal state transitions — reporting every state change
// through ProcessingRunEngineHost.
internal class ProcessingRunEngine(
    private val host: ProcessingRunEngineHost,
    private val planner: ProcessingRunPlanner,
    private val modelRepository: ModelProvider,
    private val nativeImageProcessingRepository: ImageProcessingStore,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val coordinatorClient: CoordinatorClient?,
    private val logRepository: LogRepository,
    private val defaultDispatcher: CoroutineDispatcher
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
        if (host.isProcessingJobActive()) {
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
            val errorMessage = "Model not found: $modelName"
            logRepository.error("ProcessingViewModel", errorMessage)
            host.updateUiState { state ->
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
            activeTask = host.currentUiState().task
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

        host.updateUiState { state ->
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
                    host.updateUiState { state ->
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
            host.updateUiState { state ->
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

        planner.saveCheckpointSnapshot(
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

        host.updateUiState { state ->
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
                    failedItemNames = planner.failureNamesFromOrdered(initialFailureList),
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

        host.launch(queueItemId) {
            var successfulCount = completedCount
            var lastError: String? = initialLastError
            val pendingByUri = LinkedHashMap<String, ResolvedImageInput>()
            pendingImages.forEach { pendingByUri[it.uri.toString()] = it }
            val failureDetailsByUri = initialFailures.toMutableMap()
            // Per-image routing flag. Starts as the initial mode but may flip to
            // false mid-batch once the PC is found unreachable, so the rest of the
            // batch runs natively without re-trying a dead link each image.
            var remoteRouting = effectiveDistributedMode
            var processingSession: ImageProcessingSession? = null

            try {
                if (remoteRouting) {
                    // remoteRouting == true only when coordinatorClient != null
                    // (invariant: effectiveDistributedMode = coordinatorClient?.isConnected() == true).
                    val endpoint = coordinatorClient.activeDeviceId().orEmpty()
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
                        val errorMessage = "Failed to initialize native session for model $modelName"
                        lastError = errorMessage
                        logRepository.error("ProcessingViewModel", errorMessage)
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
                        host.updateUiState { state ->
                            val currentTask = state.task ?: return@updateUiState state
                            val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                            state.copy(
                                task = currentTask.copy(
                                    status = ProcessingStatus.FAILED,
                                    currentItemName = null,
                                    pendingItems = pendingByUri.size,
                                    failedItems = orderedFailures.size,
                                    failedItemNames = planner.failureNamesFromOrdered(orderedFailures),
                                    failureDetails = orderedFailures,
                                    lastErrorMessage = errorMessage
                                ),
                                isProcessing = false
                            )
                        }
                        return@launch
                    }
                }

                for (item in runImages) {
                    if (!isActive) {
                        return@launch
                    }

                    host.updateUiState { state ->
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

                    // PC unreachable mid-batch: drop remote for this image and the
                    // rest of the batch, re-process THIS image natively, and emit a
                    // UI signal so the mode chip / snackbar can react.
                    if (remoteRouting && result.isFailure) {
                        host.emitRemoteFallbackMessage("PC 不可达，已切换本地处理")
                        logRepository.warning(
                            "ProcessingViewModel",
                            "Remote submit failed for ${item.displayName}, falling back to native for remaining items"
                        )
                        remoteRouting = false
                        val reopenedSession = processingSession ?: withContext(defaultDispatcher) {
                            nativeImageProcessingRepository.openSession(
                                modelName = modelName,
                                modelBasePath = modelBasePath,
                                preferGpu = true
                            )
                        }
                        processingSession = reopenedSession
                        if (reopenedSession != null) {
                            result = withContext(defaultDispatcher) {
                                reopenedSession.process(input = item, outputFile = outputFile)
                            }
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
                            latestProcessorLabel = planner.resolveProcessorLabel(
                                explicitProcessor = result.getOrNull()?.processorLabel,
                                distributedRemoteEnabled = remoteRouting
                            )
                            latestAttributionEntry = "${item.displayName} · ${latestProcessorLabel}"
                            logRepository.info(
                                "ProcessingViewModel",
                                "Processed ${item.displayName} by ${latestProcessorLabel} -> $destination (${elapsedMs}ms, done=$successfulCount/$totalItems, pending=${pendingByUri.size})"
                            )
                            if (!queueItemId.isNullOrBlank()) {
                                host.updateUiState { state ->
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

                    host.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                        val updatedAttributions = if (latestAttributionEntry == null) {
                            currentTask.recentProcessorAttributions
                        } else {
                            (currentTask.recentProcessorAttributions + latestAttributionEntry).takeLast(MAX_RECENT_ATTRIBUTIONS)
                        }
                        state.copy(
                            task = currentTask.copy(
                                progress = planner.progressOf(successfulCount, totalItems),
                                currentItemName = item.displayName,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = planner.failureNamesFromOrdered(orderedFailures),
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
                    host.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
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
                    host.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                status = terminalStatus,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = planner.failureNamesFromOrdered(orderedFailures),
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
                logRepository.warning("ProcessingViewModel", "Task $taskId canceled")
                throw cancellationException
            } catch (throwable: Throwable) {
                val errorMessage = throwable.message ?: "Unknown task failure"
                lastError = errorMessage
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
                    host.updateUiState { state ->
                        val currentTask = state.task ?: return@updateUiState state
                        val orderedFailures = planner.orderedFailureDetails(failureDetailsByUri)
                        state.copy(
                            task = currentTask.copy(
                                status = ProcessingStatus.FAILED,
                                currentItemName = null,
                                completedItems = successfulCount,
                                pendingItems = pendingByUri.size,
                                failedItems = orderedFailures.size,
                                failedItemNames = planner.failureNamesFromOrdered(orderedFailures),
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
                if (host.currentUiState().isProcessing) {
                    host.updateUiState { state ->
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
                host.onRunFinished()
            }
        }
        return true
    }

    companion object {
        private const val DISTRIBUTED_TILE_SIZE = 256
        private const val DISTRIBUTED_TILE_OVERLAP = 16
        private const val MAX_RECENT_ATTRIBUTIONS = 8
    }
}
