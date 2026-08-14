package com.crossscale

import android.graphics.BitmapFactory
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.crossscale.native.NCNNEngine
import com.crossscale.native.NativeWorker
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.FixMethodOrder
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.MethodSorters

/**
 * End-to-end remote test: the device pairs with a PC coordinator and upscales
 * an image through it over the encrypted distributed protocol.
 *
 * Requires a coordinator running on the host machine reachable from the device:
 *   crossscale-cli server start -p <port> --auto-approve
 * (run from a directory whose ./models holds a model). The host/port/model are
 * passed as instrumentation arguments, e.g. via am instrument:
 *   adb shell am instrument -w \
 *     -e host 192.168.1.10 -e port 9530 -e model Real-ESRGANv3-anime-x2 \
 *     -e class com.crossscale.RemoteDistributedTest \
 *     com.crossscale.test/androidx.test.runner.AndroidJUnitRunner
 *
 * Orchestration lives in scripts/test_remote_e2e.sh.
 */
@RunWith(AndroidJUnit4::class)
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class RemoteDistributedTest {

    private fun arg(key: String, default: String): String =
        InstrumentationRegistry.getArguments().getString(key, default)

    private fun host(): String = arg("host", "").trim()
    private fun port(): Int = arg("port", "9530").trim().toIntOrNull() ?: 9530
    private fun model(): String = arg("model", "Real-ESRGANv3-anime-x2")

    private fun requireHost() {
        val h = host()
        assertTrue(
            "No host supplied. Pass -e host <pc-lan-ip> (and optionally -e port/-e model).",
            h.isNotBlank()
        )
    }

    /** TOFU-pair with the coordinator; returns the learned PC identity pubkey. */
    private fun pair(): String {
        val worker = NativeWorker()
        val learnedPubKey = worker.requestConnection(
            host = host(),
            port = port(),
            peerPubKeyB64 = "",      // TOFU: learn the PC key on first contact
            wantsEncryptedStream = true
        )
        assertNotNull(
            "Pairing failed (PC unreachable, rejected, or MITM mismatch). " +
                "Check the coordinator is running with --auto-approve and the " +
                "device can reach ${host()}:${port()}.",
            learnedPubKey
        )
        assertTrue("Learned pubkey must be non-empty base64", learnedPubKey!!.isNotBlank())
        return learnedPubKey
    }

    // 64x64 RGB synthetic image (deterministic gradient) reused across formats.
    private val syntheticRgb: ByteArray by lazy {
        val size = 64
        val rgb = ByteArray(size * size * 3)
        var i = 0
        for (y in 0 until size) {
            for (x in 0 until size) {
                rgb[i++] = (x * 4 and 0xFF).toByte()
                rgb[i++] = (y * 4 and 0xFF).toByte()
                rgb[i++] = ((x + y) and 0xFF).toByte()
            }
        }
        rgb
    }

    /** Assert the bytes start with the magic header of the requested format. */
    private fun assertEncodedFormat(bytes: ByteArray, format: String) {
        fun b(i: Int) = bytes[i]
        fun eq(i: Int, v: Int) = bytes.size > i && b(i) == v.toByte()
        val ok = when (format) {
            "jpg"  -> eq(0, 0xFF) && eq(1, 0xD8) && eq(2, 0xFF)
            "png"  -> eq(0, 0x89) && eq(1, 0x50) && eq(2, 0x4E) && eq(3, 0x47) // ‰PNG
            "webp" -> eq(0, 0x52) && eq(1, 0x49) && eq(2, 0x46) && eq(3, 0x46) && // RIFF
                      eq(8, 0x57) && eq(9, 0x45) && eq(10, 0x42) && eq(11, 0x50)  // WEBP
            else   -> false
        }
        val head = bytes.take(4).joinToString("") { "%02x".format(it) }
        assertTrue("result is not $format (first 4 bytes: $head)", ok)
    }

    /**
     * Pair, submit a 64x64 image requesting [format], and assert the PC returns
     * bytes in exactly that format, decodable, and upscaled (larger than input).
     */
    private fun roundTripInFormat(format: String) {
        requireHost()
        val key = pair()
        val encoded: ByteArray? = NCNNEngine.nativeProcessImageDistributed(
            host = host(),
            port = port(),
            peerPubKeyB64 = key,
            wantsEncryptedStream = true,
            modelName = model(),
            tileSize = 64,
            tileOverlap = 8,
            inputImage = syntheticRgb,
            width = 64,
            height = 64,
            channels = 3,
            outputFormat = format,
            outputQuality = 95   // ignored by lossless PNG; honors jpg/webp
        )
        assertNotNull(
            "submit($format) returned null. Last native error: " +
                "${runCatching { NCNNEngine.nativeGetLastError() }.getOrNull()}",
            encoded
        )
        assertTrue("submit($format) returned empty bytes", encoded!!.isNotEmpty())

        // The PC must have encoded in the requested format, not silently fallen
        // back to jpg — verify via the magic header.
        assertEncodedFormat(encoded, format)

        // And the result must decode + be upscaled beyond the 64x64 input.
        val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(encoded, 0, encoded.size, opts)
        assertTrue(
            "submit($format) did not decode (outWidth=${opts.outWidth})",
            opts.outWidth > 64 && opts.outHeight > 64
        )
    }

    /** Phase 1 — pairing only (no submit). */
    @Test
    fun a_pairing_succeeds() {
        requireHost()
        pair()
    }

    /** Phase 2 — distributed round-trip, one method per output format. */
    @Test
    fun b_distributedUpscale_jpg() = roundTripInFormat("jpg")

    @Test
    fun c_distributedUpscale_png() = roundTripInFormat("png")

    @Test
    fun d_distributedUpscale_webp() = roundTripInFormat("webp")
}
