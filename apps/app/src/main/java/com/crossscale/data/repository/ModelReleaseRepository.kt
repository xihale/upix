package com.crossscale.data.repository

import com.crossscale.data.model.RemoteModel
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URL
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.Locale
import java.util.zip.ZipInputStream
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import kotlin.random.Random
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
 *   2. 失败后 DoT(tls://223.5.5.5) 解析真实 IP + SNI 剥离直连。
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
                    logRepository.info(
                        "ModelReleaseRepository",
                        "Manifest attempt failed (sni-stripped=$stripSni): ${throwable.message}"
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

        /** 连接策略序列：false=系统解析直连；true=DoT 真实 IP + SNI 剥离。 */
        val CONNECT_STRATEGIES = listOf(false, true)

        const val CONNECT_TIMEOUT_MS = 8_000
        const val READ_TIMEOUT_MS = 15_000
        const val DOWNLOAD_BUFFER_BYTES = 64 * 1024
    }
}

/**
 * 「真实 IP + SNI 剥离」套接字工厂：
 * - 目标主机经 [DotDns] 解析出真实 IP 后直连（绕过 DNS 污染）；
 * - 按 IP 建连，JSSE 默认不发送 SNI，ClientHello 即完成剥离；
 * - 证书判断保留：握手链校验走系统信任库，握手后 HttpsURLConnection
 *   的默认主机名验证器仍逐跳核对证书与目标域名，不匹配直接失败。
 */
private class StripSniSocketFactory : SSLSocketFactory() {
    private val delegate = getDefault() as SSLSocketFactory

    override fun createSocket(socket: Socket?, host: String?, port: Int, autoClose: Boolean): SSLSocket =
        viaDot(requireNotNull(host) { "null host" }, port)

    override fun createSocket(host: String?, port: Int): SSLSocket =
        viaDot(requireNotNull(host) { "null host" }, port)

    override fun createSocket(host: String?, port: Int, localHost: InetAddress?, localPort: Int): SSLSocket =
        viaDot(requireNotNull(host) { "null host" }, port)

    override fun createSocket(address: InetAddress?, port: Int): SSLSocket =
        delegate.createSocket(address, port) as SSLSocket

    override fun createSocket(address: InetAddress?, port: Int, localAddress: InetAddress?, localPort: Int): SSLSocket =
        delegate.createSocket(address, port, localAddress, localPort) as SSLSocket

    private fun viaDot(host: String, port: Int): SSLSocket {
        val ip = DotDns.resolveA(host).firstOrNull()
            ?: throw IOException("DoT resolve failed: $host")
        return delegate.createSocket(ip, port) as SSLSocket
    }

    override fun getDefaultCipherSuites(): Array<String> = delegate.defaultCipherSuites

    override fun getSupportedCipherSuites(): Array<String> = delegate.supportedCipherSuites
}

/**
 * 极简 DNS-over-TLS A 记录解析器（RFC 7858）：直连 223.5.5.5:853，
 * 绕过本地 DNS 污染。手写最小 DNS 报文编解码，仅支持 A 记录查询。
 * 证书判断保留：链校验走系统信任库，另以默认验证器手动核对服务身份
 * （dns.alidns.com），不匹配即拒绝。
 */
internal object DotDns {
    private const val DOT_IP = "223.5.5.5"
    private const val DOT_IDENTITY = "dns.alidns.com"
    private const val DOT_PORT = 853
    private const val TIMEOUT_MS = 4_000

    private val cache = object : LinkedHashMap<String, List<InetAddress>>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, List<InetAddress>>): Boolean =
            size > 32
    }

    /** 返回 host 的 A 记录（会话级 LRU 缓存）；解析失败抛异常。 */
    fun resolveA(host: String): List<InetAddress> {
        synchronized(cache) { cache[host] }?.let { return it }
        val addresses = query(host)
        synchronized(cache) { cache[host] = addresses }
        return addresses
    }

    private fun query(host: String): List<InetAddress> {
        val id = Random.nextInt(0, 0xFFFF)
        SSLSocketFactory.getDefault().createSocket().use { raw ->
            val socket = raw as SSLSocket
            socket.connect(InetSocketAddress(InetAddress.getByName(DOT_IP), DOT_PORT), TIMEOUT_MS)
            socket.soTimeout = TIMEOUT_MS
            socket.startHandshake()
            // 手动验证 DoT 服务身份：固定 IP 建连，但证书必须确属 dns.alidns.com。
            check(HttpsURLConnection.getDefaultHostnameVerifier().verify(DOT_IDENTITY, socket.session)) {
                "DoT certificate does not match $DOT_IDENTITY"
            }
            DataOutputStream(socket.getOutputStream()).use { out ->
                val query = buildQuery(id, host)
                out.writeShort(query.size)
                out.write(query)
                out.flush()
            }
            DataInputStream(socket.getInputStream()).use { input ->
                val length = input.readUnsignedShort()
                val payload = ByteArray(length)
                input.readFully(payload)
                return parseARecords(id, payload)
            }
        }
    }

    /** 最小 A 查询报文：12 字节头（RD=1，QDCOUNT=1）+ QNAME + QTYPE=A + QCLASS=IN。 */
    private fun buildQuery(id: Int, host: String): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(byteArrayOf((id ushr 8).toByte(), id.toByte(), 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))
        host.split('.').filter { it.isNotEmpty() }.forEach { label ->
            val ascii = label.toByteArray(Charsets.US_ASCII)
            out.write(ascii.size)
            out.write(ascii)
        }
        out.write(0x00)
        out.write(byteArrayOf(0x00, 0x01, 0x00, 0x01))
        return out.toByteArray()
    }

    /** 解析响应中的 A 记录；支持 0xC0 压缩指针跳名。 */
    private fun parseARecords(id: Int, payload: ByteArray): List<InetAddress> {
        val buf = ByteBuffer.wrap(payload)
        require(buf.remaining() >= 12) { "short DNS response" }
        val replyId = buf.short.toInt() and 0xFFFF
        require(replyId == id) { "DNS transaction id mismatch" }
        buf.position(buf.position() + 2) // flags
        val questionCount = buf.short.toInt() and 0xFFFF
        val answerCount = buf.short.toInt() and 0xFFFF
        require(answerCount > 0) { "DNS answer count is zero" }
        repeat(questionCount) {
            skipName(buf)
            buf.position(buf.position() + 4) // qtype + qclass
        }
        val addresses = mutableListOf<InetAddress>()
        repeat(answerCount) {
            skipName(buf)
            val type = buf.short.toInt() and 0xFFFF
            buf.position(buf.position() + 2) // class
            buf.position(buf.position() + 4) // ttl
            val dataLength = buf.short.toInt() and 0xFFFF
            val end = buf.position() + dataLength
            if (type == 1 && dataLength == 4 && buf.remaining() >= 4) {
                val bytes = ByteArray(4)
                buf.get(bytes)
                addresses += InetAddress.getByAddress(bytes)
            } else {
                buf.position(end)
            }
        }
        require(addresses.isNotEmpty()) { "DNS response has no A record" }
        return addresses
    }

    private fun skipName(buffer: ByteBuffer) {
        while (true) {
            val length = buffer.get().toInt() and 0xFF
            when {
                length == 0 -> return
                length and 0xC0 == 0xC0 -> {
                    buffer.get() // 指针的第二个字节
                    return
                }
                else -> buffer.position(buffer.position() + length)
            }
        }
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
