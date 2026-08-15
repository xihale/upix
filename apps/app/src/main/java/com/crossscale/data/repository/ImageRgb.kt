package com.crossscale.data.repository

import android.content.ContentResolver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import java.io.File
import java.io.IOException

/** Decoded RGB888 image payload handed to the native layer. */
data class DecodedRgbImage(
    val width: Int,
    val height: Int,
    val rgbBytes: ByteArray
)

internal const val RGB_CHANNELS = 3

private const val DEFAULT_OUTPUT_FORMAT = "png"
private const val MAX_RGB_BUFFER_HEAP_RATIO = 0.85

/**
 * Decodes [uri] into a packed RGB (3 bytes/pixel) buffer for the native layer:
 * ARGB_8888 conversion, size/heap guards and the row-wise ARGB->RGB extraction
 * in one place. Returns null on any failure; [onError] receives a diagnostic
 * message the caller can log with its own tag.
 */
internal fun decodeRgb(
    resolver: ContentResolver,
    uri: Uri,
    onError: (message: String) -> Unit
): DecodedRgbImage? {
    val bitmap = runCatching {
        resolver.openInputStream(uri)?.use { inputStream ->
            BitmapFactory.decodeStream(inputStream)
        }
    }.onFailure { throwable ->
        onError("Unable to read input URI $uri: ${throwable.message}")
    }.getOrNull() ?: return null

    val argbBitmap = if (bitmap.config == Bitmap.Config.ARGB_8888) {
        bitmap
    } else {
        val converted = bitmap.copy(Bitmap.Config.ARGB_8888, false)
        bitmap.recycle()
        converted ?: return null
    }

    val width = argbBitmap.width
    val height = argbBitmap.height
    if (width <= 0 || height <= 0) {
        argbBitmap.recycle()
        return null
    }

    val rgbByteCount = width.toLong() * height.toLong() * RGB_CHANNELS
    if (rgbByteCount <= 0L || rgbByteCount > Int.MAX_VALUE.toLong()) {
        onError("Input image is too large to allocate RGB buffer: $width x $height")
        argbBitmap.recycle()
        return null
    }

    if (!hasSufficientHeapForRgbBuffer(width = width, rgbByteCount = rgbByteCount)) {
        val runtime = Runtime.getRuntime()
        val used = runtime.totalMemory() - runtime.freeMemory()
        val available = (runtime.maxMemory() - used).coerceAtLeast(0L)
        onError(
            "Insufficient heap before RGB conversion: $width x $height, " +
                "required=${rgbByteCount}B, available=${available}B"
        )
        argbBitmap.recycle()
        return null
    }

    val rgbBytes = runCatching {
        ByteArray(rgbByteCount.toInt())
    }.onFailure { throwable ->
        onError("Failed to allocate RGB buffer for $uri (${width}x${height}): ${throwable.message}")
    }.getOrNull() ?: run {
        argbBitmap.recycle()
        return null
    }

    val rowPixels = IntArray(width)
    var writeIndex = 0
    for (row in 0 until height) {
        argbBitmap.getPixels(
            rowPixels,
            0,
            width,
            0,
            row,
            width,
            1
        )
        for (column in 0 until width) {
            val pixel = rowPixels[column]
            rgbBytes[writeIndex++] = ((pixel shr 16) and 0xFF).toByte()
            rgbBytes[writeIndex++] = ((pixel shr 8) and 0xFF).toByte()
            rgbBytes[writeIndex++] = (pixel and 0xFF).toByte()
        }
    }
    argbBitmap.recycle()

    return DecodedRgbImage(
        width = width,
        height = height,
        rgbBytes = rgbBytes
    )
}

private fun hasSufficientHeapForRgbBuffer(
    width: Int,
    rgbByteCount: Long
): Boolean {
    val runtime = Runtime.getRuntime()
    val used = runtime.totalMemory() - runtime.freeMemory()
    val available = (runtime.maxMemory() - used).coerceAtLeast(0L)
    val estimatedAdditionalBytes = rgbByteCount + (width.toLong() * Int.SIZE_BYTES)
    val safeBudget = (available * MAX_RGB_BUFFER_HEAP_RATIO).toLong()
    return estimatedAdditionalBytes <= safeBudget
}

/** Output encoding format requested from the native layer for [file]'s extension. */
internal fun outputFormatFor(file: File): String {
    return when (file.extension.trim().lowercase()) {
        "png" -> "png"
        "jpg" -> "jpg"
        "jpeg" -> "jpeg"
        "webp" -> "webp"
        else -> DEFAULT_OUTPUT_FORMAT
    }
}

/**
 * True for EEXIST/"File exists" IO failures raced between a temp write and the
 * move onto the target — the retryable case shared by the output writers.
 */
internal fun isExistsConflict(throwable: Throwable): Boolean {
    return throwable is IOException &&
        (throwable.message?.contains("EEXIST", ignoreCase = true) == true ||
            throwable.message?.contains("File exists", ignoreCase = true) == true)
}
