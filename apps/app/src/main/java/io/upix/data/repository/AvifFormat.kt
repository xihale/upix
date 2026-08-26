package io.upix.data.repository

/**
 * ISOBMFF helpers for AVIF. Used when BitmapFactory cannot decode the
 * container (API 26–30) so remote-produced `.avif` bytes can still be
 * dimension-checked and saved.
 */
internal object AvifFormat {
    fun looksLike(bytes: ByteArray): Boolean {
        if (bytes.size < 16) {
            return false
        }
        if (!typeEquals(bytes, 4, "ftyp")) {
            return false
        }
        return hasBrand(bytes, "avif") || hasBrand(bytes, "avis")
    }

    fun sniffDimensions(bytes: ByteArray): Pair<Int, Int>? {
        if (!looksLike(bytes)) {
            return null
        }
        val found = intArrayOf(0, 0)
        walkBoxes(bytes, 0, bytes.size, found, 0)
        val width = found[0]
        val height = found[1]
        if (width <= 0 || height <= 0) {
            return null
        }
        return width to height
    }

    private fun hasBrand(bytes: ByteArray, brand: String): Boolean {
        val boxSize = be32(bytes, 0).toLong()
        if (boxSize < 16L) {
            return false
        }
        val limit = minOf(bytes.size.toLong(), boxSize)
        var offset = 8
        while (offset + 4 <= limit) {
            if (typeEquals(bytes, offset, brand)) {
                return true
            }
            offset += 4
        }
        return false
    }

    private fun walkBoxes(
        bytes: ByteArray,
        begin: Int,
        end: Int,
        found: IntArray,
        depth: Int
    ) {
        if (depth > 12 || begin >= end) {
            return
        }
        var offset = begin
        while (offset + 8 <= end) {
            val size32 = be32(bytes, offset)
            var header = 8
            var boxSize = size32.toLong()
            if (size32 == 1) {
                if (offset + 16 > end) {
                    return
                }
                boxSize = (be32(bytes, offset + 8).toLong() shl 32) or
                    be32(bytes, offset + 12).toLong()
                header = 16
            } else if (size32 == 0) {
                boxSize = (end - offset).toLong()
            }
            if (boxSize < header.toLong() || offset + boxSize > end) {
                return
            }
            val payloadBegin = offset + header
            val payloadEnd = offset + boxSize.toInt()
            if (typeEquals(bytes, offset + 4, "ispe") && payloadEnd - payloadBegin >= 12) {
                found[0] = be32(bytes, payloadBegin + 4)
                found[1] = be32(bytes, payloadBegin + 8)
                return
            }
            if (typeEquals(bytes, offset + 4, "meta") && payloadEnd >= payloadBegin + 4) {
                walkBoxes(bytes, payloadBegin + 4, payloadEnd, found, depth + 1)
            } else if (
                typeEquals(bytes, offset + 4, "iprp") ||
                typeEquals(bytes, offset + 4, "ipco")
            ) {
                walkBoxes(bytes, payloadBegin, payloadEnd, found, depth + 1)
            }
            if (found[0] > 0 && found[1] > 0) {
                return
            }
            offset = payloadEnd
        }
    }

    private fun typeEquals(bytes: ByteArray, offset: Int, type: String): Boolean {
        if (offset + 4 > bytes.size || type.length != 4) {
            return false
        }
        return bytes[offset] == type[0].code.toByte() &&
            bytes[offset + 1] == type[1].code.toByte() &&
            bytes[offset + 2] == type[2].code.toByte() &&
            bytes[offset + 3] == type[3].code.toByte()
    }

    private fun be32(bytes: ByteArray, offset: Int): Int {
        return ((bytes[offset].toInt() and 0xFF) shl 24) or
            ((bytes[offset + 1].toInt() and 0xFF) shl 16) or
            ((bytes[offset + 2].toInt() and 0xFF) shl 8) or
            (bytes[offset + 3].toInt() and 0xFF)
    }
}
