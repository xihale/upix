package com.crossscale.data.repository

import android.content.Context
import android.content.ContentValues
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.provider.DocumentsContract
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.security.MessageDigest

data class BatchOutputDirectory(
    val batchId: String,
    val displayPath: String,
    val outputDirectoryUri: String? = null,
    val internalDirectory: File? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true
)

class BatchOutputRepository(
    context: Context
) : BatchOutputStore {
    private val appContext = context.applicationContext
    private val contentResolver = appContext.contentResolver
    private val publicRootOutputDir = File(
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
        OUTPUT_ROOT_DIR
    )
    private val fallbackRootOutputDir = appContext.getExternalFilesDir(Environment.DIRECTORY_PICTURES)
        ?.let { directory -> File(directory, OUTPUT_ROOT_DIR) }
        ?: File(appContext.filesDir, OUTPUT_ROOT_DIR)
    private val tempOutputRootDir = File(appContext.cacheDir, TEMP_OUTPUT_ROOT_DIR)

    override fun prepareOutputDirectory(
        source: String,
        modelName: String,
        preferredTreeUri: String?,
        overwriteOriginal: Boolean,
        backupOriginal: Boolean,
        resumeOutputDirectory: String?,
        resumeOutputDirectoryUri: String?,
        resumeOverwriteOriginal: Boolean?,
        resumeBackupOriginal: Boolean?
    ): BatchOutputDirectory {
        val batchId = batchIdFor(source = source, modelName = modelName)
        val resolvedOverwrite = resumeOverwriteOriginal ?: overwriteOriginal
        val resolvedBackup = if (resolvedOverwrite) (resumeBackupOriginal ?: backupOriginal) else false
        val normalizedResumeUri = resumeOutputDirectoryUri.orEmpty().trim()
        if (normalizedResumeUri.isNotBlank()) {
            val parsedResumeUri = parseUri(normalizedResumeUri)
            if (parsedResumeUri != null && !parsedResumeUri.scheme.isNullOrBlank()) {
                val displayPath = resumeOutputDirectory?.takeIf { it.isNotBlank() } ?: parsedResumeUri.toString()
                return BatchOutputDirectory(
                    batchId = batchId,
                    displayPath = displayPath,
                    outputDirectoryUri = parsedResumeUri.toString(),
                    internalDirectory = null,
                    overwriteOriginal = resolvedOverwrite,
                    backupOriginal = resolvedBackup
                )
            }
        }

        val normalizedResumeDirectory = resumeOutputDirectory.orEmpty().trim()
        if (normalizedResumeDirectory.startsWith(CONTENT_SCHEME_PREFIX, ignoreCase = true)) {
            val parsedResumeDirectoryUri = parseUri(normalizedResumeDirectory)
            if (parsedResumeDirectoryUri != null && !parsedResumeDirectoryUri.scheme.isNullOrBlank()) {
                return BatchOutputDirectory(
                    batchId = batchId,
                    displayPath = normalizedResumeDirectory,
                    outputDirectoryUri = parsedResumeDirectoryUri.toString(),
                    internalDirectory = null,
                    overwriteOriginal = resolvedOverwrite,
                    backupOriginal = resolvedBackup
                )
            }
        }

        val preferredUri = parseUri(preferredTreeUri.orEmpty().trim())
        if (preferredUri != null) {
            val folderName = "crossscale_$batchId"
            val outputDirectoryUri = ensureSubDirectory(
                rootTreeUri = preferredUri,
                directoryName = folderName
            )
            if (outputDirectoryUri != null) {
                return BatchOutputDirectory(
                    batchId = batchId,
                    displayPath = outputDirectoryUri.toString(),
                    outputDirectoryUri = outputDirectoryUri.toString(),
                    internalDirectory = null,
                    overwriteOriginal = resolvedOverwrite,
                    backupOriginal = resolvedBackup
                )
            }
        }

        val internalDirectory = when {
            normalizedResumeDirectory.isBlank() -> File(defaultOutputRootDir(), batchId)
            else -> File(normalizedResumeDirectory)
        }
        if (!internalDirectory.exists()) {
            internalDirectory.mkdirs()
        }
        return BatchOutputDirectory(
            batchId = batchId,
            displayPath = internalDirectory.absolutePath,
            outputDirectoryUri = null,
            internalDirectory = internalDirectory,
            overwriteOriginal = resolvedOverwrite,
            backupOriginal = resolvedBackup
        )
    }

    override fun outputFileFor(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ): File {
        val outputName = outputNameFor(input)
        val tempDir = File(tempOutputRootDir, outputDirectory.batchId)
        if (!tempDir.exists()) {
            tempDir.mkdirs()
        }
        return File(tempDir, outputName)
    }

    override fun isOutputReady(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ): Boolean {
        val outputName = outputNameFor(input)
        val outputDirectoryUri = outputDirectory.outputDirectoryUri.orEmpty()
        if (outputDirectoryUri.isBlank()) {
            val internalDirectory = outputDirectory.internalDirectory ?: return false
            return File(internalDirectory, outputName).exists()
        }
        val directoryUri = parseUri(outputDirectoryUri) ?: return false
        return findChildDocumentUri(
            parentDocumentUri = directoryUri,
            displayName = outputName,
            expectDirectory = false
        ) != null
    }

    override fun firstReadyOutputUri(
        outputDirectory: BatchOutputDirectory,
        inputs: List<ResolvedImageInput>
    ): String? {
        if (inputs.isEmpty()) {
            return null
        }

        val outputDirectoryUri = outputDirectory.outputDirectoryUri.orEmpty()
        if (outputDirectoryUri.isBlank()) {
            val internalDirectory = outputDirectory.internalDirectory ?: return null
            inputs.forEach { input ->
                val outputFile = File(internalDirectory, outputNameFor(input))
                if (outputFile.exists()) {
                    return outputFile.absolutePath
                }
            }
            return null
        }

        val parsedDirectoryUri = parseUri(outputDirectoryUri) ?: return null
        inputs.forEach { input ->
            val outputUri = findChildDocumentUri(
                parentDocumentUri = parsedDirectoryUri,
                displayName = outputNameFor(input),
                expectDirectory = false
            )
            if (outputUri != null) {
                return outputUri.toString()
            }
        }
        return null
    }

    override fun commitOutput(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput,
        outputFile: File
    ): Result<String> {
        if (outputDirectory.overwriteOriginal) {
            return commitOverwriteOriginal(outputDirectory = outputDirectory, input = input, outputFile = outputFile)
        }
        val outputName = outputNameFor(input)
        val outputDirectoryUri = outputDirectory.outputDirectoryUri.orEmpty()
        if (outputDirectoryUri.isBlank()) {
            return commitToFilesystem(
                outputDirectory = outputDirectory,
                outputName = outputName,
                outputFile = outputFile
            )
        }
        val parsedDirectoryUri = parseUri(outputDirectoryUri)
            ?: return Result.failure(IllegalArgumentException("Invalid output URI: $outputDirectoryUri"))
        return runCatching {
            val outputDocumentUri = findOrCreateChildDocumentUri(
                parentDocumentUri = parsedDirectoryUri,
                displayName = outputName,
                mimeType = mimeTypeForOutputName(outputName),
                expectDirectory = false
            )

            openDocumentOutputStream(outputDocumentUri).use { outputStream ->
                outputFile.inputStream().use { inputStream ->
                    inputStream.copyTo(outputStream)
                }
            }

            outputFile.delete()
            outputDocumentUri.toString()
        }
    }

    private fun commitToFilesystem(
        outputDirectory: BatchOutputDirectory,
        outputName: String,
        outputFile: File
    ): Result<String> {
        return runCatching {
            val targetDirectory = requireNotNull(outputDirectory.internalDirectory) {
                "Internal output directory is missing for ${outputDirectory.batchId}"
            }

            if (targetDirectory.exists() && !targetDirectory.isDirectory) {
                throw IOException("Output parent is not directory: ${targetDirectory.absolutePath}")
            }
            if (!targetDirectory.exists() && !targetDirectory.mkdirs()) {
                throw IOException("Failed to create output directory: ${targetDirectory.absolutePath}")
            }

            val targetFile = File(targetDirectory, outputName)
            if (targetFile.exists() && targetFile.isDirectory) {
                if (!targetFile.deleteRecursively()) {
                    throw IOException("Output target is directory and cannot be removed: ${targetFile.absolutePath}")
                }
            }

            val useMediaStore = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
                targetDirectory.absolutePath.startsWith(publicRootOutputDir.absolutePath)
            if (useMediaStore) {
                val mediaStoreUri = commitToMediaStore(
                    batchId = outputDirectory.batchId,
                    outputName = outputName,
                    outputFile = outputFile
                )
                outputFile.delete()
                mediaStoreUri.toString()
            } else {
                writeTempToTarget(outputFile = outputFile, targetFile = targetFile)
                outputFile.delete()
                notifyMediaScanner(targetFile)
                targetFile.absolutePath
            }
        }
    }

    private fun commitToMediaStore(
        batchId: String,
        outputName: String,
        outputFile: File
    ): Uri {
        val relativePath = "${Environment.DIRECTORY_PICTURES}/$OUTPUT_ROOT_DIR/$batchId/"
        var lastError: Throwable? = null
        for (attempt in 0..MAX_CREATE_DOCUMENT_RETRIES) {
            val candidateName = if (attempt == 0) outputName else outputNameWithRetrySuffix(outputName, attempt)
            val insertedUri = runCatching {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, candidateName)
                    put(MediaStore.Images.Media.MIME_TYPE, mimeTypeForOutputName(candidateName))
                    put(MediaStore.Images.Media.RELATIVE_PATH, relativePath)
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            }.onFailure { throwable ->
                lastError = throwable
            }.getOrNull() ?: continue

            try {
                contentResolver.openOutputStream(insertedUri, "w")?.use { outputStream ->
                    outputFile.inputStream().use { inputStream ->
                        inputStream.copyTo(outputStream)
                    }
                } ?: throw IOException("Unable to open MediaStore output stream: $insertedUri")

                val publishValues = ContentValues().apply {
                    put(MediaStore.Images.Media.IS_PENDING, 0)
                }
                contentResolver.update(insertedUri, publishValues, null, null)
                return insertedUri
            } catch (throwable: Throwable) {
                lastError = throwable
                runCatching {
                    contentResolver.delete(insertedUri, null, null)
                }
            }
        }
        throw IOException("Failed to commit output via MediaStore: $outputName", lastError)
    }

    private fun notifyMediaScanner(file: File) {
        runCatching {
            MediaScannerConnection.scanFile(
                appContext,
                arrayOf(file.absolutePath),
                null,
                null
            )
        }
    }

    private fun writeTempToTarget(
        outputFile: File,
        targetFile: File
    ) {
        try {
            outputFile.inputStream().use { inputStream ->
                FileOutputStream(targetFile).use { outputStream ->
                    inputStream.copyTo(outputStream)
                    outputStream.fd.sync()
                }
            }
        } catch (throwable: Throwable) {
            val isExistsConflict = throwable is IOException &&
                (throwable.message?.contains("EEXIST", ignoreCase = true) == true ||
                    throwable.message?.contains("File exists", ignoreCase = true) == true)
            if (!isExistsConflict) {
                throw throwable
            }

            if (targetFile.exists() && !targetFile.delete()) {
                targetFile.deleteRecursively()
            }

            outputFile.inputStream().use { inputStream ->
                FileOutputStream(targetFile).use { outputStream ->
                    inputStream.copyTo(outputStream)
                    outputStream.fd.sync()
                }
            }
        }
    }

    override fun discardOutputFile(outputFile: File) {
        if (outputFile.exists()) {
            outputFile.delete()
        }
    }

    private fun batchIdFor(
        source: String,
        modelName: String
    ): String {
        return "batch_${shortHash("$modelName|$source")}"
    }

    private fun shortHash(value: String): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(value.toByteArray(Charsets.UTF_8))
        return digest.take(8).joinToString("") { byte -> "%02x".format(byte) }
    }

    private fun outputNameFor(input: ResolvedImageInput): String {
        val baseName = input.displayName.substringBeforeLast('.', input.displayName)
            .replace(Regex("[^A-Za-z0-9._-]"), "_")
            .take(64)
            .ifBlank { "image" }
        val uniqueSuffix = shortHash(input.uri.toString())
        val extension = normalizedOutputExtension(input.displayName)
        return "${baseName}_${uniqueSuffix}.$extension"
    }

    private fun normalizedOutputExtension(displayName: String): String {
        return when (displayName.substringAfterLast('.', "").lowercase()) {
            "jpg" -> "jpg"
            "jpeg" -> "jpeg"
            "webp" -> "webp"
            "png" -> "png"
            else -> "png"
        }
    }

    private fun mimeTypeForOutputName(outputName: String): String {
        return when (outputName.substringAfterLast('.', "").lowercase()) {
            "jpg", "jpeg" -> JPEG_MIME_TYPE
            "webp" -> WEBP_MIME_TYPE
            else -> PNG_MIME_TYPE
        }
    }

    private fun commitOverwriteOriginal(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput,
        outputFile: File
    ): Result<String> {
        val inputUri = input.uri
        return runCatching {
            if (outputDirectory.backupOriginal) {
                backupOriginalInput(outputDirectory = outputDirectory, input = input)
            }
            if (inputUri.scheme.equals("file", ignoreCase = true)) {
                val targetFile = File(requireNotNull(inputUri.path) { "Invalid file uri: $inputUri" })
                outputFile.copyTo(targetFile, overwrite = true)
                outputFile.delete()
                targetFile.absolutePath
            } else {
                contentResolver.openOutputStream(inputUri, "w")?.use { outputStream ->
                    outputFile.inputStream().use { inputStream ->
                        inputStream.copyTo(outputStream)
                    }
                } ?: throw IOException("Unable to overwrite input URI: $inputUri")
                outputFile.delete()
                inputUri.toString()
            }
        }
    }

    private fun backupOriginalInput(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ) {
        val backupOutputRootDir = File(defaultOutputRootDir(), BACKUP_ROOT_DIR)
        val backupDir = File(backupOutputRootDir, outputDirectory.batchId)
        if (!backupDir.exists()) {
            backupDir.mkdirs()
        }
        val backupFile = File(backupDir, input.displayName)
        if (backupFile.exists()) {
            return
        }

        if (input.uri.scheme.equals("file", ignoreCase = true)) {
            val sourceFile = File(requireNotNull(input.uri.path) { "Invalid file uri: ${input.uri}" })
            sourceFile.copyTo(backupFile, overwrite = false)
        } else {
            contentResolver.openInputStream(input.uri)?.use { source ->
                backupFile.outputStream().use { sink ->
                    source.copyTo(sink)
                }
            }
        }
    }

    private fun ensureSubDirectory(
        rootTreeUri: Uri,
        directoryName: String
    ): Uri? {
        return runCatching {
            val rootDocumentId = DocumentsContract.getTreeDocumentId(rootTreeUri)
            val rootDocumentUri = DocumentsContract.buildDocumentUriUsingTree(rootTreeUri, rootDocumentId)
            findOrCreateChildDocumentUri(
                parentDocumentUri = rootDocumentUri,
                displayName = directoryName,
                mimeType = DocumentsContract.Document.MIME_TYPE_DIR,
                expectDirectory = true,
                allowAlternateNameOnConflict = false
            )
        }.getOrNull()
    }

    private fun findOrCreateChildDocumentUri(
        parentDocumentUri: Uri,
        displayName: String,
        mimeType: String,
        expectDirectory: Boolean,
        allowAlternateNameOnConflict: Boolean = true
    ): Uri {
        findChildDocumentUri(
            parentDocumentUri = parentDocumentUri,
            displayName = displayName,
            expectDirectory = expectDirectory
        )?.let { existingUri ->
            return existingUri
        }

        var createFailure: Throwable? = null
        val createdUri = runCatching {
            DocumentsContract.createDocument(
                contentResolver,
                parentDocumentUri,
                mimeType,
                displayName
            )
        }.onFailure { throwable ->
            createFailure = throwable
        }.getOrNull()

        if (createdUri != null) {
            return createdUri
        }

        findChildDocumentUri(
            parentDocumentUri = parentDocumentUri,
            displayName = displayName,
            expectDirectory = expectDirectory
        )?.let { existingUri ->
            return existingUri
        }

        val conflictByName = createFailure?.message?.let { message ->
            message.contains("EEXIST", ignoreCase = true) ||
                message.contains("exists", ignoreCase = true)
        } == true
        if (allowAlternateNameOnConflict && !expectDirectory && conflictByName) {
            for (attempt in 1..MAX_CREATE_DOCUMENT_RETRIES) {
                val candidateName = outputNameWithRetrySuffix(displayName, attempt)
                findChildDocumentUri(
                    parentDocumentUri = parentDocumentUri,
                    displayName = candidateName,
                    expectDirectory = false
                )?.let { existingUri ->
                    return existingUri
                }
                val candidateUri = runCatching {
                    DocumentsContract.createDocument(
                        contentResolver,
                        parentDocumentUri,
                        mimeType,
                        candidateName
                    )
                }.getOrNull()
                if (candidateUri != null) {
                    return candidateUri
                }
            }
        }

        val messageSuffix = createFailure?.message?.let { message -> ": $message" }.orEmpty()
        throw IOException("Failed to create output document: $displayName$messageSuffix", createFailure)
    }

    private fun outputNameWithRetrySuffix(
        outputName: String,
        attempt: Int
    ): String {
        val dotIndex = outputName.lastIndexOf('.')
        if (dotIndex <= 0 || dotIndex == outputName.lastIndex) {
            return "${outputName}_$attempt"
        }
        val base = outputName.substring(0, dotIndex)
        val extension = outputName.substring(dotIndex)
        return "${base}_$attempt$extension"
    }

    private fun openDocumentOutputStream(documentUri: Uri): OutputStream {
        val modes = arrayOf("rwt", "wt", "w")
        var lastError: Throwable? = null
        for (mode in modes) {
            val stream = runCatching {
                contentResolver.openOutputStream(documentUri, mode)
            }.onFailure { throwable ->
                lastError = throwable
            }.getOrNull()
            if (stream != null) {
                return stream
            }
        }
        throw IOException("Unable to open output stream: $documentUri", lastError)
    }

    private fun findChildDocumentUri(
        parentDocumentUri: Uri,
        displayName: String,
        expectDirectory: Boolean
    ): Uri? {
        return runCatching {
            val parentDocumentId = DocumentsContract.getDocumentId(parentDocumentUri)
            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                parentDocumentUri,
                parentDocumentId
            )
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE
            )

            contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
                val idIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                val typeIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
                while (cursor.moveToNext()) {
                    if (idIndex == -1 || nameIndex == -1 || typeIndex == -1) {
                        continue
                    }
                    val childId = cursor.getString(idIndex) ?: continue
                    val childName = cursor.getString(nameIndex) ?: continue
                    val childMime = cursor.getString(typeIndex)
                    if (childName != displayName) {
                        continue
                    }
                    val isDirectory = childMime == DocumentsContract.Document.MIME_TYPE_DIR
                    if (expectDirectory != isDirectory) {
                        continue
                    }
                    return DocumentsContract.buildDocumentUriUsingTree(parentDocumentUri, childId)
                }
                null
            }
        }.getOrNull()
    }

    private fun parseUri(rawValue: String): Uri? {
        if (rawValue.isBlank()) {
            return null
        }
        return runCatching { Uri.parse(rawValue) }.getOrNull()
    }

    private fun defaultOutputRootDir(): File {
        val publicReady = runCatching {
            publicRootOutputDir.exists() || publicRootOutputDir.mkdirs()
        }.getOrDefault(false)
        if (publicReady) {
            return publicRootOutputDir
        }

        val fallbackReady = runCatching {
            fallbackRootOutputDir.exists() || fallbackRootOutputDir.mkdirs()
        }.getOrDefault(false)
        if (fallbackReady) {
            return fallbackRootOutputDir
        }

        return publicRootOutputDir
    }

    companion object {
        private const val OUTPUT_ROOT_DIR = "CrossScale"
        private const val BACKUP_ROOT_DIR = "original_backups"
        private const val TEMP_OUTPUT_ROOT_DIR = "outputs_temp"
        private const val PNG_MIME_TYPE = "image/png"
        private const val JPEG_MIME_TYPE = "image/jpeg"
        private const val WEBP_MIME_TYPE = "image/webp"
        private const val CONTENT_SCHEME_PREFIX = "content://"
        private const val MAX_CREATE_DOCUMENT_RETRIES = 3
    }
}
