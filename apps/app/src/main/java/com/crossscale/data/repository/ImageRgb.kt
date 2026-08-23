package com.crossscale.data.repository

import android.content.ContentResolver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import androidx.exifinterface.media.ExifInterface
import java.io.File
import java.io.IOException
import java.io.InputStream

/** Decoded RGB888 image payload handed to the native layer. */
data class DecodedRgbImage(
    val width: Int,
    val height: Int,
    val rgbBytes: ByteArray
)

internal const val RGB_CHANNELS = 3

private const val MAX_RGB_BUFFER_HEAP_RATIO = 0.85
private const val ANDROID_AVIF_DECODE_SDK = 31

/**
 * Decodes [uri] into a packed RGB (3 bytes/pixel) buffer for the native layer:
 * ARGB_8888 conversion, size/heap guards and the row-wise ARGB->RGB extraction
 * in one place. Returns null on any failure; [onError] receives a diagnostic
 * message the caller can log with its own tag.
 */
internal fun decodeRgb(
    resolver: ContentResolver,
    uri: Uri,
    displayName: String? = null,
    onError: (message: String) -> Unit
): DecodedRgbImage? {
    val bitmap = runCatching {
        resolver.openInputStream(uri)?.use { inputStream ->
            BitmapFactory.decodeStream(inputStream)
        }
    }.onFailure { throwable ->
        onError("Unable to read input URI $uri: ${throwable.message}")
    }.getOrNull()
    if (bitmap == null) {
        if (isAvifName(displayName) || isAvifUri(resolver, uri)) {
            onError(avifDecodeErrorMessage(displayName ?: uri.toString()))
        }
        return null
    }

    // 竖拍照片的 EXIF 朝向：解码后按角度旋转，否则人像图会被横着放大。
    // 流只能读一次，EXIF 需要单独再开一个流。
    val orientationDegrees = readExifOrientationDegrees { resolver.openInputStream(uri) }
    val orientedBitmap = applyExifRotation(bitmap, orientationDegrees)

    val argbBitmap = if (orientedBitmap.config == Bitmap.Config.ARGB_8888) {
        orientedBitmap
    } else {
        val converted = orientedBitmap.copy(Bitmap.Config.ARGB_8888, false)
        orientedBitmap.recycle()
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
    return outputFormatForExtension(file.extension)
}

internal fun decodeEncodedImageBounds(encodedBytes: ByteArray): Pair<Int, Int>? {
    val options = BitmapFactory.Options().apply {
        inJustDecodeBounds = true
    }
    BitmapFactory.decodeByteArray(encodedBytes, 0, encodedBytes.size, options)
    if (options.outWidth > 0 && options.outHeight > 0) {
        return options.outWidth to options.outHeight
    }
    return AvifFormat.sniffDimensions(encodedBytes)
}

internal fun isAvifName(name: String?): Boolean {
    return name?.substringAfterLast('.', "")?.equals("avif", ignoreCase = true) == true
}

private fun isAvifUri(resolver: ContentResolver, uri: Uri): Boolean {
    return resolver.getType(uri)?.equals("image/avif", ignoreCase = true) == true
}

internal fun avifDecodeErrorMessage(label: String): String {
    return if (android.os.Build.VERSION.SDK_INT < ANDROID_AVIF_DECODE_SDK) {
        "无法解码 AVIF（需要 Android 12 及以上）：$label"
    } else {
        "无法解码 AVIF：$label"
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

// ── EXIF 朝向（缩略图与 RGB 解码共用）──────────────────────────────

/**
 * 读取 EXIF 朝向角（0/90/180/270）。流只能消费一次，[openStream] 由调用方
 * 提供一条独立的新流；读不到或不含 EXIF 时返回 0。
 */
internal fun readExifOrientationDegrees(openStream: () -> InputStream?): Int {
    return runCatching {
        openStream()?.use { stream ->
            val exif = ExifInterface(stream)
            when (
                exif.getAttributeInt(
                    ExifInterface.TAG_ORIENTATION,
                    ExifInterface.ORIENTATION_NORMAL
                )
            ) {
                ExifInterface.ORIENTATION_ROTATE_90,
                ExifInterface.ORIENTATION_TRANSPOSE -> 90

                ExifInterface.ORIENTATION_ROTATE_180,
                ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> 180

                ExifInterface.ORIENTATION_ROTATE_270,
                ExifInterface.ORIENTATION_TRANSVERSE -> 270

                else -> 0
            }
        } ?: 0
    }.getOrDefault(0)
}

/** 按角度顺时针旋转 [bitmap]；0 度原样返回。非 0 度时原 bitmap 会被回收。 */
internal fun applyExifRotation(bitmap: Bitmap, degrees: Int): Bitmap {
    val normalizedDegrees = ((degrees % 360) + 360) % 360
    if (normalizedDegrees == 0) {
        return bitmap
    }
    val matrix = Matrix().apply { postRotate(normalizedDegrees.toFloat()) }
    val rotated = Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, false)
    bitmap.recycle()
    return rotated
}
