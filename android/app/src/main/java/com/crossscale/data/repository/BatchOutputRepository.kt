package com.crossscale.data.repository

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import java.security.MessageDigest

data class BatchOutputDirectory(
    val batchId: String,
    val displayPath: String,
    val outputDirectoryUri: String? = null,
    val internalDirectory: File? = null
)

class BatchOutputRepository(
    context: Context
) : BatchOutputStore {
    private val appContext = context.applicationContext
    private val contentResolver = appContext.contentResolver
    private val rootOutputDir = File(appContext.filesDir, OUTPUT_ROOT_DIR)
    private val tempOutputRootDir = File(appContext.cacheDir, TEMP_OUTPUT_ROOT_DIR)

    override fun prepareOutputDirectory(
        source: String,
        modelName: String,
        preferredTreeUri: String?,
        resumeOutputDirectory: String?,
        resumeOutputDirectoryUri: String?
    ): BatchOutputDirectory {
        val batchId = batchIdFor(source = source, modelName = modelName)
        val normalizedResumeUri = resumeOutputDirectoryUri.orEmpty().trim()
        if (normalizedResumeUri.isNotBlank()) {
            val parsedResumeUri = parseUri(normalizedResumeUri)
            if (parsedResumeUri != null && !parsedResumeUri.scheme.isNullOrBlank()) {
                val displayPath = resumeOutputDirectory?.takeIf { it.isNotBlank() } ?: parsedResumeUri.toString()
                return BatchOutputDirectory(
                    batchId = batchId,
                    displayPath = displayPath,
                    outputDirectoryUri = parsedResumeUri.toString(),
                    internalDirectory = null
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
                    internalDirectory = null
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
                    internalDirectory = null
                )
            }
        }

        val internalDirectory = when {
            normalizedResumeDirectory.isBlank() -> File(rootOutputDir, batchId)
            else -> File(normalizedResumeDirectory)
        }
        if (!internalDirectory.exists()) {
            internalDirectory.mkdirs()
        }
        return BatchOutputDirectory(
            batchId = batchId,
            displayPath = internalDirectory.absolutePath,
            outputDirectoryUri = null,
            internalDirectory = internalDirectory
        )
    }

    override fun outputFileFor(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput
    ): File {
        val outputName = outputNameFor(input)
        return if (outputDirectory.outputDirectoryUri.isNullOrBlank()) {
            val outputDir = requireNotNull(outputDirectory.internalDirectory) {
                "Internal output directory is missing for ${outputDirectory.batchId}"
            }
            File(outputDir, outputName)
        } else {
            val tempDir = File(tempOutputRootDir, outputDirectory.batchId)
            if (!tempDir.exists()) {
                tempDir.mkdirs()
            }
            File(tempDir, outputName)
        }
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

    override fun commitOutput(
        outputDirectory: BatchOutputDirectory,
        input: ResolvedImageInput,
        outputFile: File
    ): Result<String> {
        val outputName = outputNameFor(input)
        val outputDirectoryUri = outputDirectory.outputDirectoryUri.orEmpty()
        if (outputDirectoryUri.isBlank()) {
            return Result.success(outputFile.absolutePath)
        }
        val parsedDirectoryUri = parseUri(outputDirectoryUri)
            ?: return Result.failure(IllegalArgumentException("Invalid output URI: $outputDirectoryUri"))
        return runCatching {
            val existingUri = findChildDocumentUri(
                parentDocumentUri = parsedDirectoryUri,
                displayName = outputName,
                expectDirectory = false
            )
            if (existingUri != null) {
                runCatching {
                    DocumentsContract.deleteDocument(contentResolver, existingUri)
                }
            }

            val createdUri = DocumentsContract.createDocument(
                contentResolver,
                parsedDirectoryUri,
                PNG_MIME_TYPE,
                outputName
            ) ?: throw IOException("Failed to create output document: $outputName")

            contentResolver.openOutputStream(createdUri, "w")?.use { outputStream ->
                outputFile.inputStream().use { inputStream ->
                    inputStream.copyTo(outputStream)
                }
            } ?: throw IOException("Unable to open output stream: $createdUri")

            outputFile.delete()
            createdUri.toString()
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
        return "${baseName}_${uniqueSuffix}.png"
    }

    private fun ensureSubDirectory(
        rootTreeUri: Uri,
        directoryName: String
    ): Uri? {
        return runCatching {
            val rootDocumentId = DocumentsContract.getTreeDocumentId(rootTreeUri)
            val rootDocumentUri = DocumentsContract.buildDocumentUriUsingTree(rootTreeUri, rootDocumentId)
            findChildDocumentUri(
                parentDocumentUri = rootDocumentUri,
                displayName = directoryName,
                expectDirectory = true
            ) ?: DocumentsContract.createDocument(
                contentResolver,
                rootDocumentUri,
                DocumentsContract.Document.MIME_TYPE_DIR,
                directoryName
            )
        }.getOrNull()
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

    companion object {
        private const val OUTPUT_ROOT_DIR = "outputs"
        private const val TEMP_OUTPUT_ROOT_DIR = "outputs_temp"
        private const val PNG_MIME_TYPE = "image/png"
        private const val CONTENT_SCHEME_PREFIX = "content://"
    }
}
