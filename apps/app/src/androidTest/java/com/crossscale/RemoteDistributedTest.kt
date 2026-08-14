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

    /**
     * Phase 1 — pairing. TOFU ECDH handshake + CONNECT_REQUEST; the PC
     * auto-approves (--auto-approve), the phone learns the PC identity key.
     * Asserts a non-null base64 pubkey is returned.
     */
    @Test
    fun a_pairing_succeeds() {
        requireHost()
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
        assertTrue(
            "Learned pubkey must be non-empty base64",
            learnedPubKey!!.isNotBlank()
        )
    }

    /**
     * Phase 2 — full distributed round-trip. Pairs (learns key), then submits a
     * small synthetic image; asserts the PC returns a decodable upscaled result.
     */
    @Test
    fun b_distributedUpscale_roundTrip() {
        requireHost()
        val worker = NativeWorker()
        val learnedPubKey = worker.requestConnection(
            host = host(),
            port = port(),
            peerPubKeyB64 = "",
            wantsEncryptedStream = true
        )
        assertNotNull("Pairing failed before submit; see a_pairing_succeeds", learnedPubKey)

        // 64x64 RGB synthetic image (deterministic gradient).
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

        val encoded: ByteArray? = NCNNEngine.nativeProcessImageDistributed(
            host = host(),
            port = port(),
            peerPubKeyB64 = learnedPubKey!!,
            wantsEncryptedStream = true,
            modelName = model(),
            tileSize = 64,
            tileOverlap = 8,
            inputImage = rgb,
            width = size,
            height = size,
            channels = 3,
            outputFormat = "jpg",
            outputQuality = 90
        )
        assertNotNull(
            "Distributed submit returned null (PC rejected/errored). " +
                "Last native error: ${runCatching { NCNNEngine.nativeGetLastError() }.getOrNull()}",
            encoded
        )
        assertTrue("Distributed submit returned empty bytes", encoded!!.isNotEmpty())

        // The result must decode and be upscaled (larger than the 64x64 input).
        val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(encoded, 0, encoded.size, opts)
        assertTrue(
            "Result did not decode as an image (outWidth=${opts.outWidth})",
            opts.outWidth > size
        )
        assertTrue(
            "Result was not upscaled (outWidth=${opts.outWidth} <= $size)",
            opts.outWidth > size && opts.outHeight > size
        )
    }
}
