package com.crossscale.data.repository

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import com.crossscale.R
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.ModelImportResult
import java.io.BufferedInputStream
import java.io.File
import java.io.IOException
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
    override val models: StateFlow<List<ModelInfo>> = _models.asStateFlow()

    override suspend fun refreshModels() = withContext(Dispatchers.IO) {
        ensureModelDirectory()
        refreshModelsInternal()
    }

    override suspend fun deleteModel(modelInfo: ModelInfo) = withContext(Dispatchers.IO) {
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

    override suspend fun importModels(inputUris: List<Uri>): ModelImportResult = withContext(Dispatchers.IO) {
        ensureModelDirectory()

        val normalizedUris = inputUris.distinct()
        if (normalizedUris.isEmpty()) {
            return@withContext ModelImportResult(
                invalidItems = listOf(
                    context.getString(R.string.models_import_error_no_files)
                )
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
                invalidItems = listOf(
                    context.getString(R.string.models_import_error_staging_dir)
                )
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
                        invalidItems.add(
                            context.getString(R.string.models_import_error_invalid_extension, sourceName)
                        )
                    }
                }
            }

            candidates.values.forEach { candidate ->
                val paramFile = candidate.paramFile
                val binFile = candidate.binFile
                if (paramFile == null || binFile == null) {
                    invalidItems.add(
                        context.getString(
                            R.string.models_import_error_invalid_pair,
                            candidate.suggestedName
                        )
                    )
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
                    invalidItems.add(
                        context.getString(
                            R.string.models_import_error_import_failed,
                            candidate.suggestedName,
                            reason
                        )
                    )
                    return@forEach
                }

                importedModelNames.add(importedName)
            }

            refreshModelsInternal()
        } finally {
            stagingRoot.deleteRecursively()
        }

        if (candidates.isEmpty()) {
            invalidItems.add(
                context.getString(R.string.models_import_error_no_models_found)
            )
        }
        if (importedModelNames.isEmpty() && invalidItems.isEmpty()) {
            invalidItems.add(
                context.getString(R.string.models_import_error_invalid_pair_generic)
            )
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

    override fun modelDirectoryPath(): String {
        return modelsDirectory.absolutePath
    }

    /** 远程下载的落盘目录（与发现扫描的根目录一致）。 */
    fun modelsDirectory(): File = modelsDirectory

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
            invalidItems.add(
                context.getString(R.string.models_import_error_unreadable, sourceName)
            )
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
            invalidItems.add(
                context.getString(
                    R.string.models_import_error_read_failed,
                    sourceName,
                    throwable.message.orEmpty()
                )
            )
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
            invalidItems.add(
                context.getString(R.string.models_import_error_zip_unreadable, sourceName)
            )
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
                            // Security fix: Validate against Zip Slip vulnerability.
                            // Reject entries with path traversal sequences ("..").
                            if (normalizedEntryPath.contains("..")) {
                                invalidItems.add(
                                    context.getString(
                                        R.string.models_import_error_zip_illegal_entry,
                                        sourceName,
                                        normalizedEntryPath
                                    )
                                )
                                zipInput.closeEntry()
                                entry = zipInput.nextEntry
                                continue
                            }
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
            invalidItems.add(
                context.getString(
                    R.string.models_import_error_unzip_failed,
                    sourceName,
                    throwable.message.orEmpty()
                )
            )
            return
        }

        if (matchedEntryCount <= 0) {
            invalidItems.add(
                context.getString(R.string.models_import_error_zip_no_models, sourceName)
            )
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
                invalidItems.add(
                    context.getString(
                        R.string.models_import_error_duplicate_param,
                        candidate.suggestedName
                    )
                )
                return
            }
            candidate.paramFile = stagingFile
            return
        }

        if (extension == BIN_FILE_EXTENSION) {
            if (candidate.binFile != null) {
                invalidItems.add(
                    context.getString(
                        R.string.models_import_error_duplicate_bin,
                        candidate.suggestedName
                    )
                )
                return
            }
            candidate.binFile = stagingFile
            return
        }

        invalidItems.add(
            context.getString(R.string.models_import_error_invalid_extension, sourceLabel)
        )
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
            throw IOException(
                context.getString(
                    R.string.models_import_error_mkdir_failed,
                    targetDirectory.absolutePath
                )
            )
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
        val sanitized = rawName.trim()
            .map { char ->
                if (char.isLetterOrDigit() || char == '_' || char == '-') char else '_'
            }
            .joinToString("")
            .trim('_')

        return sanitized
            .ifBlank { DEFAULT_IMPORTED_MODEL_PREFIX }
            .take(MAX_MODEL_NAME_LENGTH)
    }

    private fun sanitizeFileToken(rawName: String): String {
        return rawName
            .map { char -> if (char.isLetterOrDigit()) char else '_' }
            .joinToString("")
            .ifBlank { "file" }
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
        return ModelInfo(
            name = modelName,
            scaleFactor = inferScaleFactor(modelName),
            totalSize = param.length() + bin.length(),
            paramPath = param.absolutePath,
            binPath = bin.absolutePath
        )
    }

    private fun inferScaleFactor(modelName: String): Int {
        val regex = Regex("x(\\d+)", RegexOption.IGNORE_CASE)
        // 兜底与引擎一致（NativeImageProcessingRepository DEFAULT_SCALE_FACTOR = 2）。
        return regex.find(modelName)?.groupValues?.getOrNull(1)?.toIntOrNull() ?: 2
    }

    private data class MutableModelImportCandidate(
        val suggestedName: String,
        val sourceLabel: String,
        var paramFile: File? = null,
        var binFile: File? = null
    )

    companion object {
        private const val DEFAULT_IMPORTED_MODEL_PREFIX = "imported_model"
        private const val MAX_MODEL_NAME_LENGTH = 80
        private const val PARAM_FILE_EXTENSION = "param"
        private const val BIN_FILE_EXTENSION = "bin"
        private const val ZIP_FILE_EXTENSION = "zip"
    }

}
