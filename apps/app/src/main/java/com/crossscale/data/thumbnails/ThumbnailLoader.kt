package com.crossscale.data.thumbnails

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.crossscale.data.repository.InputSourceSpec
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val THUMBNAIL_TARGET_SIZE_PX = 192

private fun decodeQueueThumbnail(
    context: Context,
    source: String
): ImageBitmap? {
    val sourceKey = thumbnailCacheKey(source)
    ThumbnailMemoryCache.getThumbnail(sourceKey)?.let { cachedBitmap ->
        return cachedBitmap
    }

    val thumbnailSources = resolveThumbnailCandidateSources(context = context, source = source)
    for (thumbnailSource in thumbnailSources) {
        val candidateKey = thumbnailCacheKey(thumbnailSource)
        ThumbnailMemoryCache.getThumbnail(candidateKey)?.let { cachedBitmap ->
            ThumbnailMemoryCache.putThumbnail(sourceKey, cachedBitmap)
            return cachedBitmap
        }

        val boundsOptions = BitmapFactory.Options().apply {
            inJustDecodeBounds = true
        }
        val hasBounds = openThumbnailInputStream(context = context, source = thumbnailSource)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, boundsOptions)
            true
        } == true
        if (!hasBounds) {
            continue
        }

        val sourceWidth = boundsOptions.outWidth
        val sourceHeight = boundsOptions.outHeight
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            continue
        }

        val decodeOptions = BitmapFactory.Options().apply {
            inSampleSize = calculateInSampleSize(sourceWidth, sourceHeight, THUMBNAIL_TARGET_SIZE_PX)
            inPreferredConfig = Bitmap.Config.RGB_565
        }

        val thumbnail = openThumbnailInputStream(context = context, source = thumbnailSource)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, decodeOptions)
        } ?: continue

        val imageBitmap = thumbnail.asImageBitmap()
        ThumbnailMemoryCache.putThumbnail(candidateKey, imageBitmap)
        ThumbnailMemoryCache.putThumbnail(sourceKey, imageBitmap)
        return imageBitmap
    }
    return null
}

internal suspend fun decodeQueueThumbnailWithLimit(
    context: Context,
    source: String
): ImageBitmap? {
    return withContext(Dispatchers.IO) {
        ThumbnailDecodeLimiter.withPermit {
            decodeQueueThumbnail(context = context, source = source)
        }
    }
}

private fun resolveThumbnailCandidateSources(
    context: Context,
    source: String
): List<String> {
    val normalizedSource = source.trim()
    if (normalizedSource.isBlank()) {
        return emptyList()
    }

    ThumbnailMemoryCache.getSourceCandidates(normalizedSource)?.let { cachedCandidates ->
        return cachedCandidates
    }

    val decodedSources = InputSourceSpec.decode(normalizedSource).orEmpty()
    val directSources = if (decodedSources.isEmpty()) {
        listOf(normalizedSource)
    } else {
        decodedSources
    }

    val resolvedSources = directSources
        .map { candidate -> candidate.trim() }
        .filter { candidate -> candidate.isNotBlank() }
        .flatMap { candidate ->
            val treePreview = resolveTreeSourceThumbnail(context = context, source = candidate)
            if (treePreview == null) {
                listOf(candidate)
            } else {
                listOf(treePreview, candidate)
            }
        }
        .distinct()

    ThumbnailMemoryCache.putSourceCandidates(normalizedSource, resolvedSources)
    return resolvedSources
}

private fun resolveTreeSourceThumbnail(
    context: Context,
    source: String
): String? {
    val sourceUri = runCatching { Uri.parse(source) }.getOrNull() ?: return null
    if (sourceUri.scheme?.lowercase() != "content") {
        return null
    }

    if (!DocumentsContract.isTreeUri(sourceUri)) {
        return null
    }

    val treeDocumentId = runCatching {
        DocumentsContract.getTreeDocumentId(sourceUri)
    }.getOrNull() ?: return null
    val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(sourceUri, treeDocumentId)

    return runCatching {
        context.contentResolver.query(
            childrenUri,
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE
            ),
            null,
            null,
            null
        )?.use { cursor ->
            val documentIdIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
            val mimeTypeIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
            if (documentIdIndex < 0 || mimeTypeIndex < 0) {
                return@use null
            }
            while (cursor.moveToNext()) {
                val mimeType = cursor.getString(mimeTypeIndex)?.trim().orEmpty()
                if (!mimeType.startsWith("image/")) {
                    continue
                }
                val documentId = cursor.getString(documentIdIndex)?.trim().orEmpty()
                if (documentId.isBlank()) {
                    continue
                }
                val documentUri = DocumentsContract.buildDocumentUriUsingTree(sourceUri, documentId)
                return@use documentUri.toString()
            }
            null
        }
    }.getOrNull()
}

private fun openThumbnailInputStream(
    context: Context,
    source: String
) = runCatching {
    if (source.startsWith("/")) {
        return@runCatching File(source).inputStream()
    }

    val parsedUri = Uri.parse(source)
    when (parsedUri.scheme?.lowercase()) {
        null -> File(source).inputStream()
        "file" -> {
            val path = parsedUri.path ?: return@runCatching null
            File(path).inputStream()
        }
        else -> context.contentResolver.openInputStream(parsedUri)
    }
}.getOrNull()

private fun calculateInSampleSize(
    sourceWidth: Int,
    sourceHeight: Int,
    requestedSizePx: Int
): Int {
    var sampleSize = 1
    var currentWidth = sourceWidth
    var currentHeight = sourceHeight
    while (currentWidth > requestedSizePx || currentHeight > requestedSizePx) {
        sampleSize *= 2
        currentWidth /= 2
        currentHeight /= 2
    }
    return sampleSize.coerceAtLeast(1)
}

private fun thumbnailCacheKey(source: String): String {
    return source.trim()
}
