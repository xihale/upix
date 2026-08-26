package io.upix.ui.scanner

import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import java.nio.ByteBuffer

internal object QrPayloadDecoder {
    private val hints: Map<DecodeHintType, Any> = mapOf(
        DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE),
        DecodeHintType.TRY_HARDER to true,
        DecodeHintType.CHARACTER_SET to "UTF-8"
    )

    fun decodeYPlane(
        yPlane: ByteArray,
        width: Int,
        height: Int,
        rotationDegrees: Int
    ): String? {
        val (data, w, h) = rotateY(yPlane, width, height, rotationDegrees)
        val source = PlanarYUVLuminanceSource(data, w, h, 0, 0, w, h, false)
        val bitmap = BinaryBitmap(HybridBinarizer(source))
        val reader = QRCodeReader()
        return try {
            reader.decode(bitmap, hints).text?.trim()?.takeIf { it.isNotEmpty() }
        } catch (_: ReaderException) {
            null
        } finally {
            reader.reset()
        }
    }

    fun compactYPlane(
        buffer: ByteBuffer,
        width: Int,
        height: Int,
        rowStride: Int,
        pixelStride: Int
    ): ByteArray {
        val src = buffer.duplicate()
        val packed = width * height
        if (pixelStride == 1 && rowStride == width && src.remaining() >= packed) {
            val out = ByteArray(packed)
            src.get(out)
            return out
        }
        val out = ByteArray(packed)
        var offset = 0
        for (row in 0 until height) {
            val rowStart = row * rowStride
            if (pixelStride == 1) {
                src.position(rowStart)
                src.get(out, offset, width)
                offset += width
            } else {
                for (col in 0 until width) {
                    out[offset++] = src.get(rowStart + col * pixelStride)
                }
            }
        }
        return out
    }

    internal fun rotateY(
        src: ByteArray,
        width: Int,
        height: Int,
        rotationDegrees: Int
    ): Triple<ByteArray, Int, Int> {
        val rotation = ((rotationDegrees % 360) + 360) % 360
        when (rotation) {
            0 -> return Triple(src, width, height)
            90 -> {
                val dst = ByteArray(width * height)
                for (y in 0 until height) {
                    for (x in 0 until width) {
                        dst[x * height + (height - 1 - y)] = src[y * width + x]
                    }
                }
                return Triple(dst, height, width)
            }
            180 -> {
                val dst = ByteArray(width * height)
                for (i in src.indices) {
                    dst[dst.lastIndex - i] = src[i]
                }
                return Triple(dst, width, height)
            }
            270 -> {
                val dst = ByteArray(width * height)
                for (y in 0 until height) {
                    for (x in 0 until width) {
                        dst[(width - 1 - x) * height + y] = src[y * width + x]
                    }
                }
                return Triple(dst, height, width)
            }
            else -> return Triple(src, width, height)
        }
    }
}
