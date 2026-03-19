package com.crossscale.data.repository

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.ModelImportResult
import java.io.BufferedInputStream
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.Locale
import java.util.zip.ZipInputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

class ModelRepository(
    private val context: Context,
    private val logRepository: LogRepository
) : ModelProvider {
    private val contentResolver = context.contentResolver
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
        cleanupEmptyParentDirectories(paramFile.parentFile)
        cleanupEmptyParentDirectories(binFile.parentFile)
        logRepository.info("ModelRepository", "Deleted model ${modelInfo.name}")
        refreshModelsInternal()
    }

    suspend fun importModels(inputUris: List<Uri>): ModelImportResult = withContext(Dispatchers.IO) {
        ensureModelDirectory()

        val normalizedUris = inputUris.distinct()
        if (normalizedUris.isEmpty()) {
            return@withContext ModelImportResult(
                invalidItems = listOf("未选择任何模型文件")
            )
        }

        val stagingRoot = File(
            context.cacheDir,
            "model-import-${System.currentTimeMillis()}-${System.nanoTime()}"
        )
        if (!stagingRoot.exists() && !stagingRoot.mkdirs()) {
            logRepository.error(
                "ModelRepository",
                "Failed to create staging directory for model import: ${stagingRoot.absolutePath}"
            )
            return@withContext ModelImportResult(
                invalidItems = listOf("模型导入失败：无法创建临时目录")
            )
        }

        val candidates = linkedMapOf<String, MutableModelImportCandidate>()
        val invalidItems = mutableListOf<String>()
        val importedModelNames = mutableListOf<String>()

        try {
            normalizedUris.forEachIndexed { index, uri ->
                val sourceName = resolveSourceName(uri = uri, fallbackIndex = index)
                val extension = sourceName
                    .substringAfterLast('.', missingDelimiterValue = "")
                    .lowercase(Locale.ROOT)
                when (extension) {
                    PARAM_FILE_EXTENSION,
                    BIN_FILE_EXTENSION -> collectSingleModelFileCandidate(
                        sourceUri = uri,
                        sourceName = sourceName,
                        extension = extension,
                        stagingRoot = stagingRoot,
                        candidates = candidates,
                        invalidItems = invalidItems
                    )

                    ZIP_FILE_EXTENSION -> collectZipModelCandidates(
                        sourceUri = uri,
                        sourceName = sourceName,
                        stagingRoot = stagingRoot,
                        candidates = candidates,
                        invalidItems = invalidItems
                    )

                    else -> {
                        invalidItems.add("$sourceName：仅支持 .param/.bin/.zip")
                    }
                }
            }

            candidates.values.forEach { candidate ->
                val paramFile = candidate.paramFile
                val binFile = candidate.binFile
                if (paramFile == null || binFile == null) {
                    invalidItems.add("${candidate.suggestedName}：模型无效，必须同时包含同名 .param 和 .bin")
                    return@forEach
                }

                val importedName = runCatching {
                    importCandidatePair(
                        suggestedName = candidate.suggestedName,
                        paramFile = paramFile,
                        binFile = binFile
                    )
                }.getOrElse { throwable ->
                    val reason = throwable.message.orEmpty().ifBlank { "unknown error" }
                    invalidItems.add("${candidate.suggestedName}：导入失败（$reason）")
                    return@forEach
                }

                importedModelNames.add(importedName)
            }

            refreshModelsInternal()
        } finally {
            stagingRoot.deleteRecursively()
        }

        if (candidates.isEmpty()) {
            invalidItems.add("未发现可导入模型，模型文件必须为 .param/.bin 或包含它们的 .zip")
        }
        if (importedModelNames.isEmpty() && invalidItems.isEmpty()) {
            invalidItems.add("模型无效，必须同时包含同名 .param 和 .bin")
        }

        if (importedModelNames.isNotEmpty()) {
            logRepository.info(
                "ModelRepository",
                "Imported ${importedModelNames.size} model(s): ${importedModelNames.joinToString()}"
            )
        }
        invalidItems.distinct().forEach { invalidMessage ->
            logRepository.warning("ModelRepository", invalidMessage)
        }

        ModelImportResult(
            importedModelNames = importedModelNames,
            invalidItems = invalidItems.distinct()
        )
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
        val discoveredModels = discoverModels()
        _models.value = discoveredModels.sortedBy { model -> model.name.lowercase(Locale.ROOT) }
        if (discoveredModels.isEmpty()) {
            logRepository.warning(
                "ModelRepository",
                "No valid model pair found under ${modelsDirectory.absolutePath}"
            )
        }
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

        val paramCopied = ensureBundledAssetPresent(
            assetPath = paramAssetPath,
            destination = File(modelsDirectory, DEFAULT_MODEL_PARAM_FILE)
        )
        val binCopied = ensureBundledAssetPresent(
            assetPath = binAssetPath,
            destination = File(modelsDirectory, DEFAULT_MODEL_BIN_FILE)
        )
        return paramCopied && binCopied
    }

    private fun ensureBundledAssetPresent(
        assetPath: String,
        destination: File
    ): Boolean {
        if (destination.isFile && destination.length() > 0L) {
            return true
        }
        return copyAssetToDestination(assetPath = assetPath, destination = destination)
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

    private fun resolveSourceName(uri: Uri, fallbackIndex: Int): String {
        if (uri.scheme.equals("content", ignoreCase = true)) {
            val projection = arrayOf(OpenableColumns.DISPLAY_NAME)
            val displayName = runCatching {
                contentResolver.query(uri, projection, null, null, null)?.use { cursor ->
                    val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (index >= 0 && cursor.moveToFirst()) {
                        cursor.getString(index)
                    } else {
                        null
                    }
                }
            }.getOrNull()
            if (!displayName.isNullOrBlank()) {
                return displayName
            }
        }

        val pathName = uri.lastPathSegment
            ?.substringAfterLast('/')
            ?.trim()
            ?.ifBlank { null }
        return pathName ?: "model_source_${fallbackIndex + 1}"
    }

    private fun collectSingleModelFileCandidate(
        sourceUri: Uri,
        sourceName: String,
        extension: String,
        stagingRoot: File,
        candidates: MutableMap<String, MutableModelImportCandidate>,
        invalidItems: MutableList<String>
    ) {
        val sourceInputStream = contentResolver.openInputStream(sourceUri)
        if (sourceInputStream == null) {
            invalidItems.add("$sourceName：无法读取文件")
            return
        }

        val rawBaseName = sourceName.substringBeforeLast('.')
        val suggestedName = sanitizeModelName(rawBaseName)
        val logicalKey = "direct/${suggestedName.lowercase(Locale.ROOT)}"
        val stagingFile = File(
            stagingRoot,
            "direct_${candidates.size}_${sanitizeFileToken(rawBaseName)}.$extension"
        )

        runCatching {
            sourceInputStream.use { input ->
                stagingFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        }.onFailure { throwable ->
            invalidItems.add("$sourceName：读取失败（${throwable.message}）")
            return
        }

        addCandidateFile(
            candidates = candidates,
            logicalKey = logicalKey,
            suggestedName = suggestedName,
            sourceLabel = sourceName,
            extension = extension,
            stagingFile = stagingFile,
            invalidItems = invalidItems
        )
    }

    private fun collectZipModelCandidates(
        sourceUri: Uri,
        sourceName: String,
        stagingRoot: File,
        candidates: MutableMap<String, MutableModelImportCandidate>,
        invalidItems: MutableList<String>
    ) {
        val sourceInputStream = contentResolver.openInputStream(sourceUri)
        if (sourceInputStream == null) {
            invalidItems.add("$sourceName：无法读取 zip 文件")
            return
        }

        var matchedEntryCount = 0
        runCatching {
            sourceInputStream.use { input ->
                ZipInputStream(BufferedInputStream(input)).use { zipInput ->
                    var entry = zipInput.nextEntry
                    while (entry != null) {
                        if (!entry.isDirectory) {
                            val normalizedEntryPath = entry.name
                                .replace('\\', '/')
                                .trim()
                                .trim('/')
                            val extension = normalizedEntryPath
                                .substringAfterLast('.', missingDelimiterValue = "")
                                .lowercase(Locale.ROOT)
                            if (extension == PARAM_FILE_EXTENSION || extension == BIN_FILE_EXTENSION) {
                                val logicalKey = normalizedEntryPath
                                    .substringBeforeLast('.', missingDelimiterValue = "")
                                    .lowercase(Locale.ROOT)
                                    .ifBlank { "zip/${candidates.size}" }
                                val rawBaseName = normalizedEntryPath
                                    .substringAfterLast('/')
                                    .substringBeforeLast('.', missingDelimiterValue = "model")
                                val suggestedName = sanitizeModelName(rawBaseName)
                                val stagingFile = File(
                                    stagingRoot,
                                    "zip_${candidates.size}_${sanitizeFileToken(rawBaseName)}.$extension"
                                )

                                stagingFile.outputStream().use { output ->
                                    zipInput.copyTo(output)
                                }

                                addCandidateFile(
                                    candidates = candidates,
                                    logicalKey = logicalKey,
                                    suggestedName = suggestedName,
                                    sourceLabel = "$sourceName:$normalizedEntryPath",
                                    extension = extension,
                                    stagingFile = stagingFile,
                                    invalidItems = invalidItems
                                )
                                matchedEntryCount += 1
                            }
                        }
                        zipInput.closeEntry()
                        entry = zipInput.nextEntry
                    }
                }
            }
        }.onFailure { throwable ->
            invalidItems.add("$sourceName：解压失败（${throwable.message}）")
            return
        }

        if (matchedEntryCount <= 0) {
            invalidItems.add("$sourceName：zip 中未找到 .param/.bin 模型文件")
        }
    }

    private fun addCandidateFile(
        candidates: MutableMap<String, MutableModelImportCandidate>,
        logicalKey: String,
        suggestedName: String,
        sourceLabel: String,
        extension: String,
        stagingFile: File,
        invalidItems: MutableList<String>
    ) {
        val candidate = candidates.getOrPut(logicalKey) {
            MutableModelImportCandidate(
                suggestedName = suggestedName,
                sourceLabel = sourceLabel
            )
        }

        if (extension == PARAM_FILE_EXTENSION) {
            if (candidate.paramFile != null) {
                invalidItems.add("${candidate.suggestedName}：检测到重复 .param，仅保留首个")
                return
            }
            candidate.paramFile = stagingFile
            return
        }

        if (extension == BIN_FILE_EXTENSION) {
            if (candidate.binFile != null) {
                invalidItems.add("${candidate.suggestedName}：检测到重复 .bin，仅保留首个")
                return
            }
            candidate.binFile = stagingFile
            return
        }

        invalidItems.add("$sourceLabel：无效的模型文件扩展名")
    }

    private fun importCandidatePair(
        suggestedName: String,
        paramFile: File,
        binFile: File
    ): String {
        val sanitizedName = sanitizeModelName(suggestedName)
        val uniqueName = resolveUniqueModelName(sanitizedName)
        val targetDirectory = File(modelsDirectory, uniqueName)
        if (!targetDirectory.exists() && !targetDirectory.mkdirs()) {
            throw IOException("无法创建模型目录：${targetDirectory.absolutePath}")
        }

        val targetParam = File(targetDirectory, "$uniqueName.$PARAM_FILE_EXTENSION")
        val targetBin = File(targetDirectory, "$uniqueName.$BIN_FILE_EXTENSION")
        paramFile.copyTo(targetParam, overwrite = true)
        binFile.copyTo(targetBin, overwrite = true)
        return uniqueName
    }

    private fun resolveUniqueModelName(baseName: String): String {
        val normalizedBaseName = sanitizeModelName(baseName)
        var candidate = normalizedBaseName
        var suffix = 2
        while (hasModelNameConflict(candidate)) {
            candidate = "${normalizedBaseName}_$suffix"
            suffix += 1
        }
        return candidate
    }

    private fun hasModelNameConflict(modelName: String): Boolean {
        if (File(modelsDirectory, modelName).exists()) {
            return true
        }
        if (File(modelsDirectory, "$modelName.$PARAM_FILE_EXTENSION").exists()) {
            return true
        }
        if (File(modelsDirectory, "$modelName.$BIN_FILE_EXTENSION").exists()) {
            return true
        }
        return _models.value.any { model ->
            model.name.equals(modelName, ignoreCase = true)
        }
    }

    private fun sanitizeModelName(rawName: String): String {
        val sanitized = buildString {
            rawName.trim().forEach { char ->
                if (char.isLetterOrDigit() || char == '_' || char == '-') {
                    append(char)
                } else {
                    append('_')
                }
            }
        }.trim('_')

        return sanitized
            .ifBlank { DEFAULT_IMPORTED_MODEL_PREFIX }
            .take(MAX_MODEL_NAME_LENGTH)
    }

    private fun sanitizeFileToken(rawName: String): String {
        return buildString {
            rawName.forEach { char ->
                if (char.isLetterOrDigit()) {
                    append(char)
                } else {
                    append('_')
                }
            }
        }.ifBlank { "file" }
    }

    private fun discoverModels(): List<ModelInfo> {
        if (!modelsDirectory.exists()) {
            return emptyList()
        }

        val discoveredModels = mutableListOf<ModelInfo>()
        val usedModelNames = mutableSetOf<String>()
        val paramFiles = modelsDirectory.walkTopDown()
            .filter { file ->
                file.isFile && file.extension.equals(PARAM_FILE_EXTENSION, ignoreCase = true)
            }
            .toList()

        paramFiles.forEach { paramFile ->
            val binFile = findMatchingBinFile(paramFile)
            if (binFile == null) {
                logRepository.warning(
                    "ModelRepository",
                    "Skip invalid model file without matching .bin: ${paramFile.absolutePath}"
                )
                return@forEach
            }

            val displayName = buildDisplayModelName(paramFile)
            val uniqueName = ensureUniqueDiscoveredName(displayName, usedModelNames)
            discoveredModels += toModelInfo(uniqueName, paramFile, binFile)
        }

        return discoveredModels
    }

    private fun findMatchingBinFile(paramFile: File): File? {
        val parent = paramFile.parentFile ?: return null
        val paramNameWithoutExtension = paramFile.nameWithoutExtension
        return parent.listFiles()?.firstOrNull { file ->
            file.isFile &&
                file.extension.equals(BIN_FILE_EXTENSION, ignoreCase = true) &&
                file.nameWithoutExtension.equals(paramNameWithoutExtension, ignoreCase = true)
        }
    }

    private fun buildDisplayModelName(paramFile: File): String {
        val normalizedName = sanitizeModelName(paramFile.nameWithoutExtension)
        return if (normalizedName.isBlank()) {
            DEFAULT_IMPORTED_MODEL_PREFIX
        } else {
            normalizedName
        }
    }

    private fun ensureUniqueDiscoveredName(
        modelName: String,
        usedModelNames: MutableSet<String>
    ): String {
        var candidate = modelName
        var suffix = 2
        while (usedModelNames.any { existing -> existing.equals(candidate, ignoreCase = true) }) {
            candidate = "${modelName}_$suffix"
            suffix += 1
        }
        usedModelNames.add(candidate)
        return candidate
    }

    private fun cleanupEmptyParentDirectories(directory: File?) {
        var current = directory
        while (current != null && current != modelsDirectory) {
            val files = current.listFiles()
            if (files != null && files.isNotEmpty()) {
                break
            }
            val deleted = runCatching { current.delete() }.getOrDefault(false)
            if (!deleted) {
                break
            }
            current = current.parentFile
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

    private data class MutableModelImportCandidate(
        val suggestedName: String,
        val sourceLabel: String,
        var paramFile: File? = null,
        var binFile: File? = null
    )

    companion object {
        private const val DEFAULT_MODEL_NAME = "Real-ESRGANv3-anime_x2"
        private const val DEFAULT_MODEL_PARAM_FILE = "$DEFAULT_MODEL_NAME.param"
        private const val DEFAULT_MODEL_BIN_FILE = "$DEFAULT_MODEL_NAME.bin"
        private const val DEFAULT_IMPORTED_MODEL_PREFIX = "imported_model"
        private const val MAX_MODEL_NAME_LENGTH = 80
        private const val PARAM_FILE_EXTENSION = "param"
        private const val BIN_FILE_EXTENSION = "bin"
        private const val ZIP_FILE_EXTENSION = "zip"
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
