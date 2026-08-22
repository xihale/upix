package com.crossscale.data.repository

import com.crossscale.data.model.RemoteModel
import java.io.File
import java.io.InputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.InetSocketAddress
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
 * 网络策略（国内环境）：固定 IP 表（见 GITHUB_FIXED_IPS）直连，TLS 默认
 * 携带 SNI 会被 GFW 掐断（表现为超时），故用自研裸 HTTPS 客户端剥离 SNI。
 * 证书判断完整保留：握手链校验走系统信任库，平台默认主机名验证器
 * 核对证书与目标域名，不匹配即失败，绝不关闭校验。
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
            try {
                return@runCatching parseManifest(httpGetText(MANIFEST_URL))
            } catch (throwable: Throwable) {
                logRepository.debug(
                    "ModelReleaseRepository",
                    "Manifest fetch attempt failed: ${throwable.message}"
                )
                throw throwable
            }
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
            return@runCatching downloadAndInstall(model, modelsDirectory, onProgress)
        }.onFailure { throwable ->
            logRepository.error("ModelReleaseRepository", "Download ${model.name} failed: ${throwable.message}")
        }
    }

    /** 单次尝试：下载→sha256 校验→解压，任一步失败抛异常并清理临时文件。 */
    private fun downloadAndInstall(
        model: RemoteModel,
        modelsDirectory: File,
        onProgress: (Long, Long) -> Unit
    ): String {
        // 1. 流式下载到 .part 临时文件。
        val zipFile = File(modelsDirectory, "${model.name}.zip.part")
        val digest = MessageDigest.getInstance("SHA-256")
        var downloaded = 0L
        try {
            RawHttpsFetcher().get(URL(model.url)).use { response ->
                if (response.status != HttpURLConnection.HTTP_OK) {
                    throw IOException("HTTP ${response.status} for ${model.url}")
                }
                val total = response.header("content-length")?.toLongOrNull() ?: 0L
                response.body.use { input ->
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
        RawHttpsFetcher().get(URL(url)).use { response ->
            if (response.status != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${response.status} for $url")
            }
            return response.body.bufferedReader().use { it.readText() }
        }
    }

    /**
     * 裸 HTTPS 客户端。绕过 HttpURLConnection 的原因：其内部 TLS 升级会按
     * 真实主机名回填 SNI，GFW 据此掐断连接（表现为超时），自定义
     * SSLSocketFactory 无法阻止。这里自己完成 TCP→TLS→HTTP：
     *   - TCP 直连固定 IP 表地址（绕过 DNS 污染）；
     *   - 以 InetAddress 建 TLS 连接，默认不携带 SNI；若 CDN 因此返回
     *     错误默认证书，再携 SNI 重试（证书校验始终完整执行）；
     *   - 握手后用平台默认 HostnameVerifier 核对证书与目标域名；
     *   - 手动跟随 3xx 重定向。
     */
    private class RawHttpsFetcher {
        class Response(
            val status: Int,
            val headers: Map<String, String>,
            val body: InputStream,
            private val socket: Socket
        ) : AutoCloseable {
            override fun close() {
                runCatching { body.close() }
                runCatching { socket.close() }
            }

            fun header(name: String): String? = headers[name.lowercase(Locale.ROOT)]
        }

        fun get(url: URL, redirectsLeft: Int = MAX_REDIRECTS): Response {
            if (redirectsLeft < 0) throw IOException("too many redirects for $url")
            val host = requireNotNull(url.host)
            val port = if (url.port > 0) url.port else url.defaultPort
            require(url.protocol == "https") { "only https is supported: $url" }

            val ip = fixedIpFor(host) ?: InetAddress.getByName(host)
            val ssl = try {
                connectTls(ip, port, host, sendSni = false)
            } catch (verifyFailure: IOException) {
                if (verifyFailure.message?.startsWith("hostname verification") == true) {
                    connectTls(ip, port, host, sendSni = true)
                } else {
                    throw verifyFailure
                }
            }

            try {
                val request = buildString {
                    append("GET ").append(url.file.ifEmpty { "/" }).append(" HTTP/1.1\r\n")
                    append("Host: ").append(host).append("\r\n")
                    append("User-Agent: crossscale-android/0.1\r\n")
                    append("Accept: */*\r\n")
                    append("Connection: close\r\n\r\n")
                }
                ssl.outputStream.write(request.toByteArray(Charsets.US_ASCII))
                ssl.outputStream.flush()

                val reader = ssl.inputStream.bufferedReader(Charsets.ISO_8859_1)
                val statusLine = reader.readLine()
                    ?: throw IOException("empty response from $host")
                val status = statusLine.split(" ").getOrNull(1)?.toIntOrNull()
                    ?: throw IOException("malformed status line: $statusLine")
                val headers = mutableMapOf<String, String>()
                while (true) {
                    val line = reader.readLine() ?: break
                    if (line.isEmpty()) break
                    val idx = line.indexOf(':')
                    if (idx > 0) {
                        headers[line.substring(0, idx).trim().lowercase(Locale.ROOT)] =
                            line.substring(idx + 1).trim()
                    }
                }

                if (status in 300..399) {
                    val location = headers["location"]
                        ?: throw IOException("redirect without Location ($status)")
                    ssl.close()
                    return get(URL(url, location), redirectsLeft - 1)
                }

                val body = if (headers["transfer-encoding"]?.equals("chunked", true) == true) {
                    ChunkedInputStream(ssl.getInputStream())
                } else {
                    ssl.getInputStream()
                }
                return Response(status, headers, body, ssl)
            } catch (throwable: Throwable) {
                runCatching { ssl.close() }
                throw throwable
            }
        }

        /** TCP 直连固定 IP → TLS 握手（可选 SNI）→ 平台默认校验器核对证书域名。 */
        private fun connectTls(ip: InetAddress, port: Int, host: String, sendSni: Boolean): SSLSocket {
            val plain = Socket()
            try {
                plain.connect(InetSocketAddress(ip, port), CONNECT_TIMEOUT_MS)
                plain.soTimeout = READ_TIMEOUT_MS
                val factory = SSLSocketFactory.getDefault() as SSLSocketFactory
                // InetAddress 重载：TLS ClientHello 默认不携带 SNI。
                val s = factory.createSocket(plain, ip.hostAddress, port, true) as SSLSocket
                if (sendSni) {
                    // Conscrypt 专有 API，反射调用以避免编译期依赖。
                    runCatching {
                        s.javaClass.methods.firstOrNull { it.name == "setHostname" }
                            ?.invoke(s, host)
                    }
                }
                s.startHandshake()
                val verified = HttpsURLConnection.getDefaultHostnameVerifier()
                    .verify(host, s.session)
                if (!verified) {
                    runCatching { s.close() }
                    throw IOException("hostname verification failed for $host via $ip")
                }
                return s
            } catch (throwable: Throwable) {
                runCatching { plain.close() }
                throw throwable
            }
        }

        private companion object {
            const val MAX_REDIRECTS = 5
        }
    }

    /** RFC 7230 chunked 传输解码流（最小实现，仅覆盖本客户端场景）。 */
    private class ChunkedInputStream(private val input: InputStream) : InputStream() {
        private var remaining = 0L
        private var done = false

        override fun read(): Int = read(ByteArray(1), 0, 1).let { if (it < 0) -1 else it }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            if (done) return -1
            if (remaining <= 0) {
                remaining = readChunkSize()
                if (remaining == 0L) {
                    // 尾随头部直到空行。
                    val reader = input.bufferedReader(Charsets.ISO_8859_1)
                    while (reader.readLine()?.isNotEmpty() == true) { /* drain trailers */ }
                    done = true
                    return -1
                }
            }
            val n = input.read(buffer, offset, minOf(length.toLong(), remaining).toInt())
            if (n < 0) throw IOException("truncated chunked body")
            remaining -= n
            if (remaining == 0L) {
                val cr = input.read()
                val lf = input.read()
                if (cr != '\r'.code || lf != '\n'.code) {
                    throw IOException("malformed chunk terminator")
                }
            }
            return n
        }

        private fun readChunkSize(): Long {
            val sizeLine = input.bufferedReader(Charsets.ISO_8859_1).readLine()
                ?: throw IOException("missing chunk size")
            return sizeLine.substringBefore(';').trim().toLong(16)
        }
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
