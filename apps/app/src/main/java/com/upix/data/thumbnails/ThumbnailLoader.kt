package com.upix.data.thumbnails

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.upix.data.repository.InputSourceSpec
import com.upix.data.repository.applyExifRotation
import com.upix.data.repository.readExifOrientationDegrees
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val THUMBNAIL_TARGET_SIZE_PX = 192

private fun decodeQueueThumbnail(
    context: Context,
    source: String,
    targetSizePx: Int
): ImageBitmap? {
    val sourceKey = thumbnailCacheKey(source, targetSizePx)
    ThumbnailMemoryCache.getThumbnail(sourceKey)?.let { cachedBitmap ->
        return cachedBitmap
    }

    val thumbnailSources = resolveThumbnailCandidateSources(context = context, source = source)
    for (thumbnailSource in thumbnailSources) {
        val candidateKey = thumbnailCacheKey(thumbnailSource, targetSizePx)
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
            inSampleSize = calculateInSampleSize(sourceWidth, sourceHeight, targetSizePx)
            // ARGB_8888：RGB_565 会色带/色偏；预览大图必须真彩。
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }

        val thumbnail = openThumbnailInputStream(context = context, source = thumbnailSource)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, decodeOptions)
        } ?: continue

        // 竖拍照片按 EXIF 朝向旋转，避免缩略图横躺；流只能读一次，EXIF 单独再开一条。
        val orientationDegrees = readExifOrientationDegrees {
            openThumbnailInputStream(context = context, source = thumbnailSource)
        }
        val orientedThumbnail = applyExifRotation(thumbnail, orientationDegrees)

        val imageBitmap = orientedThumbnail.asImageBitmap()
        ThumbnailMemoryCache.putThumbnail(candidateKey, imageBitmap)
        ThumbnailMemoryCache.putThumbnail(sourceKey, imageBitmap)
        return imageBitmap
    }
    return null
}

internal suspend fun decodeQueueThumbnailWithLimit(
    context: Context,
    source: String,
    targetSizePx: Int = THUMBNAIL_TARGET_SIZE_PX
): ImageBitmap? {
    return withContext(Dispatchers.IO) {
        ThumbnailDecodeLimiter.withPermit {
            decodeQueueThumbnail(context = context, source = source, targetSizePx = targetSizePx)
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
    // 规则一：最大 2^n 使降采样后两维仍 >= requested——普通图保清晰，
    // 不冲过头（旧循环 3024px 在 1080 目标下采到 756px，拉伸发糊）。
    // 规则二：投影像素超预算继续翻倍——长图（如 1080x20000）不满足规则一，
    // 会全分辨率解码爆内存；统一降到预算内。
    var sampleSize = 1
    val totalPixels = sourceWidth.toLong() * sourceHeight.toLong()
    while (((sourceWidth / (sampleSize * 2) >= requestedSizePx &&
                sourceHeight / (sampleSize * 2) >= requestedSizePx) ||
            totalPixels / (sampleSize.toLong() * 4) > MaxDecodedPixels)
    ) {
        sampleSize *= 2
    }
    return sampleSize
}

/** 单张解码像素上限（约 10MP）：超出即继续降采样，防长图 OOM。 */
private const val MaxDecodedPixels = 10_000_000L

// Cache keys must encode the decode size: the same source is decoded at
// different target sizes (queue rows vs. stage vs. preview) and must never
// alias onto each other's cache entries.
private fun thumbnailCacheKey(source: String, targetSizePx: Int): String {
    return "${source.trim()}#$targetSizePx"
}
