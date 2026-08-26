package io.upix.data.repository

import android.net.Uri
import io.upix.data.model.ModelImportResult
import io.upix.data.model.ModelInfo
import java.io.File
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

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
 * AVIF 仅在自带编码器的构建（-Pupix.avifEncoder=true）或 PC 远程处理时可写；
 * 两种情况下本地处理都不再硬失败，详见 [NativeCapabilities]。
 */
enum class OutputImageFormat(val forcedExtension: String?) {
    FOLLOW(null),
    PNG("png"),
    JPG("jpg"),
    WEBP("webp"),
    AVIF("avif")
}

/**
 * 高级编码参数（与 native 端 upix::EncoderOptions 对齐，字段名 snake_case
 * 以便 native 侧 glaze 反射直接解析；quality 由既有 outputQuality 通道单独
 * 传递，native 解析时会忽略该键）。默认值 = 历史行为。
 */
@Serializable
data class EncoderSettings(
    val quality: Int = 90,
    @SerialName("png_level") val pngLevel: Int = -1,
    @SerialName("jpeg_subsampling") val jpegSubsampling: Int = 420,
    @SerialName("webp_method") val webpMethod: Int = -1,
    @SerialName("webp_lossless") val webpLossless: Boolean = false,
    @SerialName("avif_speed") val avifSpeed: Int = 6,
    @SerialName("avif_subsampling") val avifSubsampling: Int = 420,
    @SerialName("avif_threads") val avifThreads: Int = 0
) {
    companion object {
        private val json = Json {
            encodeDefaults = true
            ignoreUnknownKeys = true
        }
    }

    /** JNI 传递用 JSON（含 quality，native 侧忽略未知键）。 */
    fun toNativeJson(): String = json.encodeToString(this)

    val normalizedQuality: Int get() = quality.coerceIn(1, 100)
}

interface BatchOutputStore {
    fun outputFormatSetting(): OutputImageFormat = OutputImageFormat.FOLLOW

    /** Writing AVIF is allowed: remote PC, or a local build with the AVIF encoder. */
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

    /**
     * 预取提示：提前解码 [input]，让下一张图的解码与当前图的推理/编码
     * 重叠（depth-1）。实现可以忽略；失败必须吞掉——process() 里的正常
     * 解码路径会兜底重试并把真实错误上报。每次调用覆盖上一个预取槽位。
     */
    fun prefetch(input: ResolvedImageInput) {}
}

interface ImageProcessingStore {
    fun openSession(
        modelName: String,
        modelBasePath: String,
        preferGpu: Boolean
    ): ImageProcessingSession?

    /** Run-time encoder settings; default no-op for test doubles. */
    fun updateEncoderSettings(settings: EncoderSettings) {}
}
