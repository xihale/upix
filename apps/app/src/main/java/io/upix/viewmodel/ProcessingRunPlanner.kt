package io.upix.viewmodel

import android.net.Uri
import io.upix.data.model.ModelInfo
import io.upix.data.repository.BatchOutputStore
import io.upix.data.repository.InputSourceResolver
import io.upix.data.repository.LogRepository
import io.upix.data.repository.ProcessingCheckpoint
import io.upix.data.repository.ProcessingCheckpointStore
import io.upix.data.repository.ProcessingFailureDetail
import io.upix.data.repository.ProcessingPendingItem
import io.upix.data.repository.ResolvedImageInput
import io.upix.data.repository.SrMarkerInfo

// Run planning and checkpoint bookkeeping extracted from ProcessingViewModel.
// The planner owns no state: every function is a computation over its arguments
// plus repository reads/writes that are injected in the constructor. It never
// reads view state — 每次运行的目录扫描开关由调用方显式传入。
class ProcessingRunPlanner(
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val checkpointRepository: ProcessingCheckpointStore,
    private val logRepository: LogRepository,
    // Byte-level SR-marker probe (reads the input URI). Injected so the
    // planner stays testable without a ContentResolver.
    private val probeSrMarker: (ResolvedImageInput) -> SrMarkerInfo?
) {
    internal fun buildRunSetup(
        imagePath: String,
        modelName: String,
        requestedOutputDirectoryUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean,
        skipSrMarkedInputs: Boolean,
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

        // 已超分跳过同样跟随 checkpoint（resume 还原当时的语义）。
        val skipMarkedInputs = resumeCheckpoint?.skipSrMarkedInputs ?: skipSrMarkedInputs

        // Drops inputs carrying this tool's own SR marker (already
        // super-resolved), logging each skip. Probe runs after the cheaper
        // output-existence check, so only still-pending inputs are read.
        fun filterSrMarked(
            inputs: List<ResolvedImageInput>
        ): Pair<List<ResolvedImageInput>, Int> {
            if (!skipMarkedInputs) {
                return inputs to 0
            }
            var skippedCount = 0
            val remaining = inputs.filterNot { input ->
                val marker = probeSrMarker(input)
                if (marker != null) {
                    skippedCount++
                    logRepository.info(
                        "ProcessingViewModel",
                        "Skipped already-upscaled input: ${input.displayName} " +
                            "(model=${marker.model} scale=${marker.scale}x ts=${marker.timestampSec})"
                    )
                    true
                } else {
                    false
                }
            }
            return remaining to skippedCount
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
                val pendingAfterOutputReady = pendingImagesFromCheckpoint.filterNot { input ->
                    batchOutputRepository.isOutputReady(outputDirectory = outputDirectory, input = input)
                }
                val (pendingImages, srSkippedCount) = filterSrMarked(pendingAfterOutputReady)

                // 输出已就绪与已超分跳过都不再进入本轮 run，一并计为已完成。
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
                        skippedItems = srSkippedCount,
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
                    skippedItems = srSkippedCount,
                    outputDirectory = outputDirectory,
                    failureDetails = failureDetails
                )
            }
        }

        val inputImages = inputSourceRepository.resolveImageInputs(source = imagePath, nested = scanNestedFolders)
        if (inputImages.isEmpty()) {
            return null
        }

        val pendingAfterOutputReady = inputImages.filterNot { item ->
            batchOutputRepository.isOutputReady(outputDirectory = outputDirectory, input = item)
        }
        val (pendingImages, srSkippedCount) = filterSrMarked(pendingAfterOutputReady)
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
            skippedItems = srSkippedCount,
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
        skipSrMarkedInputs: Boolean,
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
                skipSrMarkedInputs = skipSrMarkedInputs,
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
