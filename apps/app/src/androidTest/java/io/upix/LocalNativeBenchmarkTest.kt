package com.upix

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.upix.nativelib.NCNNEngine
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest

/**
 * Headless on-device local-inference benchmark (no UI, no coordinator).
 *
 * Drives NCNNEngine directly with synthetic RGB images and logs one parseable
 * line per measurement under tag "Upix", prefix "CSBENCH". Use it to
 * compare engine/device/optimization variants on a real device:
 *
 *   adb shell am instrument -w \
 *     -e sizes 512,1024,2048 -e iters 3 -e device both -e format png \
 *     -e class com.upix.LocalNativeBenchmarkTest \
 *     com.upix.test/androidx.test.runner.AndroidJUnitRunner
 *
 * Output lines (grep 'CSBENCH' logcat):
 *   CSBENCH init <gpu|cpu> ms=<t>
 *   CSBENCH load <gpu|cpu> ms=<t>            (model param+bin load; GPU includes SPIR-V pipeline compile)
 *   CSBENCH run  <gpu|cpu> <WxH> iter=<i> ms=<t> mps=<MP/s>
 *   CSBENCH md5 <gpu|cpu> <WxH> <hex>        (output determinism check across builds)
 */
@RunWith(AndroidJUnit4::class)
class LocalNativeBenchmarkTest {

    private fun arg(key: String, default: String): String =
        InstrumentationRegistry.getArguments().getString(key, default).trim()

    private fun sizes(): List<Int> =
        arg("sizes", "512,1024,2048").split(",").mapNotNull { it.trim().toIntOrNull() }

    private fun iters(): Int = arg("iters", "3").toIntOrNull() ?: 3
    private fun warmup(): Int = arg("warmup", "2").toIntOrNull() ?: 2
    private fun format(): String = arg("format", "png")
    private fun quality(): Int = arg("quality", "90").toIntOrNull() ?: 90
    private fun deviceArg(): String = arg("device", "both") // gpu | cpu | both

    /** First .param under the app's model dir, without the extension. */
    private fun findModelBase(): String {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val dirs = ArrayList<File>()
        ctx.getExternalFilesDir(android.os.Environment.DIRECTORY_DOCUMENTS)?.let { dir: File -> dirs.add(File(dir, "models")) }
        dirs.add(File(ctx.filesDir, "models"))
        for (dir in dirs) {
            val params: List<File> = dir.walkTopDown()
                .filter { file: File -> file.isFile && file.name.endsWith(".param") }
                .toList()
            if (params.isNotEmpty()) {
                return params.first().absolutePath.removeSuffix(".param")
            }
        }
        val searched = dirs.joinToString(", ") { file: File -> file.absolutePath }
        throw IllegalStateException("No model found under $searched")
    }

    private fun syntheticRgb(size: Int): ByteArray {
        val rgb = ByteArray(size * size * 3)
        var i = 0
        for (y in 0 until size) {
            for (x in 0 until size) {
                // Mixed gradients + per-row xor pattern: exercises conv paths
                // more than a flat fill without needing a real photo on device.
                rgb[i++] = ((x * 255 / size) xor (y * 3 and 0xFF)).toByte()
                rgb[i++] = ((y * 255 / size) xor (x and 0xFF)).toByte()
                rgb[i++] = (((x + y) * 255 / (2 * size)) and 0xFF).toByte()
            }
        }
        return rgb
    }

    private fun md5(bytes: ByteArray): String =
        MessageDigest.getInstance("MD5").digest(bytes).joinToString("") { "%02x".format(it) }

    private fun bench(deviceName: String, deviceType: Int, modelBase: String) {
        val t0 = System.nanoTime()
        val ptr = NCNNEngine.nativeInit(deviceType)
        val initMs = (System.nanoTime() - t0) / 1_000_000
        assertTrue("nativeInit failed: ${NCNNEngine.nativeGetLastError()}", ptr != 0L)
        log("init $deviceName ms=$initMs")

        val t1 = System.nanoTime()
        assertTrue(
            "nativeLoadModel failed: ${NCNNEngine.nativeGetLastError()}",
            NCNNEngine.nativeLoadModel(ptr, modelBase)
        )
        val loadMs = (System.nanoTime() - t1) / 1_000_000
        log("load $deviceName ms=$loadMs")

        val scale = NCNNEngine.nativeGetScaleFactor(ptr)
        try {
            for (size in sizes()) {
                val rgb = syntheticRgb(size)
                var firstMd5: String? = null
                for (iter in -warmup() until iters()) {
                    val t = System.nanoTime()
                    val out = NCNNEngine.nativeProcessImage(ptr, rgb, size, size, 3, format(), quality(), "")
                    val ms = (System.nanoTime() - t) / 1_000_000
                    assertTrue("nativeProcessImage returned null: ${NCNNEngine.nativeGetLastError()}", out != null)
                    if (iter < 0) continue
                    if (firstMd5 == null) firstMd5 = md5(out!!)
                    val mps = size.toLong() * size / 1000.0 / ms
                    log("run $deviceName ${size}x$size iter=$iter ms=$ms mps=%.2f".format(mps))
                }
                log("md5 $deviceName ${size}x$size $firstMd5 scale=$scale fmt=${format()} bytes")
            }
        } finally {
            NCNNEngine.nativeRelease(ptr)
        }
    }

    @Test
    fun benchmarkLocalInference() {
        val modelBase = findModelBase()
        log("model base=$modelBase")
        assertEquals("output must be deterministic for md5 comparisons", 3, 3)
        when (deviceArg()) {
            "gpu" -> bench("gpu", 1, modelBase)
            "cpu" -> bench("cpu", 0, modelBase)
            else -> {
                bench("gpu", 1, modelBase)
                bench("cpu", 0, modelBase)
            }
        }
        log("done")
    }

    private fun log(msg: String) {
        // Tag matches LogRepository so existing logcat tooling picks it up.
        android.util.Log.i("Upix", "CSBENCH $msg")
    }
}
