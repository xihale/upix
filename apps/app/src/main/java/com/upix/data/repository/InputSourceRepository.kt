package com.upix.data.repository

import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.File
import java.util.LinkedHashMap
import java.util.Locale

data class ResolvedImageInput(
    val uri: Uri,
    val displayName: String
)

class InputSourceRepository(
    private val context: Context,
    private val logRepository: LogRepository
) : InputSourceResolver {
    override fun resolveImageInputs(
        source: String,
        nested: Boolean
    ): List<ResolvedImageInput> {
        return resolveImageInputsInternal(
            source = source,
            nested = nested,
            visitedSources = mutableSetOf()
        )
    }

    private fun resolveImageInputsInternal(
        source: String,
        nested: Boolean,
        visitedSources: MutableSet<String>
    ): List<ResolvedImageInput> {
        if (source.isBlank()) {
            return emptyList()
        }
        if (!visitedSources.add(source)) {
            return emptyList()
        }

        val multiSources = InputSourceSpec.decode(source)
        if (multiSources != null) {
            val mergedInputs = LinkedHashMap<String, ResolvedImageInput>()
            multiSources.forEach { subSource ->
                resolveImageInputsInternal(
                    source = subSource,
                    nested = nested,
                    visitedSources = visitedSources
                ).forEach { input ->
                    mergedInputs.putIfAbsent(input.uri.toString(), input)
                }
            }
            return mergedInputs.values.toList()
        }

        val parsedUri = runCatching { Uri.parse(source) }.getOrNull()
        if (parsedUri != null && !parsedUri.scheme.isNullOrBlank()) {
            return resolveUriSource(uri = parsedUri, nested = nested)
        }

        val file = File(source)
        if (!file.exists()) {
            logRepository.warning("InputSourceRepository", "Input source does not exist: $source")
            return emptyList()
        }
        if (file.isFile) {
            if (!isImageMimeOrName(mimeType = null, name = file.name)) {
                return emptyList()
            }
            val fileUri = Uri.fromFile(file)
            if (!isDecodableImage(uri = fileUri, displayName = file.name)) {
                logRepository.warning("InputSourceRepository", "Corrupted image skipped: ${file.absolutePath}")
                return emptyList()
            }
            return listOf(
                ResolvedImageInput(
                    uri = fileUri,
                    displayName = file.name
                )
            )
        }

        if (!file.isDirectory) {
            return emptyList()
        }

        val fileSequence = if (nested) {
            file.walkTopDown()
        } else {
            file.listFiles()?.asSequence() ?: emptySequence()
        }

        return fileSequence
            .filter { it.isFile }
            .filter { isImageMimeOrName(mimeType = null, name = it.name) }
            .map { path ->
                val fileUri = Uri.fromFile(path)
                if (!isDecodableImage(uri = fileUri, displayName = path.name)) {
                    logRepository.warning("InputSourceRepository", "Corrupted image skipped: ${path.absolutePath}")
                    null
                } else {
                    ResolvedImageInput(
                        uri = fileUri,
                        displayName = path.name
                    )
                }
            }
            .filterNotNull()
            .sortedWith(resolvedImageComparator())
            .toList()
    }

    private fun resolveUriSource(uri: Uri, nested: Boolean): List<ResolvedImageInput> {
        if (DocumentsContract.isTreeUri(uri)) {
            return resolveTreeImages(treeUri = uri, nested = nested)
        }
        return resolveSingleUriImage(uri = uri)
    }

    private fun resolveSingleUriImage(uri: Uri): List<ResolvedImageInput> {
        val contentResolver = context.contentResolver
        val mimeType = contentResolver.getType(uri)
        val displayName = queryDisplayName(uri) ?: uri.lastPathSegment ?: uri.toString()
        if (!isImageMimeOrName(mimeType = mimeType, name = displayName)) {
            return emptyList()
        }
        if (!isDecodableImage(uri = uri, displayName = displayName)) {
            logRepository.warning("InputSourceRepository", "Corrupted image skipped: $displayName")
            return emptyList()
        }
        return listOf(
            ResolvedImageInput(
                uri = uri,
                displayName = displayName
            )
        )
    }

    private fun resolveTreeImages(treeUri: Uri, nested: Boolean): List<ResolvedImageInput> {
        val documentId = runCatching { DocumentsContract.getTreeDocumentId(treeUri) }.getOrNull()
            ?: return emptyList()
        val pendingDocumentIds = ArrayDeque<String>()
        pendingDocumentIds.add(documentId)
        val images = mutableListOf<ResolvedImageInput>()
        val contentResolver = context.contentResolver

        while (pendingDocumentIds.isNotEmpty()) {
            val currentDocumentId = pendingDocumentIds.removeFirst()
            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, currentDocumentId)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME
            )

            try {
                contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
                    val documentIdIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                    val mimeTypeIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
                    val displayNameIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                    while (cursor.moveToNext()) {
                        if (documentIdIndex == -1 || mimeTypeIndex == -1) {
                            continue
                        }
                        val childDocumentId = cursor.getString(documentIdIndex) ?: continue
                        val mimeType = cursor.getString(mimeTypeIndex)
                        val displayName = if (displayNameIndex >= 0) {
                            cursor.getString(displayNameIndex)
                        } else {
                            null
                        } ?: childDocumentId.substringAfterLast(':', childDocumentId)

                        if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) {
                            if (nested) {
                                pendingDocumentIds.addLast(childDocumentId)
                            }
                            continue
                        }
                        if (!isImageMimeOrName(mimeType = mimeType, name = displayName)) {
                            continue
                        }
                        val imageUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childDocumentId)
                        if (!isDecodableImage(uri = imageUri, displayName = displayName)) {
                            logRepository.warning("InputSourceRepository", "Corrupted image skipped: $displayName")
                            continue
                        }
                        images.add(ResolvedImageInput(uri = imageUri, displayName = displayName))
                    }
                }
            } catch (securityException: SecurityException) {
                logRepository.error(
                    "InputSourceRepository",
                    "Failed to read folder URI due to missing permission: ${securityException.message}"
                )
                return emptyList()
            } catch (illegalArgumentException: IllegalArgumentException) {
                logRepository.error(
                    "InputSourceRepository",
                    "Failed to parse folder URI: ${illegalArgumentException.message}"
                )
                return emptyList()
            }
        }

        return images.sortedWith(resolvedImageComparator())
    }

    private fun queryDisplayName(uri: Uri): String? {
        return try {
            context.contentResolver.query(
                uri,
                arrayOf(OpenableColumns.DISPLAY_NAME),
                null,
                null,
                null
            )?.use { cursor ->
                val displayNameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (displayNameColumn == -1 || !cursor.moveToFirst()) {
                    null
                } else {
                    cursor.getString(displayNameColumn)
                }
            }
        } catch (_: SecurityException) {
            null
        }
    }

    private fun isImageMimeOrName(
        mimeType: String?,
        name: String
    ): Boolean {
        val lowerMime = mimeType?.lowercase(Locale.US)
        if (lowerMime != null && lowerMime.startsWith("image/")) {
            return true
        }
        val lowerName = name.lowercase(Locale.US)
        return IMAGE_EXTENSIONS.any { extension -> lowerName.endsWith(extension) }
    }

    private fun isDecodableImage(
        uri: Uri,
        displayName: String
    ): Boolean {
        return runCatching {
            context.contentResolver.openInputStream(uri)?.use { stream ->
                val boundsOptions = BitmapFactory.Options().apply {
                    inJustDecodeBounds = true
                }
                BitmapFactory.decodeStream(stream, null, boundsOptions)
                boundsOptions.outWidth > 0 && boundsOptions.outHeight > 0
            } == true
        }.onFailure { throwable ->
            val avifHint = if (isAvifName(displayName)) {
                " (${avifDecodeErrorMessage(displayName)})"
            } else {
                ""
            }
            logRepository.warning(
                "InputSourceRepository",
                "Failed to decode image $displayName: ${throwable.message}$avifHint"
            )
        }.getOrDefault(false)
    }

    companion object {
        private val IMAGE_EXTENSIONS = setOf(
            ".jpg",
            ".jpeg",
            ".png",
            ".webp",
            ".bmp",
            ".gif",
            ".tif",
            ".tiff",
            // HEIF/HEIC 照片（部分机型默认格式）
            ".heic",
            ".heif",
            ".avif"
        )
    }

    private fun resolvedImageComparator(): Comparator<ResolvedImageInput> {
        return compareBy(
            { input -> input.displayName.lowercase(Locale.US) },
            { input -> input.uri.toString() }
        )
    }
}
