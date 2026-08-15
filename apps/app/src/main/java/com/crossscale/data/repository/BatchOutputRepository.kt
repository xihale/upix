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
    val backupOriginal: Boolean = true,
    val processNestedFolders: Boolean = true
)

class BatchOutputRepository(
    private val context: Context
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

    /**
     * 输出格式设置：由设置层（MainViewModel）写入，生成输出文件名时读取。
     * 输出名在处理时逐项生成，因此设置变更对已入队未处理的项同样生效；
     * 覆盖原图模式下输出写回原路径，该设置被忽略（见 [outputNameFor]）。
     */
    @Volatile
    var requestedOutputFormat: OutputImageFormat = OutputImageFormat.FOLLOW

    override fun prepareOutputDirectory(
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
    ): BatchOutputDirectory {
        val batchId = batchIdFor(source = source, modelName = modelName)
        val sourceFolderName = sourceFolderName(source)
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
                    backupOriginal = resolvedBackup,
                    processNestedFolders = processNestedFolders
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
                    backupOriginal = resolvedBackup,
                    processNestedFolders = processNestedFolders
                )
            }
        }

        val preferredUri = parseUri(preferredTreeUri.orEmpty().trim())
        if (preferredUri != null) {
            val outputDirectoryUri = when (sourceFolderName) {
                null -> rootDocumentUriFromTreeUri(preferredUri)
                else -> ensureSubDirectory(
                    rootTreeUri = preferredUri,
                    directoryName = sourceFolderName
                )
            }
            if (outputDirectoryUri != null) {
                return BatchOutputDirectory(
                    batchId = batchId,
                    displayPath = outputDirectoryUri.toString(),
                    outputDirectoryUri = outputDirectoryUri.toString(),
                    internalDirectory = null,
                    overwriteOriginal = resolvedOverwrite,
                    backupOriginal = resolvedBackup,
                    processNestedFolders = processNestedFolders
                )
            }
        }

        val baseInternalDirectory = when {
            normalizedResumeDirectory.isBlank() -> defaultOutputRootDir()
            else -> File(normalizedResumeDirectory)
        }
        val internalDirectory = when (sourceFolderName) {
            null -> baseInternalDirectory
            else -> File(baseInternalDirectory, sourceFolderName)
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
            backupOriginal = resolvedBackup,
            processNestedFolders = processNestedFolders
        )
    }

    override fun outputFileFor(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ): File {
        val outputName = outputNameFor(outputDirectory, input)
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
        if (outputDirectory.overwriteOriginal) {
            return false
        }
        val outputName = outputNameFor(outputDirectory, input)
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
        if (outputDirectory.overwriteOriginal) {
            inputs.forEach { input ->
                if (hasBackupForInput(input)) {
                    return input.uri.toString()
                }
            }
            return null
        }

        val outputDirectoryUri = outputDirectory.outputDirectoryUri.orEmpty()
        if (outputDirectoryUri.isBlank()) {
            val internalDirectory = outputDirectory.internalDirectory ?: return null
            inputs.forEach { input ->
                val outputFile = File(internalDirectory, outputNameFor(outputDirectory, input))
                if (outputFile.exists()) {
                    return Uri.fromFile(outputFile).toString()
                }
            }
            return null
        }

        val parsedDirectoryUri = parseUri(outputDirectoryUri) ?: return null
        inputs.forEach { input ->
            val outputUri = findChildDocumentUri(
                parentDocumentUri = parsedDirectoryUri,
                displayName = outputNameFor(outputDirectory, input),
                expectDirectory = false
            )
            if (outputUri != null) {
                return outputUri.toString()
            }
        }
        return null
    }

    /** Where the .bak backup of [input] lives: file system or SAF documents. */
    private sealed interface BackupTarget {
        /** file:// input: original file plus its sibling .bak directory. */
        data class FileSystem(val sourceFile: File, val backupDir: File) : BackupTarget

        /** SAF document input: the parent document holding the .bak directory. */
        data class Documents(val parentUri: Uri) : BackupTarget
    }

    /**
     * Single place that derives the backup location for an input: file:// URIs
     * back up into a sibling `.bak` directory next to the original; SAF document
     * URIs back up into a `.bak` child of the parent document. Everything else
     * (simple content URIs) has no addressable backup location and returns null.
     */
    private fun backupTargetFor(input: ResolvedImageInput): BackupTarget? {
        return when {
            input.uri.scheme.equals("file", ignoreCase = true) -> {
                val sourceFile = File(
                    requireNotNull(input.uri.path) { "Invalid file uri: ${input.uri}" }
                )
                val parentFile = sourceFile.parentFile ?: return null
                BackupTarget.FileSystem(
                    sourceFile = sourceFile,
                    backupDir = File(parentFile, BACKUP_DIR_NAME)
                )
            }
            DocumentsContract.isDocumentUri(context, input.uri) -> {
                BackupTarget.Documents(
                    parentUri = findParentDocumentUri(input.uri) ?: return null
                )
            }
            else -> null
        }
    }

    private fun hasBackupForInput(input: ResolvedImageInput): Boolean {
        return when (val target = backupTargetFor(input)) {
            is BackupTarget.FileSystem -> {
                if (!target.backupDir.exists() || !target.backupDir.isDirectory) return false
                val backupFile = File(target.backupDir, input.displayName)
                backupFile.exists() && backupFile.isFile
            }
            is BackupTarget.Documents -> {
                val bakDirUri = findChildDocumentUri(
                    parentDocumentUri = target.parentUri,
                    displayName = BACKUP_DIR_NAME,
                    expectDirectory = true
                ) ?: return false
                findChildDocumentUri(
                    parentDocumentUri = bakDirUri,
                    displayName = input.displayName,
                    expectDirectory = false
                ) != null
            }
            null -> false
        }
    }

    override fun hasBackup(input: ResolvedImageInput): Boolean {
        return hasBackupForInput(input)
    }

    override fun restoreBackup(input: ResolvedImageInput): Result<Unit> {
        return runCatching {
            if (!hasBackupForInput(input)) {
                throw IOException("No backup found for ${input.displayName}")
            }

            when (val target = backupTargetFor(input)) {
                is BackupTarget.FileSystem -> {
                    val backupFile = File(target.backupDir, input.displayName)

                    // Copy backup over original
                    backupFile.copyTo(target.sourceFile, overwrite = true)
                    // Delete backup after restore
                    backupFile.delete()

                    // Cleanup .bak folder if empty
                    if (target.backupDir.list()?.isEmpty() == true) {
                        target.backupDir.delete()
                    }
                }
                is BackupTarget.Documents -> {
                    val bakDirUri = findChildDocumentUri(
                        parentDocumentUri = target.parentUri,
                        displayName = BACKUP_DIR_NAME,
                        expectDirectory = true
                    ) ?: throw IOException("No .bak dir")
                    val backupUri = findChildDocumentUri(
                        parentDocumentUri = bakDirUri,
                        displayName = input.displayName,
                        expectDirectory = false
                    ) ?: throw IOException("No backup file")

                    // Copy backup over original
                    contentResolver.openInputStream(backupUri)?.use { sourceStream ->
                        contentResolver.openOutputStream(input.uri, "wt")?.use { sinkStream ->
                            sourceStream.copyTo(sinkStream)
                        } ?: throw IOException("Failed to write to original file")
                    } ?: throw IOException("Failed to read backup file")

                    // Delete backup after restore
                    DocumentsContract.deleteDocument(contentResolver, backupUri)

                    // Directory cleanup is harder with SAF without extra queries, skip for now
                }
                null -> throw UnsupportedOperationException(
                    "Restore not supported for scheme: ${input.uri.scheme}"
                )
            }
        }
    }

    override fun commitOutput(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput,
        outputFile: File
    ): Result<String> {
        if (outputDirectory.overwriteOriginal) {
            return commitOverwriteOriginal(outputDirectory = outputDirectory, input = input, outputFile = outputFile)
        }
        return commitAsNewOutput(
            outputDirectory = outputDirectory,
            outputName = outputNameFor(outputDirectory, input),
            outputFile = outputFile
        )
    }

    /**
     * Saves [outputFile] as a new entry in the output directory: SAF document
     * target when an output URI is configured, plain filesystem otherwise.
     */
    private fun commitAsNewOutput(
        outputDirectory: BatchOutputDirectory,
        outputName: String,
        outputFile: File
    ): Result<String> {
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
        return commitToContentUri(
            parentDirectoryUri = parsedDirectoryUri,
            outputName = outputName,
            outputFile = outputFile
        )
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

            val targetFile = resolveAvailableTargetFile(
                targetDirectory = targetDirectory,
                outputName = outputName
            )
            if (targetFile.exists() && targetFile.isDirectory) {
                if (!targetFile.deleteRecursively()) {
                    throw IOException("Output target is directory and cannot be removed: ${targetFile.absolutePath}")
                }
            }

            val useMediaStore = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
                targetDirectory.absolutePath.startsWith(publicRootOutputDir.absolutePath)
            if (useMediaStore) {
                val mediaStoreUri = commitToMediaStore(
                    targetDirectory = targetDirectory,
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
        targetDirectory: File,
        outputName: String,
        outputFile: File
    ): Uri {
        val relativeSubPath = targetDirectory.absolutePath
            .removePrefix(publicRootOutputDir.absolutePath)
            .trimStart('/')
        val relativePath = buildString {
            append(Environment.DIRECTORY_PICTURES)
            append('/')
            append(OUTPUT_ROOT_DIR)
            append('/')
            if (relativeSubPath.isNotBlank()) {
                append(relativeSubPath)
                append('/')
            }
        }
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

    /**
     * Copies the staged temp file into the stream opened by [open], then
     * fsyncs (file streams) or flushes (provider streams). Shared by every
     * commit path that funnels a temp file into an output stream.
     */
    private inline fun copyThenSync(src: File, open: () -> OutputStream) {
        open().use { outputStream ->
            src.inputStream().use { inputStream ->
                inputStream.copyTo(outputStream)
            }
            if (outputStream is FileOutputStream) {
                outputStream.fd.sync()
            } else {
                outputStream.flush()
            }
        }
    }

    private fun writeTempToTarget(
        outputFile: File,
        targetFile: File
    ) {
        try {
            copyThenSync(src = outputFile) { FileOutputStream(targetFile) }
        } catch (throwable: Throwable) {
            if (!isExistsConflict(throwable)) {
                throw throwable
            }

            if (targetFile.exists() && !targetFile.delete()) {
                targetFile.deleteRecursively()
            }

            copyThenSync(src = outputFile) { FileOutputStream(targetFile) }
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

    private fun outputNameFor(outputDirectory: BatchOutputDirectory, input: ResolvedImageInput): String {
        val baseName = input.displayName.substringBeforeLast('.', input.displayName)
            .replace(Regex("[^A-Za-z0-9._-]"), "_")
            .take(64)
            .ifBlank { "image" }
        val extension = if (outputDirectory.overwriteOriginal) {
            // 覆盖原图：输出写回原路径，扩展名必须与原图一致，格式设置不生效。
            normalizedOutputExtension(input.displayName)
        } else {
            requestedOutputFormat.forcedExtension ?: normalizedOutputExtension(input.displayName)
        }
        return "$baseName.$extension"
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
        var targetUri = input.uri
        
        // Check for read-only URIs first (like Photo Picker)
        if (isPhotoPickerUri(targetUri)) {
            val realUri = resolveRealUriFromPicker(targetUri)
            if (realUri != null) {
                // We successfully found the real underlying URI, so we can try to overwrite it.
                targetUri = realUri
            } else {
                // Photo Picker URIs are read-only and cannot be overwritten.
                // Fallback to saving as a new file in the output directory.
                return commitAsNewOutput(
                    outputDirectory = outputDirectory,
                    outputName = outputNameFor(outputDirectory, input),
                    outputFile = outputFile
                )
            }
        }

        return runCatching {
            if (outputDirectory.backupOriginal) {
                // Backup reads from the original input.uri, which is always readable (even if it's from Picker)
                backupOriginalInput(outputDirectory = outputDirectory, input = input)
            }
            if (targetUri.scheme.equals("file", ignoreCase = true)) {
                val targetFile = File(requireNotNull(targetUri.path) { "Invalid file uri: $targetUri" })
                writeTempToTarget(outputFile = outputFile, targetFile = targetFile)
                outputFile.delete()
                targetFile.absolutePath
            } else if (isMediaStoreUri(targetUri)) {
                commitOverwriteMediaStore(targetUri, outputFile)
                outputFile.delete()
                targetUri.toString()
            } else {
                copyThenSync(src = outputFile) {
                    openDocumentOutputStream(targetUri)
                }
                outputFile.delete()
                targetUri.toString()
            }
        }
    }

    private fun resolveRealUriFromPicker(pickerUri: Uri): Uri? {
        var realPath: String? = null
        runCatching {
            contentResolver.query(pickerUri, arrayOf(MediaStore.MediaColumns.DATA), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val columnIndex = cursor.getColumnIndex(MediaStore.MediaColumns.DATA)
                    if (columnIndex != -1) {
                        realPath = cursor.getString(columnIndex)
                    }
                }
            }
        }
        
        if (realPath.isNullOrBlank()) return null
        
        // Try to map back to a real MediaStore URI
        runCatching {
            val projection = arrayOf(MediaStore.Images.Media._ID)
            val selection = "${MediaStore.Images.Media.DATA} = ?"
            val selectionArgs = arrayOf(realPath)
            contentResolver.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, projection, selection, selectionArgs, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val idColumn = cursor.getColumnIndex(MediaStore.Images.Media._ID)
                    if (idColumn != -1) {
                        val id = cursor.getLong(idColumn)
                        return android.content.ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, id)
                    }
                }
            }
        }
        
        // Fallback to a File URI if we got a valid path but couldn't find it in MediaStore
        return Uri.fromFile(File(realPath))
    }

    private fun isPhotoPickerUri(uri: Uri): Boolean {
        // Photo Picker authority is often "com.android.providers.media.photopicker".
        // However, on some systems (like MIUI/HyperOS), the authority might be "media" 
        // while the path contains "photopicker" or starts with "/picker".
        val authority = uri.authority
        val path = uri.path
        return authority?.contains("photopicker", ignoreCase = true) == true ||
                path?.contains("photopicker", ignoreCase = true) == true ||
                path?.startsWith("/picker") == true
    }

    private fun isMediaStoreUri(uri: Uri): Boolean {
        // Regular MediaStore authority is "media".
        // We ensure it's specifically the media authority and NOT photopicker.
        return uri.authority == MediaStore.AUTHORITY && !isPhotoPickerUri(uri)
    }

    private fun commitToContentUri(
        parentDirectoryUri: Uri,
        outputName: String,
        outputFile: File
    ): Result<String> {
        return runCatching {
            val outputDocumentUri = findOrCreateChildDocumentUri(
                parentDocumentUri = parentDirectoryUri,
                displayName = outputName,
                mimeType = mimeTypeForOutputName(outputName),
                expectDirectory = false
            )

            copyThenSync(src = outputFile) {
                openDocumentOutputStream(outputDocumentUri)
            }

            outputFile.delete()
            outputDocumentUri.toString()
        }
    }

    private fun commitOverwriteMediaStore(uri: Uri, outputFile: File) {
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        
        try {
            contentResolver.update(uri, values, null, null)
        } catch (e: Exception) {
            // Ignore if IS_PENDING update fails (e.g. on older Android versions or read-only items)
        }

        try {
            copyThenSync(src = outputFile) {
                contentResolver.openOutputStream(uri, "wt")
                    ?: throw IOException("Unable to open output stream: $uri (pending=1)")
            }
        } finally {
            values.clear()
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            try {
                contentResolver.update(uri, values, null, null)
            } catch (e: Exception) {
                // Ignore failure to clear IS_PENDING
            }
        }
    }

    private fun backupOriginalInput(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ) {
        when (val target = backupTargetFor(input)) {
            is BackupTarget.FileSystem -> {
                if (!target.sourceFile.exists()) {
                    throw IOException(
                        "Source file does not exist for backup: ${target.sourceFile.absolutePath}"
                    )
                }
                if (!target.backupDir.exists() && !target.backupDir.mkdirs()) {
                    throw IOException("Failed to create backup directory: ${target.backupDir.absolutePath}")
                }
                val backupFile = resolveAvailableTargetFile(
                    targetDirectory = target.backupDir,
                    outputName = input.displayName
                )
                target.sourceFile.copyTo(backupFile, overwrite = false)
            }
            is BackupTarget.Documents -> {
                // For SAF (like tree selections)
                val bakDirUri = findOrCreateChildDocumentUri(
                    parentDocumentUri = target.parentUri,
                    displayName = BACKUP_DIR_NAME,
                    mimeType = DocumentsContract.Document.MIME_TYPE_DIR,
                    expectDirectory = true,
                    allowAlternateNameOnConflict = false
                )
                val mimeType = contentResolver.getType(input.uri) ?: "application/octet-stream"
                val backupUri = findOrCreateChildDocumentUri(
                    parentDocumentUri = bakDirUri,
                    displayName = input.displayName,
                    mimeType = mimeType,
                    expectDirectory = false,
                    allowAlternateNameOnConflict = true
                )
                val source = contentResolver.openInputStream(input.uri)
                    ?: throw IOException("Unable to read original file for backup: ${input.uri}")
                val sink = contentResolver.openOutputStream(backupUri)
                    ?: throw IOException("Unable to open backup file for writing: $backupUri")
                source.use { input ->
                    sink.use { output ->
                        input.copyTo(output)
                    }
                }
            }
            null -> {
                // Fallback for simple content URIs
                val backupOutputRootDir = File(defaultOutputRootDir(), BACKUP_ROOT_DIR)
                val backupDir = File(backupOutputRootDir, outputDirectory.batchId)
                if (!backupDir.exists() && !backupDir.mkdirs()) {
                    throw IOException("Failed to create backup directory: ${backupDir.absolutePath}")
                }
                val backupFile = resolveAvailableTargetFile(targetDirectory = backupDir, outputName = input.displayName)

                val source = contentResolver.openInputStream(input.uri)
                    ?: throw IOException("Unable to read original file for backup: ${input.uri}")
                source.use { inputStream ->
                    backupFile.outputStream().use { sink ->
                        inputStream.copyTo(sink)
                    }
                }
            }
        }
    }

    private fun findParentDocumentUri(childUri: Uri): Uri? {
        return runCatching {
            // Document IDs look like "treeId:path/to/file.ext"
            val documentId = DocumentsContract.getDocumentId(childUri)
            if (!documentId.contains(':')) return null
            val root = documentId.substringBefore(':')
            val path = documentId.substringAfter(':')
            
            // If it's directly in the root, the parent is the root
            if (!path.contains('/')) {
                return DocumentsContract.buildDocumentUriUsingTree(childUri, "$root:")
            }
            
            val parentPath = path.substringBeforeLast('/')
            val parentDocumentId = "$root:$parentPath"
            return DocumentsContract.buildDocumentUriUsingTree(childUri, parentDocumentId)
        }.getOrNull()
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

    private fun rootDocumentUriFromTreeUri(rootTreeUri: Uri): Uri? {
        return runCatching {
            val rootDocumentId = DocumentsContract.getTreeDocumentId(rootTreeUri)
            DocumentsContract.buildDocumentUriUsingTree(rootTreeUri, rootDocumentId)
        }.getOrNull()
    }

    private fun sourceFolderName(source: String): String? {
        val decodedSources = InputSourceSpec.decode(source)
        if (decodedSources != null) {
            return null
        }

        val parsedUri = parseUri(source.trim())
        if (parsedUri != null && !parsedUri.scheme.isNullOrBlank()) {
            if (DocumentsContract.isTreeUri(parsedUri)) {
                val documentId = runCatching {
                    DocumentsContract.getTreeDocumentId(parsedUri)
                }.getOrNull().orEmpty()
                val treeName = documentId.substringAfter(':', documentId)
                    .substringAfterLast('/')
                return sanitizeFolderName(treeName)
            }

            if (parsedUri.scheme.equals("file", ignoreCase = true)) {
                val directory = parsedUri.path?.let { path -> File(path) }
                if (directory != null && directory.isDirectory) {
                    return sanitizeFolderName(directory.name)
                }
            }

            return null
        }

        val sourceFile = File(source)
        if (!sourceFile.isDirectory) {
            return null
        }
        return sanitizeFolderName(sourceFile.name)
    }

    private fun sanitizeFolderName(rawName: String): String? {
        val normalized = rawName
            .trim()
            .replace(Regex("[\\\\/:*?\"<>|]"), "_")
            .take(80)
        return normalized.ifBlank { null }
    }

    private fun resolveAvailableTargetFile(
        targetDirectory: File,
        outputName: String
    ): File {
        val directTarget = File(targetDirectory, outputName)
        if (!directTarget.exists()) {
            return directTarget
        }
        if (directTarget.isDirectory) {
            return directTarget
        }

        for (attempt in 1..MAX_CREATE_DOCUMENT_RETRIES) {
            val candidate = File(targetDirectory, outputNameWithRetrySuffix(outputName, attempt))
            if (!candidate.exists()) {
                return candidate
            }
            if (candidate.isDirectory) {
                return candidate
            }
        }

        return File(targetDirectory, outputNameWithRetrySuffix(outputName, MAX_CREATE_DOCUMENT_RETRIES + 1))
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
        // Prefer "wt" (Write + Truncate) for overwriting behavior.
        // Some providers might not support it, so we fallback.
        val modes = arrayOf("wt", "rwt", "w")
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
        throw IOException("Failed to open document output stream for $documentUri", lastError)
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
        private const val BACKUP_DIR_NAME = ".bak"
        private const val TEMP_OUTPUT_ROOT_DIR = "outputs_temp"
        private const val PNG_MIME_TYPE = "image/png"
        private const val JPEG_MIME_TYPE = "image/jpeg"
        private const val WEBP_MIME_TYPE = "image/webp"
        private const val CONTENT_SCHEME_PREFIX = "content://"
        private const val MAX_CREATE_DOCUMENT_RETRIES = 99
    }
}
