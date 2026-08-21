package com.crossscale.data.repository

import com.crossscale.data.model.RemoteModel
import com.sun.net.httpserver.HttpServer
import java.io.File
import java.io.IOException
import java.net.InetSocketAddress
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class ModelReleaseRepositoryTest {
    private val repository = ModelReleaseRepository(LogRepository())

    // --- parseManifest -----------------------------------------------------

    @Test
    fun parseManifest_readsEntries() {
        val body = """
            {"version": 1, "models": [
              {"name": "foo-x2", "scale": 2, "size_bytes": 42,
               "sha256": "aa", "url": "https://example/foo.zip"},
              {"name": "bar-x4", "size_bytes": 7, "sha256": "bb",
               "url": "https://example/bar.zip"}
            ]}
        """.trimIndent()

        val models = parseManifest(body)

        assertEquals(2, models.size)
        assertEquals("foo-x2", models[0].name)
        assertEquals(2, models[0].scale)
        assertEquals(42L, models[0].sizeBytes)
        assertEquals("bar-x4", models[1].name)
        assertEquals(2, models[1].scale) // absent -> default
    }

    @Test
    fun parseManifest_rejectsUnknownVersion() {
        try {
            parseManifest("""{"version": 99, "models": []}""")
            fail("expected IOException")
        } catch (expected: IOException) {
        }
    }

    @Test
    fun parseManifest_rejectsMissingField() {
        try {
            parseManifest("""{"version": 1, "models": [{"name": "x"}]}""")
            fail("expected failure")
        } catch (expected: Exception) {
        }
    }

    // --- downloadModel -----------------------------------------------------

    /** Serves one file over a local HttpServer; returns its URL. */
    private fun serveOnce(bytes: ByteArray): Pair<HttpServer, String> {
        val server = HttpServer.create(InetSocketAddress(0), 0)
        server.createContext("/") { exchange ->
            exchange.sendResponseHeaders(200, bytes.size.toLong())
            exchange.responseBody.use { it.write(bytes) }
        }
        server.start()
        return server to "http://127.0.0.1:${server.address.port}/model.zip"
    }

    private fun zipBytes(param: String, bin: ByteArray): ByteArray =
        java.io.ByteArrayOutputStream().let { buffer ->
            ZipOutputStream(buffer).use { zip ->
                zip.putNextEntry(ZipEntry("model.param"))
                zip.write(param.toByteArray())
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("model.bin"))
                zip.write(bin)
                zip.closeEntry()
            }
            buffer.toByteArray()
        }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes)
            .joinToString("") { "%02x".format(it) }

    @Test
    fun downloadModel_installsVerifiedZip() {
        val zip = zipBytes("magic-param", byteArrayOf(1, 2, 3))
        val (server, url) = serveOnce(zip)
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), sha256(zip), url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue("download failed: ${result.exceptionOrNull()}", result.isSuccess)
        assertEquals("model-x2", result.getOrThrow())
        assertEquals("magic-param", File(dir, "model.param").readText())
        assertEquals(3, File(dir, "model.bin").length())
        // No leftovers: the .part temp file and the zip must be gone.
        assertEquals(2, dir.listFiles()!!.size)
    }

    @Test
    fun downloadModel_rejectsBadSha256AndCleansUp() {
        val zip = zipBytes("magic-param", byteArrayOf(1, 2, 3))
        val (server, url) = serveOnce(zip)
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), "deadbeef", url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue(result.isFailure)
        assertTrue(
            "expected sha256 failure, got: ${result.exceptionOrNull()}",
            result.exceptionOrNull()!!.message!!.contains("sha256")
        )
        assertEquals("temp files must be cleaned up", 0, dir.listFiles()!!.size)
    }
}
