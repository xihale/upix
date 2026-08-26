package com.upix.data.repository

import android.content.ContentResolver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import android.provider.OpenableColumns
import androidx.exifinterface.media.ExifInterface
import com.upix.nativelib.NCNNEngine
import com.upix.nativelib.NativeCapabilities
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

/** 单个输入（远程处理上传用）的编码字节上限：256 MB，超过按失败处理而非 OOM。 */
internal const val MAX_ENCODED_INPUT_BYTES: Long = 256L * 1024 * 1024

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
            // 平台解码器不认（API 26-30 或 OEM 差异）：带 native dav1d 的构建
            // 走 native 解码回退，精简构建维持错误提示。
            return decodeAvifNative(resolver, uri, displayName ?: uri.toString(), onError)
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
    val available = runtime.maxMemory() - used
    val estimatedAdditionalBytes = rgbByteCount + (width.toLong() * Int.SIZE_BYTES)
    val safeBudget = (available * MAX_RGB_BUFFER_HEAP_RATIO).toLong()
    return estimatedAdditionalBytes <= safeBudget
}

/**
 * Native AVIF 解码回退（需构建携带 libavif+dav1d，见 NativeCapabilities）。
 * 平台 BitmapFactory 解不了 AVIF 的场合（API 26-30、OEM 解码器缺陷）由这里
 * 兜底。AVIF 的朝向放在容器 irot/imir box 中而非 EXIF，native 解码已按
 * libavif 语义处理；此处不做 EXIF 旋转（AVIF 输入无 EXIF 朝向）。
 */
