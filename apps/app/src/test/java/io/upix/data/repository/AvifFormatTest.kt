package io.upix.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AvifFormatTest {
    @Test
    fun sniffReadsIspeFromMinimalFtyp() {
        val bytes = avifStub(width = 320, height = 240)
        assertTrue(AvifFormat.looksLike(bytes))
        assertEquals(320 to 240, AvifFormat.sniffDimensions(bytes))
    }

    @Test
    fun pngIsNotAvif() {
        val png = byteArrayOf(
            0x89.toByte(), 'P'.code.toByte(), 'N'.code.toByte(), 'G'.code.toByte(),
            0x0d, 0x0a, 0x1a, 0x0a
        )
        assertFalse(AvifFormat.looksLike(png))
    }

    private fun avifStub(width: Int, height: Int): ByteArray {
        val out = ArrayList<Byte>()
        fun u32(value: Int) {
            out += (value ushr 24).toByte()
            out += (value ushr 16).toByte()
            out += (value ushr 8).toByte()
            out += value.toByte()
        }
        fun fourcc(text: String) {
            text.forEach { out += it.code.toByte() }
        }
        u32(20); fourcc("ftyp"); fourcc("avif"); u32(0); fourcc("avif")
        u32(48); fourcc("meta"); u32(0)
        u32(36); fourcc("iprp")
        u32(28); fourcc("ipco")
        u32(20); fourcc("ispe"); u32(0); u32(width); u32(height)
        return out.toByteArray()
    }
}
