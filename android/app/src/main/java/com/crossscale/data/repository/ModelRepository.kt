package com.crossscale.data.repository

import android.content.Context
import android.os.Environment
import com.crossscale.data.model.ModelInfo
import java.io.File
import java.security.MessageDigest
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

class ModelRepository(
    private val context: Context,
    private val logRepository: LogRepository
) : ModelProvider {
    private val modelsDirectory = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
        ?.let { directory -> File(directory, "models") }
        ?: File(context.filesDir, "models")

    private val _models = MutableStateFlow<List<ModelInfo>>(emptyList())
    val models: StateFlow<List<ModelInfo>> = _models.asStateFlow()

    suspend fun syncBundledModels() = withContext(Dispatchers.IO) {
        ensureModelDirectory()

        val copied = copyBundledDefaultModel()
        if (!copied) {
            logRepository.warning("ModelRepository", "Bundled default model copy failed")
        }
        removeUnexpectedModelFiles()

        refreshModelsInternal()
    }

    suspend fun refreshModels() = withContext(Dispatchers.IO) {
        ensureModelDirectory()
        refreshModelsInternal()
    }

    suspend fun deleteModel(modelInfo: ModelInfo) = withContext(Dispatchers.IO) {
        val paramFile = File(modelInfo.paramPath)
        val binFile = File(modelInfo.binPath)
        if (paramFile.exists()) {
            paramFile.delete()
        }
        if (binFile.exists()) {
            binFile.delete()
        }
        logRepository.info("ModelRepository", "Deleted model ${modelInfo.name}")
        refreshModelsInternal()
    }

    override fun getModelByName(name: String): ModelInfo? {
        return _models.value.firstOrNull { model ->
            model.name == name
        }
    }

    fun modelDirectoryPath(): String {
        return modelsDirectory.name
    }

    private fun ensureModelDirectory() {
        if (!modelsDirectory.exists() && !modelsDirectory.mkdirs()) {
            logRepository.error("ModelRepository", "Failed to create model directory ${modelsDirectory.path}")
        }
    }

    private fun refreshModelsInternal() {
        val param = File(modelsDirectory, DEFAULT_MODEL_PARAM_FILE)
        val bin = File(modelsDirectory, DEFAULT_MODEL_BIN_FILE)
        if (!param.exists() || !bin.exists()) {
            _models.value = emptyList()
            logRepository.warning(
                "ModelRepository",
                "Default model files missing: param=${param.exists()} bin=${bin.exists()}"
            )
            return
        }

        _models.value = listOf(toModelInfo(DEFAULT_MODEL_NAME, param, bin))
    }

    private fun copyBundledDefaultModel(): Boolean {
        val paramAssetPath = findAssetPath(PARAM_ASSET_CANDIDATES)
        val binAssetPath = findAssetPath(BIN_ASSET_CANDIDATES)
        if (paramAssetPath == null || binAssetPath == null) {
            logRepository.error(
                "ModelRepository",
                "Bundled default model assets not found (param=$paramAssetPath, bin=$binAssetPath)"
            )
            return false
        }

        val paramCopied = copyAssetToDestination(
            assetPath = paramAssetPath,
            destination = File(modelsDirectory, DEFAULT_MODEL_PARAM_FILE)
        )
        val binCopied = copyAssetToDestination(
            assetPath = binAssetPath,
            destination = File(modelsDirectory, DEFAULT_MODEL_BIN_FILE)
        )
        return paramCopied && binCopied
    }

    private fun findAssetPath(candidates: List<String>): String? {
        return candidates.firstOrNull { candidate ->
            runCatching { context.assets.open(candidate).close() }.isSuccess
        }
    }

    private fun copyAssetToDestination(
        assetPath: String,
        destination: File
    ): Boolean {
        return runCatching {
            context.assets.open(assetPath).use { input ->
                destination.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            true
        }.getOrElse { throwable ->
            logRepository.error(
                "ModelRepository",
                "Failed to copy asset $assetPath -> ${destination.name}: ${throwable.message}"
            )
            false
        }
    }

    private fun removeUnexpectedModelFiles() {
        val allowedFiles = setOf(DEFAULT_MODEL_PARAM_FILE, DEFAULT_MODEL_BIN_FILE)
        val modelFiles = modelsDirectory.listFiles { file ->
            file.extension == "param" || file.extension == "bin"
        } ?: return

        modelFiles.forEach { file ->
            if (file.name !in allowedFiles) {
                val removed = runCatching { file.delete() }.getOrDefault(false)
                if (removed) {
                    logRepository.info("ModelRepository", "Removed legacy model file: ${file.name}")
                } else {
                    logRepository.warning("ModelRepository", "Failed to remove legacy model file: ${file.name}")
                }
            }
        }
    }

    private fun toModelInfo(modelName: String, param: File, bin: File): ModelInfo {
        val totalSize = param.length() + bin.length()
        val scaleFactor = inferScaleFactor(modelName)
        return ModelInfo(
            name = modelName,
            scaleFactor = scaleFactor,
            inputChannels = 3,
            tileSize = 0,
            totalSize = totalSize,
            hash = computeHash(param, bin),
            paramPath = param.absolutePath,
            binPath = bin.absolutePath
        )
    }

    private fun inferScaleFactor(modelName: String): Int {
        val regex = Regex("x(\\d+)", RegexOption.IGNORE_CASE)
        return regex.find(modelName)?.groupValues?.getOrNull(1)?.toIntOrNull() ?: 4
    }

    private fun computeHash(param: File, bin: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        updateDigestWithFile(digest, param)
        updateDigestWithFile(digest, bin)
        return digest.digest().joinToString(separator = "") { byte -> "%02x".format(byte) }
    }

    private fun updateDigestWithFile(digest: MessageDigest, file: File) {
        file.inputStream().use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val bytesRead = input.read(buffer)
                if (bytesRead <= 0) {
                    return
                }
                digest.update(buffer, 0, bytesRead)
            }
        }
    }

    companion object {
        private const val DEFAULT_MODEL_NAME = "Real-ESRGANv3-anime_x2"
        private const val DEFAULT_MODEL_PARAM_FILE = "$DEFAULT_MODEL_NAME.param"
        private const val DEFAULT_MODEL_BIN_FILE = "$DEFAULT_MODEL_NAME.bin"
        private val PARAM_ASSET_CANDIDATES = listOf(
            "x2.param",
            "Real-ESRGANv3-anime/x2.param",
            DEFAULT_MODEL_PARAM_FILE
        )
        private val BIN_ASSET_CANDIDATES = listOf(
            "x2.bin",
            "Real-ESRGANv3-anime/x2.bin",
            DEFAULT_MODEL_BIN_FILE
        )
    }

}
