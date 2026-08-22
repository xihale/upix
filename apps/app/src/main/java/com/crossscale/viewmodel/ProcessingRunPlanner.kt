package com.crossscale.viewmodel

import android.net.Uri
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.repository.BatchOutputStore
import com.crossscale.data.repository.InputSourceResolver
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.ProcessingFailureDetail
import com.crossscale.data.repository.ProcessingPendingItem
import com.crossscale.data.repository.ResolvedImageInput

// Run planning and checkpoint bookkeeping extracted from ProcessingViewModel.
// The planner owns no state: every function is a computation over its arguments
// plus repository reads/writes that are injected in the constructor. It never
// reads view state — 每次运行的目录扫描开关由调用方显式传入。
class ProcessingRunPlanner(
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val logRepository: LogRepository
) {
    internal fun buildRunSetup(
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

        // 目录扫描使用“本次运行自己的开关”：断点续跑跟随 checkpoint 记录的
        // 值，其余情况跟随调用参数（UI 开关 / 队列项），绝不读上一个任务。
        val scanNestedFolders = resumeCheckpoint?.processNestedFolders ?: processNestedFolders

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

        val inputImages = inputSourceRepository.resolveImageInputs(source = imagePath, nested = scanNestedFolders)
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
            allImages = inputImages,
            runImages = pendingImages,
            pendingImages = pendingImages,
            completedItems = completedItems,
            totalItems = totalItems,
            outputDirectory = outputDirectory,
            failureDetails = failureDetails
        )
    }

    suspend fun saveCheckpointSnapshot(
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

        checkpointRepository.persistCheckpoint(
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
                failureDetails = orderedFailureDetails(failureDetails)
            )
        )
    }

    fun orderedFailureDetails(failureDetails: Map<String, ProcessingFailureDetail>): List<ProcessingFailureDetail> {
        if (failureDetails.isEmpty()) {
            return emptyList()
        }
        return failureDetails.values
            .sortedWith(compareBy({ it.displayName.lowercase() }, { it.uri }))
    }

    fun modelBasePathFor(modelInfo: ModelInfo): String {
        return if (modelInfo.paramPath.endsWith(PARAM_FILE_SUFFIX, ignoreCase = true)) {
            modelInfo.paramPath.dropLast(PARAM_FILE_SUFFIX.length)
        } else {
            modelInfo.paramPath
        }
    }

    fun progressOf(completedItems: Int, totalItems: Int): Float {
        if (totalItems <= 0) {
            return 0.0f
        }
        return (completedItems.toFloat() / totalItems.toFloat()).coerceIn(0.0f, 1.0f)
    }

    companion object {
        private const val PARAM_FILE_SUFFIX = ".param"
    }
}
