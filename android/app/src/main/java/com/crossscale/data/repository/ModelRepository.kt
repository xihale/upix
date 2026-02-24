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

        val copiedCount = copyBundledModelsRecursively("models")
        if (copiedCount == 0) {
            logRepository.warning("ModelRepository", "No bundled model files copied from assets/models")
        }

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
        return modelsDirectory.absolutePath
    }

    private fun ensureModelDirectory() {
        if (!modelsDirectory.exists() && !modelsDirectory.mkdirs()) {
            logRepository.error("ModelRepository", "Failed to create model directory ${modelsDirectory.path}")
        }
    }

    private fun refreshModelsInternal() {
        val params = modelsDirectory
            .listFiles { file -> file.extension == "param" }
            ?.sortedBy { it.name }
            ?: emptyList()

        val discoveredModels = params.mapNotNull { param ->
            val modelName = param.nameWithoutExtension
            val bin = File(modelsDirectory, "$modelName.bin")
            if (!bin.exists()) {
                logRepository.warning("ModelRepository", "Model $modelName missing .bin file, skipped")
                null
            } else {
                toModelInfo(modelName, param, bin)
            }
        }

        _models.value = discoveredModels
    }

    private fun copyBundledModelsRecursively(assetPath: String): Int {
        val entries = try {
            context.assets.list(assetPath) ?: return 0
        } catch (e: Exception) {
            return 0
        }

        var copiedCount = 0
        for (entry in entries) {
            val fullAssetPath = "$assetPath/$entry"
            val nestedEntries = try {
                context.assets.list(fullAssetPath) ?: emptyArray()
            } catch (e: Exception) {
                emptyArray()
            }
            if (nestedEntries.isEmpty()) {
                if (!entry.endsWith(".param") && !entry.endsWith(".bin")) {
                    continue
                }
                val destinationName = fullAssetPath.removePrefix("models/").replace('/', '_')
                val destination = File(modelsDirectory, destinationName)
                if (destination.exists() && destination.length() > 0L) {
                    continue
                }
                try {
                    context.assets.open(fullAssetPath).use { input ->
                        destination.outputStream().use { output ->
                            input.copyTo(output)
                        }
                    }
                    copiedCount += 1
                    logRepository.info("ModelRepository", "Bundled model copied: $destinationName")
                } catch (e: Exception) {
                    logRepository.error("ModelRepository", "Failed to copy $fullAssetPath: ${e.message}")
                }
            } else {
                copiedCount += copyBundledModelsRecursively(fullAssetPath)
            }
        }
        return copiedCount
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
}
