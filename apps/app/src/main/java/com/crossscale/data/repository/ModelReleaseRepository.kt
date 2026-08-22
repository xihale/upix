package com.crossscale.data.repository

import com.crossscale.data.model.RemoteModel
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.Socket
import java.net.URL
import java.security.MessageDigest
import java.util.Locale
import java.util.zip.ZipInputStream
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
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
 * 网络策略（国内环境）：
 *   1. 系统解析直连；
 *   2. 失败后查固定 IP 表（见 GITHUB_FIXED_IPS）直连 + SNI 剥离。
 * 证书判断完整保留：握手链校验走系统信任库，握手后平台默认主机名验证器
 * 逐跳核对证书与目标域名，不匹配即失败，绝不关闭校验。
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
            for (stripSni in CONNECT_STRATEGIES) {
                try {
                    return@runCatching parseManifest(httpGetText(MANIFEST_URL, stripSni))
                } catch (throwable: Throwable) {
                    lastError = throwable
                    // 逐次尝试属预期内降级，仅记 debug：避免每次联网都在日志里刷屏。
                    logRepository.debug(
                        "ModelReleaseRepository",
                        "Manifest attempt failed (sni-stripped=$stripSni): ${throwable.message}"
                    )
                }
            }
            throw lastError ?: IOException("manifest fetch failed")
        }.onSuccess {
            logRepository.info("ModelReleaseRepository", "Manifest fetched")
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
            for (stripSni in CONNECT_STRATEGIES) {
                try {
                    return@runCatching downloadAndInstall(stripSni, model, modelsDirectory, onProgress)
                } catch (throwable: Throwable) {
                    lastError = throwable
                    logRepository.info(
                        "ModelReleaseRepository",
                        "Download attempt failed (sni-stripped=$stripSni): ${throwable.message}"
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
        stripSni: Boolean,
        model: RemoteModel,
        modelsDirectory: File,
        onProgress: (Long, Long) -> Unit
    ): String {
        // 1. 流式下载到 .part 临时文件。
        val zipFile = File(modelsDirectory, "${model.name}.zip.part")
        val digest = MessageDigest.getInstance("SHA-256")
        var downloaded = 0L
        try {
            openConnection(model.url, stripSni).let { connection ->
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

    private fun httpGetText(url: String, stripSni: Boolean): String {
        val connection = openConnection(url, stripSni)
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode} for $url")
            }
            return connection.inputStream.bufferedReader().use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    private fun openConnection(url: String, stripSni: Boolean): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        if (stripSni && connection is HttpsURLConnection) {
            connection.sslSocketFactory = StripSniSocketFactory()
        }
        connection.connectTimeout = CONNECT_TIMEOUT_MS
        connection.readTimeout = READ_TIMEOUT_MS
        connection.instanceFollowRedirects = true
        connection.setRequestProperty("User-Agent", "crossscale-android/0.1")
        return connection
    }

    private fun ByteArray.toHexString(): String =
        joinToString("") { "%02x".format(it) }

    private companion object {
        // 与 CLI 的 kDefaultModelRepo 保持一致（cli/src/model_downloader.hpp）。
        const val MANIFEST_URL =
            "https://github.com/xihale/crossscale/releases/download/models/manifest.json"

        /** 连接策略：默认固定 IP 直连 + SNI 剥离，不做无谓的直连尝试。 */
        val CONNECT_STRATEGIES = listOf(true)

        const val CONNECT_TIMEOUT_MS = 8_000
        const val READ_TIMEOUT_MS = 15_000
        const val DOWNLOAD_BUFFER_BYTES = 64 * 1024
    }
}

/**
 * GitHub 固定 IP 表（绕过 DNS 污染）。键带 "*" 前缀表示后缀通配：
 * "*github.com" 匹配 github.com 及其全部子域。查询精确优先、通配其次。
 */
private val GITHUB_FIXED_IPS: Map<String, String> = mapOf(
    "api.github.com" to "20.205.243.168",
    "codeload.github.com" to "20.205.243.165",
    "community.github.com" to "140.82.114.17",
    "docs.github.com" to "185.199.110.154",
    "objects-origin.githubusercontent.com" to "140.82.113.22",
    "resources.github.com" to "185.199.108.133",
    "services.github.com" to "140.82.113.18",
    "support.github.com" to "185.199.108.133",
    "*github.com" to "20.27.177.113",
    "*githubusercontent.com" to "185.199.108.133"
)

/** 固定 IP 表查询：精确匹配优先，其次 "*" 后缀通配（须以 "." 分界）；未命中返回 null。 */
private fun fixedIpFor(host: String): InetAddress? {
    GITHUB_FIXED_IPS[host]?.let { return InetAddress.getByName(it) }
    return GITHUB_FIXED_IPS.entries.firstOrNull { (pattern, _) ->
        val suffix = pattern.removePrefix("*")
        pattern.startsWith("*") && (host == suffix || host.endsWith(".$suffix"))
    }?.let { (_, ip) -> InetAddress.getByName(ip) }
}

/**
 * 「固定 IP + SNI 剥离」套接字工厂：
 * - GitHub 各主机用 [fixedIpFor] 查固定 IP 直连，表外主机回退系统解析；
 * - 按 IP 建连，JSSE 默认不发送 SNI，ClientHello 即完成剥离；
 * - 证书判断保留：握手链校验走系统信任库，握手后 HttpsURLConnection
 *   的默认主机名验证器仍逐跳核对证书与目标域名，不匹配直接失败。
 */
private class StripSniSocketFactory : SSLSocketFactory() {
    private val delegate = getDefault() as SSLSocketFactory

    override fun createSocket(socket: Socket?, host: String?, port: Int, autoClose: Boolean): SSLSocket =
        resolveAndConnect(requireNotNull(host) { "null host" }, port)

    override fun createSocket(host: String?, port: Int): SSLSocket =
        resolveAndConnect(requireNotNull(host) { "null host" }, port)

    override fun createSocket(host: String?, port: Int, localHost: InetAddress?, localPort: Int): SSLSocket =
        resolveAndConnect(requireNotNull(host) { "null host" }, port)

    override fun createSocket(address: InetAddress?, port: Int): SSLSocket =
        delegate.createSocket(address, port) as SSLSocket

    override fun createSocket(address: InetAddress?, port: Int, localAddress: InetAddress?, localPort: Int): SSLSocket =
        delegate.createSocket(address, port, localAddress, localPort) as SSLSocket

    private fun resolveAndConnect(host: String, port: Int): SSLSocket {
        val ip = fixedIpFor(host) ?: InetAddress.getByName(host)
        return delegate.createSocket(ip, port) as SSLSocket
    }

    override fun getDefaultCipherSuites(): Array<String> = delegate.defaultCipherSuites

    override fun getSupportedCipherSuites(): Array<String> = delegate.supportedCipherSuites
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
