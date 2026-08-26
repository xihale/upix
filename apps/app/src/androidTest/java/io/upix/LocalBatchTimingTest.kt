package io.upix

import android.graphics.Bitmap
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.upix.data.repository.LogRepository
import io.upix.data.repository.NativeImageProcessingRepository
import io.upix.data.repository.ResolvedImageInput
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream

/**
 * Headless local-batch timing: drives the real NativeImageProcessingRepository
 * session across N synthetic images with the run engine's depth-1 decode-ahead
 * (prefetch N+1 while N is being processed), and logs CSLOCAL lines:
 *
 *   CSLOCAL <i>/<n> decode-ahead hidden → process ms=<t> total=<t>
 *   CSLOCAL batch n=<n> wall=<ms> avg=<ms>
 *
 * Compare with the serial baseline (`mode=serial`) to quantify the overlap.
 * With prefetch active, later items' logs also show "no prefetch hit" lines
 * only when the ahead job lost the race — steady state should be silent.
 */
@RunWith(AndroidJUnit4::class)
class LocalBatchTimingTest {

    private fun arg(key: String, default: String): String =
        InstrumentationRegistry.getArguments().getString(key, default).trim()

    private fun count(): Int = arg("count", "3").toIntOrNull() ?: 3
    private fun side(): Int = arg("side", "1024").toIntOrNull() ?: 1024
    private fun mode(): String = arg("mode", "ahead") // ahead | serial

    private fun makePngFile(dir: File, index: Int, size: Int): File {
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val pixels = IntArray(size * size) { i ->
            val x = i % size
            val y = i / size
            (0xFF shl 24) or (((x + index * 7) * 4 and 0xFF) shl 16) or ((y * 4 and 0xFF) shl 8) or ((x + y) and 0xFF)
        }
        bmp.setPixels(pixels, 0, size, 0, 0, size, size)
        val f = File(dir, "bench-in-$index.png")
        FileOutputStream(f).use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bmp.recycle()
        return f
    }

    @Test
    fun batchTiming() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val workDir = File(ctx.getExternalFilesDir(null), "bench-local-batch")
        workDir.mkdirs()
        val outDir = File(workDir, "out-${System.nanoTime()}").apply { mkdirs() }

        val modelDir = File(ctx.getExternalFilesDir(android.os.Environment.DIRECTORY_DOCUMENTS), "models")
        val param = modelDir.walkTopDown().first { it.name.endsWith(".param") }
        val modelBase = param.absolutePath.removeSuffix(".param")

        val repo = NativeImageProcessingRepository(ctx, LogRepository())
        val session = repo.openSession(
            modelName = modelBase.substringAfterLast('/'),
            modelBasePath = modelBase,
            preferGpu = true
        )
        assertNotNull("openSession failed", session)

        try {
            session!!
            val inputDir = arg("inputDir", "")
            val inputs: List<ResolvedImageInput> = if (inputDir.isNotBlank()) {
                // 真实照片模式：直接使用目录里的文件（jpg/png/webp）。
                File(inputDir).listFiles()!!
                    .filter { it.isFile }
                    .sortedBy { it.name }
                    .map { ResolvedImageInput(android.net.Uri.fromFile(it), it.name) }
            } else {
                (0 until count()).map { i ->
                    val f = makePngFile(workDir, i, side())
                    ResolvedImageInput(android.net.Uri.fromFile(f), f.name)
                }
            }

            runBlocking {
                val wall0 = System.nanoTime()
                coroutineScope {
                    for ((index, item) in inputs.withIndex()) {
                        val item0 = System.nanoTime()
                        val outputFile = File(outDir, "out-$index.jpg")

                        // 与 ProcessingRunEngine 本地分支一致的 depth-1 预取。
                        val decodeAheadJob =
                            if (mode() == "ahead" && index + 1 < inputs.size) {
                                async(Dispatchers.IO) { session.prefetch(inputs[index + 1]) }
                            } else {
                                null
                            }

                        val result = session.process(input = item, outputFile = outputFile)
                        decodeAheadJob?.join()
                        assertTrue(
                            "process #$index failed: ${result.exceptionOrNull()}",
                            result.isSuccess
                        )
                        val ms = (System.nanoTime() - item0) / 1_000_000
                        Log.i("Upix", "CSLOCAL $index/${inputs.size} process ms=$ms (${item.displayName})")
                    }
                }
                val wallMs = (System.nanoTime() - wall0) / 1_000_000
                Log.i(
                    "Upix",
                    "CSLOCAL batch mode=${mode()} n=${inputs.size} side=${side()} wall=$wallMs " +
                        "avg=${wallMs / inputs.size}"
                )
            }
        } finally {
            session?.close()
        }
    }
}
