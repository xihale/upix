package io.upix.ui.scanner

import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.ByteBuffer

class QrPayloadDecoderTest {
    @Test
    fun decodeYPlane_readsEncodedQr() {
        val payload = """{"v":2,"ip":"192.168.1.100","port":9527}"""
        val (pixels, size) = encodeQrLuma(payload)

        assertEquals(payload, QrPayloadDecoder.decodeYPlane(pixels, size, size, 0))
    }

    @Test
    fun decodeYPlane_readsRotatedQr() {
        val payload = "upix://join?host=10.0.0.11&port=12000"
        val (pixels, size) = encodeQrLuma(payload)
        val (rotated, width, height) = QrPayloadDecoder.rotateY(pixels, size, size, 90)

        assertEquals(payload, QrPayloadDecoder.decodeYPlane(rotated, width, height, 270))
    }

    @Test
    fun decodeYPlane_returnsNullForEmptyLuma() {
        val blank = ByteArray(64 * 64) { 0xFF.toByte() }
        assertNull(QrPayloadDecoder.decodeYPlane(blank, 64, 64, 0))
    }

    @Test
    fun compactYPlane_stripsRowStride() {
        val width = 3
        val height = 2
        val rowStride = 5
        val packed = byteArrayOf(1, 2, 3, 4, 5, 6)
        val strided = ByteArray(rowStride * height)
        packed.copyInto(strided, 0, 0, width)
        packed.copyInto(strided, rowStride, width, packed.size)

        val out = QrPayloadDecoder.compactYPlane(
            buffer = ByteBuffer.wrap(strided),
            width = width,
            height = height,
            rowStride = rowStride,
            pixelStride = 1
        )

        assertArrayEquals(packed, out)
    }

    @Test
    fun rotateY_swapsDimensionsFor90() {
        val src = byteArrayOf(1, 2, 3, 4, 5, 6)
        val (dst, width, height) = QrPayloadDecoder.rotateY(src, 3, 2, 90)

        assertEquals(2, width)
        assertEquals(3, height)
        // 1 2 3      4 1
        // 4 5 6  ->  5 2
        //            6 3
        assertArrayEquals(byteArrayOf(4, 1, 5, 2, 6, 3), dst)
    }

    private fun encodeQrLuma(payload: String): Pair<ByteArray, Int> {
        val size = 192
        val matrix = QRCodeWriter().encode(payload, BarcodeFormat.QR_CODE, size, size)
        val pixels = ByteArray(size * size)
        for (y in 0 until size) {
            for (x in 0 until size) {
                pixels[y * size + x] = if (matrix[x, y]) 0 else 0xFF.toByte()
            }
        }
        return pixels to size
    }
}
