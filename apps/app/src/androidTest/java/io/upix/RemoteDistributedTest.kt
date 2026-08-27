package io.upix

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.ByteArrayOutputStream
import java.io.File
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.upix.nativelib.NCNNEngine
import io.upix.nativelib.NativeWorker
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
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
 *   upix-cli host -p <port> --auto-approve
 * (run from a directory whose ./models holds a model). The host/port/model are
 * passed as instrumentation arguments, e.g. via am instrument:
 *   adb shell am instrument -w \
 *     -e host 192.168.1.10 -e port 9530 -e model Real-ESRGANv3-anime-x2 \
 *     -e class io.upix.RemoteDistributedTest \
 *     io.upix.test/androidx.test.runner.AndroidJUnitRunner
 *
 * Orchestration: run the `am instrument` command above directly (see
 * scripts/android_test.sh for the general build/unit/smoke harness).
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
        val learnedPubKey = NativeWorker.nativeRequestConnection(
            host = host(),
            port = port(),
            peerPubKeyB64 = ""      // TOFU: learn the PC key on first contact
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

    // 64x64 synthetic image (deterministic gradient), PNG-encoded: the new
    // wire protocol ships encoded file bytes and the PC decodes them.
    private val syntheticRgb: ByteArray by lazy {
        val size = 64
        val pixels = IntArray(size * size) { i ->
            val x = i % size
            val y = i / size
            (0xFF shl 24) or ((x * 4 and 0xFF) shl 16) or ((y * 4 and 0xFF) shl 8) or ((x + y) and 0xFF)
        }
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        bmp.setPixels(pixels, 0, size, 0, 0, size, size)
        val out = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
        bmp.recycle()
        out.toByteArray()
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
            "avif" -> eq(4, 'f'.code) && eq(5, 't'.code) && eq(6, 'y'.code) && eq(7, 'p'.code) &&
                      (eq(8, 'a'.code) && eq(9, 'v'.code) && eq(10, 'i'.code) && eq(11, 'f'.code) ||
                          bytes.take(32).windowed(4).any { it[0] == 'a'.toByte() && it[1] == 'v'.toByte() && it[2] == 'i'.toByte() && it[3] == 'f'.toByte() })
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
        // Persistent session: one handshake per batch (the P0 long-connection
        // model), then send + blocking receive for this single task.
        val handle: Long = NCNNEngine.nativeOpenDistributedSession(
            host = host(), port = port(), peerPubKeyB64 = key
        )
        assertTrue(
            "openDistributedSession failed. Last native error: " +
                "${runCatching { NCNNEngine.nativeGetLastError() }.getOrNull()}",
            handle != 0L
        )
        try {
            val sent = NCNNEngine.nativeSendDistributedTask(
                handle = handle,
                modelName = model(),
                tileSize = 64,
                tileOverlap = 8,
                inputFormat = "png",
                inputImage = syntheticRgb,
                outputFormat = format,
                outputQuality = 95,  // ignored by lossless PNG; honors jpg/webp
                encoderOptionsJson = "",
                allowChunked = true
            )
            val encoded: ByteArray? = if (!sent) null else
                NCNNEngine.nativeReceiveDistributedResult(handle, null)
            assertNotNull(
                "submit($format) returned null. Last native error: " +
                    "${runCatching { NCNNEngine.nativeGetLastError() }.getOrNull()}",
                encoded
            )
        assertTrue("submit($format) returned empty bytes", encoded!!.isNotEmpty())

        // The PC must have encoded in the requested format, not silently fallen
        // back to jpg — verify via the magic header.
        assertEncodedFormat(encoded, format)

        // Remote outputs carry this tool's SR XMP marker (the PC-side encoder
        // stamps it), so feeding the result back is recognized and skipped.
        assertNotNull(
            "submit($format) result carries no SR marker",
            NCNNEngine.nativeProbeSrMarker(encoded)
        )

        // And the result must decode + be upscaled beyond the 64x64 input.
        val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(encoded, 0, encoded.size, opts)
        var outWidth = opts.outWidth
        var outHeight = opts.outHeight
        if (outWidth <= 0 && format == "avif") {
            val ispe = io.upix.data.repository.AvifFormat.sniffDimensions(encoded)
            outWidth = ispe?.first ?: 0
            outHeight = ispe?.second ?: 0
        }
        assertTrue(
            "submit($format) did not decode (outWidth=$outWidth)",
            outWidth > 64 && outHeight > 64
        )
        } finally {
            NCNNEngine.nativeCloseDistributedSession(handle)
        }
    }

    /** Phase 1 — pairing only (no submit). */
    @Test
    fun a_pairing_succeeds() {
        requireHost()
        pair()
    }

    /**
     * Phase 3 — batch timing over one persistent session, replicating the
     * app's depth-1 pipeline ordering (prefetch file bytes of N+1 → await
     * result N → upload N+1 → persist N). Logs one CSREMOTE line per image:
     *
     *   CSREMOTE <i>/<n> read=<ms> upload=<ms> wait=<ms> persist=<ms> total=<ms>
     *
     * upload happens after wait completes (same thread as the app); read
     * runs during PC inference. Use adb reverse tcp:port to reach a host
     * coordinator without LAN.
     */
    @Test
    fun z_remote_batch_timing() {
        requireHost()
        val count = arg("count", "3").trim().toIntOrNull() ?: 3
        val side = arg("side", "256").trim().toIntOrNull() ?: 256
        val expectedScale = { arg("scale", "2").trim().toIntOrNull() ?: 2 }
        val key = pair()

        fun makePngBytes(size: Int): ByteArray {
            val pixels = IntArray(size * size) { i ->
                val x = i % size
                val y = i / size
                (0xFF shl 24) or ((x * 4 and 0xFF) shl 16) or ((y * 4 and 0xFF) shl 8) or ((x + y) and 0xFF)
            }
            val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            bmp.setPixels(pixels, 0, size, 0, 0, size, size)
            val out = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
            bmp.recycle()
            return out.toByteArray()
        }

        data class Timed(val read: Long, val upload: Long, val wait: Long, val persist: Long, val total: Long)

        val handle = NCNNEngine.nativeOpenDistributedSession(
            host = host(), port = port(), peerPubKeyB64 = key
        )
        assertTrue("openDistributedSession failed", handle != 0L)
        try {
            // 真实照片模式：inputDir 指向设备目录，逐文件原字节上传
            // （与 app 相同——协议传编码文件而非解码像素）。
            data class RealInput(
                val name: String, val bytes: ByteArray,
                val fmt: String, val w: Int, val h: Int
            )
            val inputDir = arg("inputDir", "")
            val realInputs = if (inputDir.isNotBlank()) {
                File(inputDir).listFiles()!!
                    .filter { it.isFile && it.extension.lowercase() in setOf("jpg", "jpeg", "png", "webp") }
                    .sortedBy { it.name }
                    .map { f ->
                        val bytes = f.readBytes()
                        val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts)
                        RealInput(f.name, bytes,
                            if (f.extension.lowercase() == "png") "png" else "jpg",
                            opts.outWidth, opts.outHeight)
                    }
            } else emptyList()
            assertTrue("inputDir 无可用图片", inputDir.isBlank() || realInputs.size > 0)
            val effectiveCount = if (realInputs.size > 0) realInputs.size else count

            // 预取槽：read 在 PC 推理期间发生（应用内一致）。
            var pendingReadMs = 0L
            var currentReal: RealInput? = null

            fun primeRead(index: Int): ByteArray {
                val t0 = System.nanoTime()
                val bytes = if (realInputs.isNotEmpty()) {
                    currentReal = realInputs[index]
                    currentReal!!.bytes
                } else {
                    makePngBytes(side)
                }
                pendingReadMs = (System.nanoTime() - t0) / 1_000_000
                return bytes
            }

            var nextIndex = 0
            var inputBytes = primeRead(nextIndex++)
            var uploaded = false

            fun send(bytes: ByteArray, fmt: String): Boolean = NCNNEngine.nativeSendDistributedTask(
                handle = handle,
                modelName = model(),
                tileSize = 0,
                tileOverlap = 8,
                inputFormat = fmt,
                inputImage = bytes,
                outputFormat = "jpg",
                outputQuality = 90,
                encoderOptionsJson = "",
                allowChunked = true
            )

            for (i in 0 until effectiveCount) {
                val t0 = System.nanoTime()
                val cur = currentReal
                assertTrue("send #$i failed", send(inputBytes, cur?.fmt ?: "png"))
                val uploadMs = (System.nanoTime() - t0) / 1_000_000
                uploaded = true

                val tWait0 = System.nanoTime()
                val encoded = NCNNEngine.nativeReceiveDistributedResult(handle, null)
                val waitMs = (System.nanoTime() - tWait0) / 1_000_000
                assertNotNull("receive #$i failed: ${NCNNEngine.nativeGetLastError()}", encoded)

                val tPersist0 = System.nanoTime()
                assertEncodedFormat(encoded!!, "jpg")
                val outOpts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeByteArray(encoded, 0, encoded.size, outOpts)
                var dimOk = "?"
                if (cur != null) {
                    // 远程输出必须恰好是输入的 scale 倍（按会话模型实际 scale）
                    dimOk = "${outOpts.outWidth}x${outOpts.outHeight}"
                    val expW = cur.w * expectedScale()
                    val expH = cur.h * expectedScale()
                    assertTrue("输出尺寸不符 (${cur.name}): ${outOpts.outWidth}x${outOpts.outHeight} want $expW x $expH",
                        outOpts.outWidth == expW && outOpts.outHeight == expH)
                } else {
                    BitmapFactory.decodeByteArray(encoded, 0, encoded.size)
                }
                val persistMs = (System.nanoTime() - tPersist0) / 1_000_000

                val total = pendingReadMs + uploadMs + waitMs + persistMs
                android.util.Log.i(
                    "Upix",
                    "CSREMOTE $i/$effectiveCount read=$pendingReadMs upload=$uploadMs " +
                        "wait=$waitMs persist=$persistMs total=$total bytes=${encoded.size}" +
                        (if (cur != null) " src=${cur.name} dims=$dimOk" else "")
                )
                val dumpDir = arg("outDir", "")
                if (dumpDir.isNotBlank()) {
                    File(dumpDir).mkdirs()
                    val nm = cur?.name ?: "synthetic-$i.png"
                    File(dumpDir, "remote-" + nm.substringBeforeLast('.') + ".jpg").writeBytes(encoded)
                }

                if (nextIndex < effectiveCount) {
                    inputBytes = primeRead(nextIndex++)
                }
            }
            check(uploaded)
        } finally {
            NCNNEngine.nativeCloseDistributedSession(handle)
        }
    }

    /** Phase 2 — distributed round-trip, one method per output format. */
    @Test
    fun b_distributedUpscale_jpg() = roundTripInFormat("jpg")

    @Test
    fun c_distributedUpscale_png() = roundTripInFormat("png")

    @Test
    fun d_distributedUpscale_webp() = roundTripInFormat("webp")

    @Test
    fun e_distributedUpscale_avif() = roundTripInFormat("avif")

    /**
     * Depth-2 submit-ahead window (requires the PC to advertise CONNECT_
     * RESPONSE.pipeline_depth >= 1): task N+1 streams up the wire while task N
     * still computes, so every image's cycle collapses onto PC inference time.
     * One CSPIPE row per image:
     *   CSPIPE <i>/<n> upload=<ms> wait=<ms> persist=<ms> total=<ms>
     * upload runs CONCURRENTLY with the previous wait (started before it).
     */
    @Test
    fun f_pipeline_window_timing() {
        requireHost()
        val key = pair()

        // Inputs: real photos when -e inputDir is given, else synthetic frames.
        data class Item(val displayName: String, val bytes: ByteArray)
        val items: List<Item>
        val dirArg = arg("inputDir", "").trim()
        if (dirArg.isNotEmpty()) {
            val ctx = InstrumentationRegistry.getInstrumentation().targetContext
            val dirFile = resolveInputDir(dirArg, ctx)
            val names = dirFile.list { _, f ->
                f.lowercase().endsWith(".jpg") || f.lowercase().endsWith(".jpeg") ||
                    f.lowercase().endsWith(".png") || f.lowercase().endsWith(".webp")
            }?.sorted() ?: emptyList()
            assertTrue("no images in $dirArg", names.isNotEmpty())
            items = names.map { name ->
                val bytes = File(dirFile, name).readBytes()
                Item(name, bytes)
            }
        } else {
            val n = arg("count", "4").trim().toIntOrNull() ?: 4
            val side = arg("side", "512").trim().toIntOrNull() ?: 512
            items = (0 until n).map { i ->
                Item("synthetic-$i.png", makeNoisePng(side, i))
            }
        }

        val handle = NCNNEngine.nativeOpenDistributedSession(
            host = host(), port = port(), peerPubKeyB64 = key
        )
        assertTrue("openDistributedSession failed", handle != 0L)

        fun formatOf(name: String): String = when {
            name.lowercase().endsWith(".png") -> "png"
            name.lowercase().endsWith(".webp") -> "webp"
            else -> "jpg"
        }

        fun send(item: Item): Boolean = NCNNEngine.nativeSendDistributedTask(
            handle = handle,
            modelName = model(),
            tileSize = 0,
            tileOverlap = 8,
            inputFormat = formatOf(item.displayName),
            inputImage = item.bytes,
            outputFormat = "jpg",
            outputQuality = 90,
            encoderOptionsJson = "",
            allowChunked = true
        )

        var outDir: File? = null
        try {
            val dumpOut = arg("outDir", "").trim()
            if (dumpOut.isNotEmpty()) {
                outDir = File(dumpOut).apply { mkdirs() }
            }

            val uploadMs = LongArray(items.size)
            val tPrime0 = System.nanoTime()
            assertTrue("send #0 failed", send(items[0]))
            uploadMs[0] = (System.nanoTime() - tPrime0) / 1_000_000
            var nextToSend = 1
            val uploader = java.util.concurrent.Executors.newSingleThreadExecutor()

            try {
                for (i in items.indices) {
                    // Keep ≤2 tasks on the wire: start uploading N+1 BEFORE
                    // waiting on N's result (this is the whole optimization).
                    val fut = if (nextToSend < items.size) {
                        val k = nextToSend++
                        uploader.submit(java.util.concurrent.Callable {
                            val t0 = System.nanoTime()
                            val ok = send(items[k])
                            uploadMs[k] = (System.nanoTime() - t0) / 1_000_000
                            ok
                        })
                    } else {
                        null
                    }

                    val tWait0 = System.nanoTime()
                    val encoded = NCNNEngine.nativeReceiveDistributedResult(handle, null)
                    val waitMs = (System.nanoTime() - tWait0) / 1_000_000
                    assertNotNull(
                        "receive #$i failed: " +
                            runCatching { NCNNEngine.nativeGetLastError() }.getOrNull(),
                        encoded
                    )

                    val tPersist0 = System.nanoTime()
                    BitmapFactory.decodeByteArray(encoded!!, 0, encoded.size)
                    outDir?.let { File(it, "pipe-${items[i].displayName}.jpg").writeBytes(encoded) }
                    val persistMs = (System.nanoTime() - tPersist0) / 1_000_000

                    if (fut != null) {
                        // Upload of item i+1 was launched before the wait;
                        // confirm it went out cleanly once this item is done.
                        assertTrue("pipelined send #$nextToSend failed", fut.get())
                    }
                    android.util.Log.i(
                        "Upix",
                        "CSPIPE $i/${items.size} upload=${uploadMs[i]} " +
                            "wait=$waitMs persist=$persistMs total=" +
                            "${uploadMs[i] + waitMs + persistMs} " +
                            "bytes=${encoded.size} (${items[i].displayName})"
                    )
                }
            } finally {
                uploader.shutdown()
            }
        } finally {
            NCNNEngine.nativeCloseDistributedSession(handle)
        }
    }

    /**
     * Cancel while a task is in flight AND another is being uploaded ahead:
     * the blocked receive must return promptly with a "Distributed task
     * canceled..." error. Guards the cancel-flag lifetime against concurrent
     * submit-ahead sends (a send used to wipe cancel_requested before the
     * receive loop could observe it).
     */
    @Test
    fun g_cancel_during_pipeline_wait() {
        requireHost()
        val key = pair()
        val handle = NCNNEngine.nativeOpenDistributedSession(
            host = host(), port = port(), peerPubKeyB64 = key
        )
        assertTrue("openDistributedSession failed", handle != 0L)
        try {
            val bigTask = makeNoisePng(1400, 7)
            val sentA = NCNNEngine.nativeSendDistributedTask(
                handle = handle,
                modelName = model(),
                tileSize = 64,
                tileOverlap = 8,
                inputFormat = "png",
                inputImage = bigTask,
                outputFormat = "jpg",
                outputQuality = 90,
                encoderOptionsJson = "",
                allowChunked = false
            )
            assertTrue("send #0 failed", sentA)

            // The slow task should be computing now. Fire off an ahead-upload
            // on another thread exactly like the depth-2 engine does…
            val ahead = Thread {
                runCatching {
                    NCNNEngine.nativeSendDistributedTask(
                        handle = handle,
                        modelName = model(),
                        tileSize = 64,
                        tileOverlap = 8,
                        inputFormat = "png",
                        inputImage = makeNoisePng(900, 11),
                        outputFormat = "jpg",
                        outputQuality = 90,
                        encoderOptionsJson = "",
                        allowChunked = true
                    )
                }
            }
            ahead.start()
            // …and hit cancel WHILE the receive below is polling.
            Thread {
                runCatching { Thread.sleep(300) }
                runCatching { NCNNEngine.nativeCancelDistributedTask(handle) }
            }.start()

            val t0 = System.currentTimeMillis()
            val encoded = NCNNEngine.nativeReceiveDistributedResult(handle, null)
            val elapsed = System.currentTimeMillis() - t0
            ahead.join(5000)

            assertNull(
                "expected a canceled receive, got ${encoded?.size} bytes",
                encoded
            )
            val reason = runCatching { NCNNEngine.nativeGetLastError() }.getOrNull().orEmpty()
            // Same classification as CoordinatorClient.awaitOutcome: the PC's
            // graceful abort arrives as a generic failure whose message is
            // exactly "canceled"; only connection-loss cancels use the
            // dedicated "Distributed task canceled..." prefix.
            val taskRejected = reason.startsWith("Distributed task failed:")
            assertTrue(
                "error '$reason' is not a cancel (after ${elapsed}ms)",
                reason.startsWith("Distributed task canceled") ||
                    (taskRejected && reason.removePrefix("Distributed task failed:").trim() == "canceled")
            )
            assertTrue("cancel took ${elapsed}ms — not prompt", elapsed < 30000)
            android.util.Log.i("Upix", "CSPIPE cancel OK after ${elapsed}ms")
        } finally {
            NCNNEngine.nativeCloseDistributedSession(handle)
        }
    }

    /** Deterministic pseudo-noise PNG (compresses poorly → predictable size). */
    private fun makeNoisePng(size: Int, seed: Int): ByteArray {
        val pixels = IntArray(size * size) { i ->
            val x = i % size
            val y = i / size
            val v = (x * y + x * 17 + y * 23 + seed * 977) and 0xFF
            (0xFF shl 24) or (v shl 16) or ((v * 3) and 0xFF shl 8) or ((v * 7) and 0xFF)
        }
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        bmp.setPixels(pixels, 0, size, 0, 0, size, size)
        val out = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
        bmp.recycle()
        return out.toByteArray()
    }

    /** inputDir resolution shared by the batch tests (device-local or pushed). */
    private fun resolveInputDir(raw: String, context: android.content.Context): File {
        val f = File(raw)
        return if (f.isDirectory) f
        else File(context.getExternalFilesDir(null), raw).also {
            assertTrue("inputDir not found: $raw", it.isDirectory)
        }
    }
}
