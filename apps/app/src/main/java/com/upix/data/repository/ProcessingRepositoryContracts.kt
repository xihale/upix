package com.upix.data.repository

import android.net.Uri
import com.upix.data.model.ModelImportResult
import com.upix.data.model.ModelInfo
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
 *
 * AVIF 只能由 PC 端系统 libavif 编码。本地处理时强制 AVIF 会直接失败；
 * FOLLOW 遇到 .avif 原图时本地落到 PNG。
 */
enum class OutputImageFormat(val forcedExtension: String?) {
    FOLLOW(null),
    PNG("png"),
    JPG("jpg"),
    WEBP("webp"),
    AVIF("avif")
}

interface BatchOutputStore {
    fun outputFormatSetting(): OutputImageFormat = OutputImageFormat.FOLLOW

    /** Remote PC can encode AVIF; local device cannot. */
    fun setWriteAvif(enabled: Boolean) {}

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

    /** Off-main-thread read: disk IO must not run on the main thread. */
    suspend fun loadCheckpoint(): ProcessingCheckpoint?

    /** Off-main-thread delete: disk IO must not run on the main thread. */
    suspend fun clearCheckpoint()
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