private fun decodeAvifNative(
    resolver: ContentResolver,
    uri: Uri,
    label: String,
    onError: (message: String) -> Unit
): DecodedRgbImage? {
    if (!NativeCapabilities.avifDecode) {
        onError(avifDecodeErrorMessage(label))
        return null
    }
    val bytes = runCatching {
        resolver.openInputStream(uri)?.use { stream -> stream.readBytes() }
    }.onFailure { throwable ->
        onError("Unable to read AVIF input $uri: ${throwable.message}")
    }.getOrNull() ?: return null
    if (bytes.isEmpty() || bytes.size > MAX_ENCODED_INPUT_BYTES) {
        onError("AVIF input has invalid size (${bytes.size} bytes): $label")
        return null
    }
    val meta = LongArray(3)
    val pixels = runCatching {
        NCNNEngine.nativeDecodeImage(bytes, meta)
    }.onFailure { throwable ->
        onError("Native AVIF decode failed for $label: ${throwable.message}")
    }.getOrNull()
    if (pixels == null) {
        val nativeError = runCatching { NCNNEngine.nativeGetLastError() }
            .getOrNull().orEmpty().trim()
        onError(
            if (nativeError.isNotBlank()) {
                "无法解码 AVIF（native）：$label（$nativeError）"
            } else {
                "无法解码 AVIF（native）：$label"
            }
        )
        return null
    }
    val width = meta[0].toInt()
    val height = meta[1].toInt()
    val rgbByteCount = width.toLong() * height.toLong() * RGB_CHANNELS
    if (width <= 0 || height <= 0 || rgbByteCount > Int.MAX_VALUE ||
        pixels.size != rgbByteCount.toInt()
    ) {
        onError("Native AVIF decode produced invalid output: ${width}x$height, bytes=${pixels.size}")
        return null
    }
    if (!hasSufficientHeapForRgbBuffer(width = width, rgbByteCount = rgbByteCount)) {
        onError("Insufficient heap for decoded AVIF: ${width}x$height")
        return null
    }
    return DecodedRgbImage(width = width, height = height, rgbBytes = pixels)
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

// ── SR 标记探测（planner 跳过已超分输入用）─────────────────────────

/** Parsed fields of a super-resolution marker found on an input image. */
data class SrMarkerInfo(
    val model: String,
    val scale: Int,
    val timestampSec: Long
)

/**
 * Probes [uri] for this tool's own SR marker. Reads the full encoded bytes:
 * in WebP/AVIF the XMP chunk sits after the image data, so a header-only
 * read cannot see it. Returns null when the input is not one of our outputs
 * or cannot be read (both mean "process normally"); [onError] gets a
 * diagnostic only for read failures.
 */
internal fun probeSrMarker(
    resolver: ContentResolver,
    uri: Uri,
    displayName: String,
    onError: (String) -> Unit
): SrMarkerInfo? {
    val bytes = runCatching {
        resolver.openInputStream(uri)?.use { stream -> stream.readBytes() }
    }.onFailure { throwable ->
        onError("Failed to read $displayName for SR marker probe: ${throwable.message}")
    }.getOrNull() ?: return null
    if (bytes.isEmpty()) return null
    val raw = runCatching { NCNNEngine.nativeProbeSrMarker(bytes) }.getOrNull()
        ?: return null
    val parts = raw.split('|')
    if (parts.size != 3) return null
    val scale = parts[1].toIntOrNull() ?: return null
    val timestampSec = parts[2].toLongOrNull() ?: return null
    return SrMarkerInfo(
        model = parts[0],
        scale = scale,
        timestampSec = timestampSec
    )
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

/**
 * Raw encoded image bytes read straight from [uri] — shipped as-is to the PC
 * for distributed jobs, so the phone never decodes (or holds raw pixels) and
 * the radio carries ~1/10 the data. Format comes from the display name's
 * extension, falling back to the content provider's mime type. Returns null
 * on any failure; [onError] receives a diagnostic message.
 *
 * 输入完全不可信（用户可从 SAF 选到任意文件）：读取前先查声明大小，读取后
 * 再校验实际大小，超过 [MAX_ENCODED_INPUT_BYTES] 直接判失败，绝不把超大
 * 文件整个读进堆里 OOM。
 */
internal fun readEncodedBytes(
    resolver: ContentResolver,
    uri: Uri,
    displayName: String,
    onError: (String) -> Unit
): Pair<ByteArray, String>? {
    val ext = displayName.substringAfterLast('.', "").lowercase()
    val format = when (ext) {
        "png", "jpg", "jpeg", "webp", "avif" -> if (ext == "jpeg") "jpg" else ext
        else -> when (resolver.getType(uri)) {
            "image/png" -> "png"
            "image/jpeg" -> "jpg"
            "image/webp" -> "webp"
            "image/avif" -> "avif"
            else -> "jpg"
        }
    }
    // 先查大小（SAF 用 OpenableColumns.SIZE，file:// 用 length()），
    // 声明超限就不开流。声明可能缺失或不准，读完后再兜底校验一次。
    val declaredSize = queryInputSizeBytes(resolver, uri)
    if (declaredSize != null && declaredSize > MAX_ENCODED_INPUT_BYTES) {
        onError(
            "Input exceeds ${MAX_ENCODED_INPUT_BYTES / (1024L * 1024)} MB limit " +
                "($declaredSize bytes): $displayName"
        )
        return null
    }
    return try {
        resolver.openInputStream(uri)?.use { stream ->
            val bytes = stream.readBytes()
            when {
                bytes.isEmpty() -> {
                    onError("Input is empty: $displayName")
                    null
                }
                bytes.size > MAX_ENCODED_INPUT_BYTES -> {
                    onError(
                        "Input exceeds ${MAX_ENCODED_INPUT_BYTES / (1024L * 1024)} MB limit " +
                            "(${bytes.size} bytes): $displayName"
                    )
                    null
                }
                else -> bytes to format
            }
        } ?: run {
            onError("Cannot open input stream: $displayName")
            null
        }
    } catch (e: IOException) {
        onError("Failed to read $displayName: ${e.message}")
        null
    }
}

/** 输入声明大小：file:// 走 [File.length]，content:// 查 [OpenableColumns.SIZE]；未知返回 null。 */
private fun queryInputSizeBytes(resolver: ContentResolver, uri: Uri): Long? {
    if (uri.scheme.equals("file", ignoreCase = true)) {
        val path = uri.path ?: return null
        return runCatching { File(path).length() }.getOrNull()
    }
    return runCatching {
        resolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { cursor ->
            if (!cursor.moveToFirst()) {
                return@use null
            }
            val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
            if (sizeIndex != -1 && !cursor.isNull(sizeIndex)) {
                cursor.getLong(sizeIndex)
            } else {
                null
            }
        }
    }.getOrNull()
}
