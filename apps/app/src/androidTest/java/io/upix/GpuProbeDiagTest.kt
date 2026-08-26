package io.upix

import android.graphics.BitmapFactory
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.upix.nativelib.NCNNEngine
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest

/**
 * Headless on-device GPU probe diagnostic. Replays the exact synthetic
 * validation probe the app uses before trusting a GPU session, against a
 * chosen model, with optional engine env overrides and tile sizes.
 *
 *   adb shell am instrument -w \
 *     -e model cugan -e sizes 96,192 -e env "UPIX_GPU_TILE_SIZE=512" \
 *     -e class io.upix.GpuProbeDiagTest \
 *     io.upix.test/androidx.test.runner.AndroidJUnitRunner
 *
 * Lines under tag "Upix" prefix "CSDIAG":
 *   CSDIAG models <param paths found>
 *   CSDIAG env K=V ...
 *   CSDIAG init <gpu|cpu> ms=<t> err=<lastError>
 *   CSDIAG run  <gpu|cpu> <model> <WxH> ms=<t> bytes=<n> md5=<hex> err=<lastError>
 *   CSDIAG luma <gpu|cpu> <WxH> min=<v> max=<v> std=<v> mean=<v>
 *   CSDIAG file <device> <absolute path>
 *   CSDIAG md5-match gpu-vs-cpu <WxH> equal=<0|1>
 */
@RunWith(AndroidJUnit4::class)
class GpuProbeDiagTest {

    private fun arg(key: String, default: String): String =
        InstrumentationRegistry.getArguments().getString(key, default).trim()

    private fun findModels(): List<File> {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val dirs = ArrayList<File>()
        ctx.getExternalFilesDir(android.os.Environment.DIRECTORY_DOCUMENTS)?.let { dirs.add(File(it, "models")) }
        dirs.add(File(ctx.filesDir, "models"))
        return dirs.flatMap { dir ->
            dir.walkTopDown().filter { it.isFile && it.name.endsWith(".param") }.toList()
        }.sorted()
    }

    /** Same generator as NativeImageProcessingRepository.buildValidationInput. */
    private fun probeRgb(size: Int): ByteArray {
        val data = ByteArray(size * size * 3)
        var i = 0
        for (y in 0 until size) {
            for (x in 0 until size) {
                val r = (x * 255) / (size - 1)
                val g = (y * 255) / (size - 1)
                val checker = if (((x / 8) + (y / 8)) % 2 == 0) 60 else 0
                val b = (((255 - r) + checker) / 2).coerceIn(0, 255)
                data[i++] = r.toByte()
                data[i++] = g.toByte()
                data[i++] = b.toByte()
            }
        }
        return data
    }

    private fun md5(bytes: ByteArray): String =
        MessageDigest.getInstance("MD5").digest(bytes).joinToString("") { "%02x".format(it) }

    private fun log(msg: String) = android.util.Log.i("Upix", "CSDIAG $msg")

    private fun lumaStats(encoded: ByteArray): String {
        val bmp = BitmapFactory.decodeByteArray(encoded, 0, encoded.size) ?: return "decode=failed"
        val w = bmp.width
        val h = bmp.height
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        var mn = 255
        var mx = 0
        var sum = 0.0
        var sq = 0.0
        for (p in px) {
            val l = (((p shr 16 and 0xFF) * 299 + (p shr 8 and 0xFF) * 587 + (p and 0xFF) * 114) / 1000)
            if (l < mn) mn = l
            if (l > mx) mx = l
            sum += l
            sq += l.toDouble() * l
        }
        val mean = sum / px.size
        val std = kotlin.math.sqrt(kotlin.math.max(0.0, sq / px.size - mean * mean))
        return "min=$mn max=$mx std=%.2f mean=%.1f".format(std, mean)
    }

    private data class RunResult(val md5: String, val bytes: Int, val ms: Long, val file: File?)

