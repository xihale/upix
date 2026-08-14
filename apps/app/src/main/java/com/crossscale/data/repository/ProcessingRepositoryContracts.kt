package com.crossscale.data.repository

import android.net.Uri
import com.crossscale.data.model.ModelImportResult
import com.crossscale.data.model.ModelInfo
import java.io.File
import kotlinx.coroutines.flow.StateFlow

/**
 * Model catalog contract: observe the available models and manage their
 * lifecycle (discovery refresh, bundled sync, import, delete).
 */
interface ModelProvider {
    val models: StateFlow<List<ModelInfo>>
    fun getModelByName(name: String): ModelInfo?
    fun modelDirectoryPath(): String
    suspend fun syncBundledModels()
    suspend fun refreshModels()
    suspend fun importModels(inputUris: List<Uri>): ModelImportResult
    suspend fun deleteModel(modelInfo: ModelInfo)
}

interface InputSourceResolver {
    fun resolveImageInputs(
        source: String,
        nested: Boolean
    ): List<ResolvedImageInput>
}

interface BatchOutputStore {
    fun prepareOutputDirectory(
        source: String,
        modelName: String,
        preferredTreeUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        processNestedFolders: Boolean,
        resumeOutputDirectory: String?,
        resumeOutputDirectoryUri: String?,
        resumeOverwriteOriginal: Boolean?,
        resumeBackupOriginal: Boolean?
    ): BatchOutputDirectory

    fun outputFileFor(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ): File

    fun isOutputReady(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ): Boolean

    fun firstReadyOutputUri(
        outputDirectory: BatchOutputDirectory,
        inputs: List<ResolvedImageInput>
    ): String?

    fun hasBackup(input: ResolvedImageInput): Boolean

    fun restoreBackup(input: ResolvedImageInput): Result<Unit>

    fun commitOutput(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput,
        outputFile: File
    ): Result<String>

    fun discardOutputFile(outputFile: File)
}

interface ProcessingCheckpointStore {
    val checkpoint: StateFlow<ProcessingCheckpoint?>

    fun saveCheckpoint(checkpoint: ProcessingCheckpoint)

    fun loadCheckpoint(): ProcessingCheckpoint?

    fun clearCheckpoint()
}

interface ProcessingQueueStore {
    val queue: StateFlow<ProcessingQueueSnapshot>

    fun saveQueue(snapshot: ProcessingQueueSnapshot)

    fun loadQueue(): ProcessingQueueSnapshot
}

interface ImageProcessingSession : AutoCloseable {
    fun process(
        input: ResolvedImageInput,
        outputFile: File
    ): Result<NativeProcessResult>
}

interface ImageProcessingStore {
    fun openSession(
        modelName: String,
        modelBasePath: String,
        preferGpu: Boolean
    ): ImageProcessingSession?
}
