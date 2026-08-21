package com.crossscale.data.repository

import android.net.Uri
import com.crossscale.data.model.ModelImportResult
import com.crossscale.data.model.ModelInfo
import java.io.File
import kotlinx.coroutines.flow.StateFlow

/**
 * Model catalog contract: observe the available models and manage their
 * lifecycle (discovery refresh, import, delete, remote download).
 */
interface ModelProvider {
    val models: StateFlow<List<ModelInfo>>
    fun getModelByName(name: String): ModelInfo?
    fun modelDirectoryPath(): String
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

/**
 * 输出格式设置：FOLLOW 表示跟随原图扩展名（现状行为），
 * 其余枚举在生成输出文件名时强制对应扩展名。
 */
enum class OutputImageFormat(val forcedExtension: String?) {
    FOLLOW(null),
    PNG("png"),
    JPG("jpg"),
    WEBP("webp")
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

    /** Off-main-thread save: serialization + disk write run on Dispatchers.IO. */
    suspend fun persistCheckpoint(checkpoint: ProcessingCheckpoint)

    fun loadCheckpoint(): ProcessingCheckpoint?

    fun clearCheckpoint()
}

interface ProcessingQueueStore {
    val queue: StateFlow<ProcessingQueueSnapshot>

    /** Off-main-thread save: serialization + disk write run on Dispatchers.IO. */
    suspend fun persistQueue(snapshot: ProcessingQueueSnapshot)

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