    private fun runOnce(device: Int, deviceName: String, modelBase: String, modelName: String,
                        rgb: ByteArray, size: Int, save: Boolean): RunResult? {
        val t0 = System.nanoTime()
        val ptr = NCNNEngine.nativeInit(device)
        val initMs = (System.nanoTime() - t0) / 1_000_000
        if (ptr == 0L) {
            log("init $deviceName ms=$initMs err=${NCNNEngine.nativeGetLastError()}")
            return null
        }
        log("init $deviceName ms=$initMs")
        try {
            check(NCNNEngine.nativeLoadModel(ptr, modelBase)) {
                "loadModel failed: ${NCNNEngine.nativeGetLastError()}"
            }
            val t1 = System.nanoTime()
            val out = NCNNEngine.nativeProcessImage(ptr, rgb, size, size, 3, "png", 90, "")
            val ms = (System.nanoTime() - t1) / 1_000_000
            if (out == null) {
                log("run $deviceName $modelName ${size}x$size ms=$ms bytes=0 md5=- err=${NCNNEngine.nativeGetLastError()}")
                return null
            }
            val hex = md5(out)
            log("run $deviceName $modelName ${size}x$size ms=$ms bytes=${out.size} md5=$hex")
            log("luma $deviceName ${size}x$size ${lumaStats(out)}")
            var file: File? = null
            if (save) {
                val dir = File(
                    InstrumentationRegistry.getInstrumentation().targetContext.getExternalFilesDir(null),
                    "probe_diag"
                )
                dir.mkdirs()
                file = File(dir, "${modelName}_${deviceName}_${size}.png")
                file.writeBytes(out)
                log("file $deviceName ${file.absolutePath}")
            }
            return RunResult(hex, out.size, ms, file)
        } finally {
            NCNNEngine.nativeRelease(ptr)
        }
    }

    @Test
    fun probeDiagnostic() {
        // ncnn failure logs go to stderr (invisible in logcat) — capture them.
        val diagDir = File(
            InstrumentationRegistry.getInstrumentation().targetContext.getExternalFilesDir(null),
            "probe_diag"
        )
        diagDir.mkdirs()
        val stderrFile = File(diagDir, "stderr.txt")
        val redirected = NCNNEngine.nativeRedirectStderr(stderrFile.absolutePath)
        log("stderr-redirect=$redirected path=${stderrFile.absolutePath}")

        arg("env", "").split(";").map { it.trim() }.filter { it.contains('=') }.forEach {
            val (k, v) = it.split('=', limit = 2)
            Os.setenv(k, v, true)
            log("env $k=$v")
        }
        val models = findModels()
        log("models ${models.joinToString(", ") { it.absolutePath }}")
        val want = arg("model", "")
        val chosen = models.firstOrNull { it.absolutePath.contains(want, ignoreCase = true) }
            ?: models.firstOrNull()
            ?: throw IllegalStateException("no models on device")
        val modelBase = chosen.absolutePath.removeSuffix(".param")
        val modelName = chosen.parentFile!!.name + "-" + chosen.name.removeSuffix(".param")
        log("chosen $modelName base=$modelBase")

        for (size in arg("sizes", "96").split(",").mapNotNull { it.trim().toIntOrNull() }) {
            val rgb = probeRgb(size)
            val gpu = runOnce(1, "gpu", modelBase, modelName, rgb, size, save = true)
            val cpu = runOnce(0, "cpu", modelBase, modelName, rgb, size, save = true)
            log("md5-match gpu-vs-cpu ${size}x$size equal=${if (gpu != null && cpu != null && gpu.md5 == cpu.md5) 1 else 0}")
            if (arg("blobscan", "1") == "1" && gpu == null) {
                val ptr = NCNNEngine.nativeInit(1)
                if (ptr != 0L) {
                    try {
                        check(NCNNEngine.nativeLoadModel(ptr, modelBase))
                        val report = NCNNEngine.nativeDebugBlobScan(ptr, rgb, size, size, 3)
                        report?.lineSequence()?.filter { it.isNotBlank() }?.forEach {
                            log("BLOB $it")
                        }
                    } finally {
                        NCNNEngine.nativeRelease(ptr)
                    }
                }
            }
        }
        // Emit captured ncnn stderr (may be empty if nothing logged).
        runCatching {
            val content = stderrFile.readText()
            log("stderr-bytes=${content.toByteArray().size}")
            content.lineSequence().filter { it.isNotBlank() }.forEach { log("NCNN $it") }
        }
        log("done")
    }
}
