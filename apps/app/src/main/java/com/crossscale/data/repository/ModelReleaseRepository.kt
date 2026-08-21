package com.crossscale.data.repository

import com.crossscale.data.model.RemoteModel
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.Locale
import java.util.zip.ZipInputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * GitHub "models" release 客户端：拉取 manifest.json 并下载/校验/安装模型。
 *
 * 与 CLI (cli/src/model_downloader.cpp) 共享同一发布布局：
 *   manifest.json          — {"version":1,"models":[{name,scale,size_bytes,sha256,url}]}
 *   <model-name>.zip       — DEFLATE zip：<name>.param + <name>.bin
 *
 * 下载流式写入 models 目录下的 .part 临时文件，校验 sha256 后解压，
 * 任一步失败即清理临时文件并返回错误信息（不抛异常）。
 */
class ModelReleaseRepository(
    private val logRepository: LogRepository
) {
    /** manifest 拉取失败（网络/解析/版本）时返回 null，message 为用户可读原因。 */
    suspend fun fetchRemoteModels(): Result<List<RemoteModel>> = withContext(Dispatchers.IO) {
        runCatching {
            var lastError: Throwable? = null
            for (candidate in candidateUrls(MANIFEST_URL)) {
                try {
                    return@runCatching parseManifest(httpGetText(candidate))
                } catch (throwable: Throwable) {
                    lastError = throwable
                    logRepository.info(
                        "ModelReleaseRepository",
                        "Manifest attempt failed ($candidate): ${throwable.message}"
                    )
                }
            }
            throw lastError ?: IOException("manifest fetch failed")
        }.onFailure { throwable ->
            logRepository.error("ModelReleaseRepository", "Fetch manifest failed: ${throwable.message}")
        }
    }

    /**
     * 下载并安装一个远程模型到 modelsDirectory。onProgress 收到
     * (已下载字节, 总字节或 0)。成功返回安装后的模型名。
     */
    suspend fun downloadModel(
        model: RemoteModel,
        modelsDirectory: File,
        onProgress: (Long, Long) -> Unit
    ): Result<String> = withContext(Dispatchers.IO) {
        runCatching {
            if (!modelsDirectory.exists() && !modelsDirectory.mkdirs()) {
                throw IOException("cannot create ${modelsDirectory.absolutePath}")
            }
            var lastError: Throwable? = null
            for (candidate in candidateUrls(model.url)) {
                try {
                    return@runCatching downloadAndInstall(candidate, model, modelsDirectory, onProgress)
                } catch (throwable: Throwable) {
                    lastError = throwable
                    logRepository.info(
                        "ModelReleaseRepository",
                        "Download attempt failed ($candidate): ${throwable.message}"
                    )
                }
            }
            throw lastError ?: IOException("download failed")
        }.onFailure { throwable ->
            logRepository.error("ModelReleaseRepository", "Download ${model.name} failed: ${throwable.message}")
        }
    }

    /** 单次尝试：下载→sha256 校验→解压，任一步失败抛异常并清理临时文件。 */
    private fun downloadAndInstall(
        url: String,
        model: RemoteModel,
        modelsDirectory: File,
        onProgress: (Long, Long) -> Unit
    ): String {
        // 1. 流式下载到 .part 临时文件。
        val zipFile = File(modelsDirectory, "${model.name}.zip.part")
        val digest = MessageDigest.getInstance("SHA-256")
        var downloaded = 0L
        try {
            openConnection(url).let { connection ->
                try {
                    val total = connection.contentLengthLong
                    connection.inputStream.use { input ->
                        zipFile.outputStream().use { output ->
                            val buffer = ByteArray(DOWNLOAD_BUFFER_BYTES)
                            while (true) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                output.write(buffer, 0, read)
                                digest.update(buffer, 0, read)
                                downloaded += read
                                onProgress(downloaded, total)
                            }
                        }
                    }
                } finally {
                    connection.disconnect()
                }
            }
        } catch (throwable: Throwable) {
            zipFile.delete()
            throw throwable
        }

        // 2. sha256 校验（与 manifest 互验，manifest 由发布脚本生成）。
        val actual = digest.digest().toHexString()
        val expected = model.sha256.lowercase(Locale.ROOT)
        if (actual != expected) {
            zipFile.delete()
            throw IOException("sha256 mismatch: expected $expected, got $actual")
        }

        // 3. 解压 .param/.bin 到 modelsDirectory（展平，条目名去目录）。
        val written = mutableListOf<File>()
        try {
            ZipInputStream(zipFile.inputStream().buffered()).use { zip ->
                var entry = zip.nextEntry
                while (entry != null) {
                    val name = entry.name.replace('\\', '/').trim('/')
                    if (!entry.isDirectory &&
                        !name.contains("..") &&
                        (name.endsWith(".param") || name.endsWith(".bin"))
                    ) {
                        val base = name.substringAfterLast('/')
                        val dest = File(modelsDirectory, base)
                        dest.outputStream().use { output -> zip.copyTo(output) }
                        written += dest
                    }
                    zip.closeEntry()
                    entry = zip.nextEntry
                }
            }
        } finally {
            zipFile.delete()
        }
        if (written.size != 2) {
            written.forEach { it.delete() }
            throw IOException("zip did not contain one .param + one .bin (got ${written.size})")
        }

        logRepository.info(
            "ModelReleaseRepository",
            "Downloaded ${model.name} (${model.sizeBytes} bytes) into ${modelsDirectory.absolutePath}"
        )
        return model.name
    }

    private fun httpGetText(url: String): String {
        val connection = openConnection(url)
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode} for $url")
            }
            return connection.inputStream.bufferedReader().use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    private fun openConnection(url: String): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = CONNECT_TIMEOUT_MS
        connection.readTimeout = READ_TIMEOUT_MS
        connection.instanceFollowRedirects = true
        connection.setRequestProperty("User-Agent", "crossscale-android/0.1")
        return connection
    }

    private fun ByteArray.toHexString(): String =
        joinToString("") { "%02x".format(it) }

    /** GitHub 直连失败时依次尝试的下载加速前缀（直连 GitHub 常见不可达）。 */
    private val ACCELERATOR_PREFIXES = listOf(
        "https://ghfast.top/",
        "https://gh-proxy.com/"
    )

    /** 候选地址：直连优先，失败后逐个走加速前缀。 */
    private fun candidateUrls(url: String): List<String> =
        listOf(url) + ACCELERATOR_PREFIXES.map { prefix -> prefix + url }

    private companion object {
        // 与 CLI 的 kDefaultModelRepo 保持一致（cli/src/model_downloader.hpp）。
        const val MANIFEST_URL =
            "https://github.com/xihale/crossscale/releases/download/models/manifest.json"
        const val CONNECT_TIMEOUT_MS = 8_000
        const val READ_TIMEOUT_MS = 15_000
        const val DOWNLOAD_BUFFER_BYTES = 64 * 1024
    }
}

/** 解析 manifest.json 文本；版本不支持或字段缺失时抛 IOException。 */
internal fun parseManifest(body: String): List<RemoteModel> {
    val root = JSONObject(body)
    val version = root.optInt("version", 0)
    if (version != 1) {
        throw IOException("unsupported manifest version $version (this build understands version 1)")
    }
    return root.getJSONArray("models").let { array ->
        List(array.length()) { index ->
            val entry = array.getJSONObject(index)
            RemoteModel(
                name = entry.getString("name"),
                scale = entry.optInt("scale", 2),
                sizeBytes = entry.optLong("size_bytes", 0L),
                sha256 = entry.getString("sha256"),
                url = entry.getString("url")
            )
        }
    }
}
