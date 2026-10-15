package io.upix.data.repository

import io.upix.data.model.RemoteModel
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

    private fun zipBytesWithEntries(entries: List<Pair<String, ByteArray>>): ByteArray =
        java.io.ByteArrayOutputStream().let { buffer ->
            ZipOutputStream(buffer).use { zip ->
                entries.forEach { (name, bytes) ->
                    zip.putNextEntry(ZipEntry(name))
                    zip.write(bytes)
                    zip.closeEntry()
                }
            }
            buffer.toByteArray()
        }

    @Test
    fun downloadModel_keepsInstalledModelWhenZipIsIncomplete() {
        // 已装好一套可用模型：本次下载的 zip 只有一个 .param（缺 .bin），解压
        // 结束后必须“整单失败、零落盘”——既不能覆盖旧模型，也不能留下半截
        // 文件让模型扫描把截断的权重交给 native 引擎。
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        File(dir, "model.param").writeText("good-param")
        File(dir, "model.bin").writeBytes(byteArrayOf(9, 9, 9, 9))

        val zip = zipBytesWithEntries(listOf("model.param" to "magic-param".toByteArray()))
        val (server, url) = serveOnce(zip)
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), sha256(zip), url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue(result.isFailure)
        assertTrue(
            "expected incomplete-zip failure, got: ${result.exceptionOrNull()}",
            result.exceptionOrNull()!!.message!!.contains("one .param + one .bin")
        )
        assertEquals("good-param", File(dir, "model.param").readText())
        assertEquals(4, File(dir, "model.bin").length())
        assertEquals(
            "leftovers after a failed install: ${dir.list()?.toList()}",
            setOf("model.param", "model.bin"),
            dir.listFiles()!!.map { it.name }.toSet()
        )
    }

    @Test
    fun downloadModel_rejectsZipWithoutMatchingPair() {
        // 两个 .param、零 .bin：条目数恰好是 2，但配不成一对。
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        val zip = zipBytesWithEntries(
            listOf(
                "a.param" to "a".toByteArray(),
                "b.param" to "b".toByteArray()
            )
        )
        val (server, url) = serveOnce(zip)
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), sha256(zip), url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue(result.isFailure)
        assertTrue(
            "expected pair validation failure, got: ${result.exceptionOrNull()}",
            result.exceptionOrNull()!!.message!!.contains("one .param + one .bin")
        )
        assertEquals(0, dir.listFiles()!!.size)
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

    // --- 暂存/就位失败路径（回归） ----------------------------------------

    /**
     * 把 zip 中名为 [entryName] 的条目的压缩数据截断到 [keepBytes] 字节，
     * 模拟解压途中写暂存文件失败（磁盘满 / 下载被切断 / zip 数据损坏）。
     */
    private fun truncateEntryData(zip: ByteArray, entryName: String, keepBytes: Int = 1): ByteArray {
        for (cursor in 0..(zip.size - 30)) {
            val isLocalHeader = zip[cursor] == 0x50.toByte() && zip[cursor + 1] == 0x4B.toByte() &&
                zip[cursor + 2] == 0x03.toByte() && zip[cursor + 3] == 0x04.toByte()
            if (!isLocalHeader) continue
            val nameLength = (zip[cursor + 26].toInt() and 0xFF) or ((zip[cursor + 27].toInt() and 0xFF) shl 8)
            val extraLength = (zip[cursor + 28].toInt() and 0xFF) or ((zip[cursor + 29].toInt() and 0xFF) shl 8)
            if (cursor + 30 + nameLength > zip.size) continue
            val name = String(zip, cursor + 30, nameLength, Charsets.UTF_8)
            if (name == entryName) {
                return zip.copyOf(cursor + 30 + nameLength + extraLength + keepBytes)
            }
        }
        throw AssertionError("zip entry not found: $entryName")
    }

    private fun installedFileNames(dir: File): Set<String> =
        dir.listFiles()!!.filter { it.isFile }.map { it.name }.toSet()

    @Test
    fun downloadModel_cleansStagingFileWhoseWriteFailed() {
        // 第二个条目（.bin）的压缩数据被截断：写 `model.bin.part` 的中途失败。
        // 失败的那次写入自己也必须被清理——它已经落了半截内容在目录里，而
        // discoverModels 之外的任何后续扫描都会把它当成残骸。
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        File(dir, "model.param").writeText("good-param")
        File(dir, "model.bin").writeBytes(byteArrayOf(9, 9, 9, 9))

        val zip = truncateEntryData(
            zipBytes("magic-param", ByteArray(1024) { (it * 31 + 7).toByte() }),
            "model.bin"
        )
        val (server, url) = serveOnce(zip)
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), sha256(zip), url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue("expected extraction failure, got: ${result.getOrNull()}", result.isFailure)
        assertEquals("good-param", File(dir, "model.param").readText())
        assertEquals(4, File(dir, "model.bin").length())
        assertEquals(
            "failed extraction left staging junk: ${dir.list()?.toList()}",
            setOf("model.param", "model.bin"),
            installedFileNames(dir)
        )
    }

    @Test
    fun downloadModel_rejectsPairWithDifferentNames() {
        // 条目数恰好是 1 个 .param + 1 个 .bin，但名字不同（A.param + B.bin）：
        // discoverModels 按同名配对，装上去只留一个永远配不上的孤儿 .param。
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        File(dir, "model.param").writeText("good-param")
        File(dir, "model.bin").writeBytes(byteArrayOf(9, 9, 9, 9))

        val zip = zipBytesWithEntries(
            listOf(
                "a.param" to "a".toByteArray(),
                "b.bin" to byteArrayOf(1, 2)
            )
        )
        val (server, url) = serveOnce(zip)
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), sha256(zip), url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue("mismatched pair must be rejected, got: ${result.getOrNull()}", result.isFailure)
        assertTrue(
            "expected pair validation failure, got: ${result.exceptionOrNull()}",
            result.exceptionOrNull()!!.message!!.contains("one .param + one .bin")
        )
        assertEquals("good-param", File(dir, "model.param").readText())
        assertEquals(4, File(dir, "model.bin").length())
        assertEquals(
            "rejected pair left junk: ${dir.list()?.toList()}",
            setOf("model.param", "model.bin"),
            installedFileNames(dir)
        )
    }

    @Test
    fun downloadModel_keepsInstalledModelWhenTargetIsNotAFile() {
        // .bin 目标被非空目录占着（非普通文件，无法整对改名）：不得先换 .param
        // 再失败，否则旧模型就只剩一半是旧的。
        val dir = File.createTempFile("models", "").let { it.delete(); it.apply { mkdirs() } }
        File(dir, "model.param").writeText("good-param")
        File(dir, "model.bin").apply { mkdirs(); resolve("occupied").writeText("not ours") }

        val zip = zipBytes("magic-param", byteArrayOf(1, 2, 3))
        val (server, url) = serveOnce(zip)
        val model = RemoteModel("model-x2", 2, zip.size.toLong(), sha256(zip), url)

        val result = runBlocking { repository.downloadModel(model, dir) { _, _ -> } }

        server.stop(0)
        assertTrue("expected install failure, got: ${result.getOrNull()}", result.isFailure)
        assertEquals(
            "already-installed .param must survive a failed install (rollback)",
            "good-param",
            File(dir, "model.param").readText()
        )
        assertEquals(
            "failed install left staging/backup junk: ${dir.list()?.toList()}",
            setOf("model.param"),
            installedFileNames(dir)
        )
    }
}
