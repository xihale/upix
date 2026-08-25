package com.upix.network

import android.content.Context
import android.net.Uri
import com.upix.data.model.PcModel
import com.upix.data.repository.LogRepository
import com.upix.data.repository.NativeProcessResult
import com.upix.data.repository.ResolvedImageInput
import com.upix.data.repository.decodeEncodedImageBounds
import com.upix.data.repository.readEncodedBytes
import com.upix.data.repository.outputFormatFor
import com.upix.nativelib.NCNNEngine
import com.upix.nativelib.NativeWorker
import java.io.File
import java.io.FileOutputStream
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class CoordinatorClient(
    context: Context,
    private val logRepository: LogRepository
) {
    private val appContext = context.applicationContext

    // Internal representation: carries the peer pubkey + stream flag needed to
    // re-run strict-MITM handshakes for each one-shot image submit. The public,
    // UI-facing endpoint shape (data.model.RemoteEndpoint) is owned by
    // DeviceViewModel, which derives it from these outcomes.
    // 处理与连接在不同线程读写：@Volatile 保证可见性。
    @Volatile
    private var connectedEndpoint: ConnectedEndpoint? = null

    // 连接在/失联的可观察信号：clearEndpoint（如 PC 掉线）会把 false 抛出来，
    // DeviceViewModel 据此把 connectionState 翻成 DISCONNECTED，UI 不再永久停留在已连接。
    // 注意两层语义：connectedEndpoint 是内部“已学到端点”（one-shot 操作需要），
    // _connected 是对外“已配对且可用”——TOFU 首连在用户确认指纹前保持 false。
    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    /**
     * 新连接流程: ECDH 握手 + CONNECT_REQUEST/RESPONSE 配对(无图像传输)。
     * PC 侧对可信 peer 自动批准, 首次 peer 阻塞在 stdin [y/n] 提示。
     *
     * peerPubKeyB64 为空时走 TOFU: 握手期间学到 PC 公钥。成功返回学到的公钥
     * (base64), 由调用方持久化并展示指纹供用户核对; 失败返回 null。
     * 非空时严格 MITM 校验, 成功返回传入公钥。后续图像处理会用返回的公钥
     * 做严格校验, 所以即便 TOFU 首连也必须把它写回 endpoint。
     */
    fun requestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String
    ): String? {
        val normalizedHost = host.trim()
        val normalizedPeerPubKeyB64 = peerPubKeyB64.trim()
        if (normalizedHost.isBlank() || port <= 0) {
            logRepository.error("CoordinatorClient", "Invalid connection params")
            return null
        }

        // Switching to a different endpoint: drop the previous one first.
        val previousEndpoint = connectedEndpoint
        if (previousEndpoint != null &&
            (previousEndpoint.host != normalizedHost || previousEndpoint.port != port)
        ) {
            clearEndpoint(previousEndpoint)
        }

        val learnedPubKey = try {
            NativeWorker.nativeRequestConnection(
                host = normalizedHost,
                port = port,
                peerPubKeyB64 = normalizedPeerPubKeyB64
            )
        } catch (e: UnsatisfiedLinkError) {
            logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
            null
        } catch (e: Exception) {
            logRepository.error("CoordinatorClient", "Worker connect failed: ${e.message}")
            null
        }

        if (learnedPubKey != null) {
            // Use the (possibly learned) key for the endpoint so subsequent image
            // processing handshakes can do strict MITM verification against it.
            connectedEndpoint = ConnectedEndpoint(
                host = normalizedHost,
                port = port,
                peerPubKeyB64 = learnedPubKey
            )
            if (normalizedPeerPubKeyB64.isNotEmpty()) {
                // strict/QR-认证路径：本次连接已携带并校验 PC 公钥，立即视为已连接。
                _connected.value = true
                logRepository.info("CoordinatorClient", "Connected to $normalizedHost:$port")
            } else {
                // TOFU 首连：端点已学到（内部保留，供确认后 one-shot 使用），但
                // 对外保持“未连接”，等 DeviceViewModel 指纹确认后调用
                // markEndpointConfirmed 置位，防止未经确认的端点接走远程处理。
                logRepository.info(
                    "CoordinatorClient",
                    "TOFU handshake learned endpoint $normalizedHost:$port, awaiting user fingerprint confirm"
                )
            }
            return learnedPubKey
        }

        // Handshake/pairing failed (PC offline, rejected, MITM mismatch, ...).
        // Clear any stale endpoint so isConnected() reflects reality and the
        // caller falls back to native processing + can prompt the user.
        connectedEndpoint?.let { clearEndpoint(it) }
        logRepository.warning("CoordinatorClient", "Connect rejected by native worker")
        return null
    }

    /**
     * The authoritative "is a PC paired & reachable right now" signal.
     *
     * Reads the external [_connected] flag rather than `connectedEndpoint !=
     * null`: a TOFU endpoint learned during the handshake stays internal-only
     * until the user confirms the fingerprint, so unconfirmed endpoints never
     * win remote-processing routing. Strict/known-key connections set the flag
     * immediately in [requestConnection].
     */
    fun isConnected(): Boolean = _connected.value

    /**
     * TOFU 指纹确认后的置位：把握手阶段学到的 pending endpoint 翻成对外
     * “已连接”。端点已被替换或清除（如确认前又发起了一次连接/掉线）时返回
     * false，调用方需自行重连兜底。
     */
    fun markEndpointConfirmed(host: String, port: Int): Boolean {
        val endpoint = connectedEndpoint ?: return false
        if (endpoint.host != host.trim() || endpoint.port != port) {
            return false
        }
        _connected.value = true
        logRepository.info("CoordinatorClient", "Endpoint confirmed by user: ${endpoint.host}:${endpoint.port}")
        return true
    }

    fun activeDeviceId(): String? {
        val endpoint = connectedEndpoint ?: return null
        return "${endpoint.host}:${endpoint.port}"
    }

    // ── 共享模型库（手机↔PC）──────────────────────────────────────────
    // 与图像提交相同的 one-shot 模式：每次操作独立连接（握手+配对+操作帧）。

    private inline fun <T> withEndpoint(onMissing: () -> T, block: (host: String, port: Int, pubkey: String) -> T): T {
        val endpoint = connectedEndpoint ?: return onMissing()
        return block(endpoint.host, endpoint.port, endpoint.peerPubKeyB64)
    }

    /** 拉取 PC 模型库；未连接或失败返回 null。 */
    fun listPCModels(): List<PcModel>? = withEndpoint(
        onMissing = { null }
    ) { host, port, pubkey ->
        val records = NativeWorker.nativeListPCModels(host, port, pubkey) ?: return@withEndpoint null
        records.lineSequence()
            .filter { it.isNotBlank() }
            .mapNotNull { line ->
                val parts = line.split('\t')
                if (parts.size < 3) return@mapNotNull null
                val scale = parts[1].toIntOrNull() ?: return@mapNotNull null
                val size = parts[2].toLongOrNull() ?: return@mapNotNull null
                PcModel(name = parts[0], scaleFactor = scale, totalSize = size)
            }
            .toList()
    }

    /** 上传本地模型到 PC 模型库。未连接返回失败。 */
    fun uploadModelToPC(model: com.upix.data.model.ModelInfo): kotlin.Result<Unit> = withEndpoint(
        onMissing = { kotlin.Result.failure(IllegalStateException("未连接 PC")) }
    ) { host, port, pubkey ->
        if (NativeWorker.nativeUploadModelToPC(
                host = host,
                port = port,
                peerPubKeyB64 = pubkey,
                modelName = model.name,
                scaleFactor = model.scaleFactor,
                paramPath = model.paramPath,
                binPath = model.binPath
            )
        ) {
            Result.success(Unit)
        } else {
            Result.failure(IllegalStateException(lastNativeError()))
        }
    }

    /** 从 PC 模型库下载模型到 destParam/destBin 路径。未连接返回失败。 */
    fun downloadModelFromPC(
        name: String,
        destParamPath: String,
        destBinPath: String
    ): kotlin.Result<Unit> = withEndpoint(
        onMissing = { kotlin.Result.failure(IllegalStateException("未连接 PC")) }
    ) { host, port, pubkey ->
        if (NativeWorker.nativeDownloadModelFromPC(
                host = host,
                port = port,
                peerPubKeyB64 = pubkey,
                modelName = name,
                destParamPath = destParamPath,
                destBinPath = destBinPath
            )
        ) {
            Result.success(Unit)
        } else {
            Result.failure(IllegalStateException(lastNativeError()))
        }
    }

    private fun lastNativeError(): String =
        runCatching { NCNNEngine.nativeGetLastError() }.getOrNull()?.trim().orEmpty()
            .ifBlank { "传输失败" }

    fun processInputRemotely(
        input: ResolvedImageInput,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        outputFile: File
    ): Result<NativeProcessResult> {
        val endpoint = connectedEndpoint
            ?: return Result.failure(IllegalStateException("No connected coordinator endpoint"))

        // Ship the ORIGINAL encoded bytes; the PC decodes them. A 12 MP photo
        // uploads as ~4 MB of JPEG instead of ~36 MB of raw RGB, and the
        // phone never holds a full-size pixel buffer for distributed jobs.
        val (inputBytes, inputFormat) = readEncodedBytes(
            resolver = appContext.contentResolver,
            uri = input.uri,
            displayName = input.displayName
        ) { message ->
            logRepository.error("CoordinatorClient", message)
        } ?: return Result.failure(
            IllegalStateException("Failed to read image input: ${input.displayName}")
        )

        // Header-only bounds read: no full decode, keeps the phone thin.
        val inputBounds = decodeEncodedImageBounds(inputBytes)
            ?: return Result.failure(
                IllegalStateException("Input is not a decodable image: ${input.displayName}")
            )

        val outputFormat = outputFormatFor(outputFile)
        val startedAt = System.currentTimeMillis()
        val encodedOutput = NCNNEngine.nativeProcessImageDistributed(
            host = endpoint.host,
            port = endpoint.port,
            peerPubKeyB64 = endpoint.peerPubKeyB64,
            modelName = modelName,
            tileSize = tileSize.coerceAtLeast(MIN_TILE_SIZE),
            tileOverlap = tileOverlap.coerceAtLeast(0),
            inputFormat = inputFormat,
            inputImage = inputBytes,
            outputFormat = outputFormat,
            outputQuality = DEFAULT_OUTPUT_QUALITY
        )

        // null/empty output == PC unreachable / handshake failed / submit error.
        // Drop the endpoint (so the UI flips to DISCONNECTED) and surface a
        // failure the caller can catch. 引擎现在的行为是中止整个批次而不是
        // 静默回退本地，因此这里只如实报告“PC 不可达”。
        if (encodedOutput == null || encodedOutput.isEmpty()) {
            clearEndpoint(endpoint)
            val reason = runCatching { NCNNEngine.nativeGetLastError() }
                .getOrNull()
                ?.trim()
                .orEmpty()
            val message = if (reason.isNotBlank()) {
                "PC 不可达：$reason"
            } else {
                "PC 不可达"
            }
            logRepository.error("CoordinatorClient", message)
            return Result.failure(IllegalStateException(message))
        }

        val outputBounds = decodeOutputBounds(encodedOutput)
            ?: return Result.failure(IllegalStateException("Distributed output is not decodable"))

        val writeResult = writeOutputBytes(outputFile, encodedOutput)
        if (writeResult.isFailure) {
            // Local FS failure — keep the endpoint (PC is fine).
            return Result.failure(
                writeResult.exceptionOrNull()
                    ?: IllegalStateException("Failed to persist distributed output")
            )
        }

        val elapsedMs = System.currentTimeMillis() - startedAt
        logRepository.info(
            "CoordinatorClient",
            "Remote processed ${input.displayName} in ${elapsedMs}ms via ${endpoint.host}:${endpoint.port}"
        )

        return Result.success(
            NativeProcessResult(
                outputFile = outputFile,
                inputWidth = inputBounds.first,
                inputHeight = inputBounds.second,
                outputWidth = outputBounds.first,
                outputHeight = outputBounds.second,
            )
        )
    }

    fun disconnectIfMatches(host: String, port: Int): Boolean {
        val endpoint = connectedEndpoint ?: return false
        if (endpoint.host != host.trim() || endpoint.port != port) {
            return false
        }
        clearEndpoint(endpoint)
        return true
    }

    fun disconnect() {
        connectedEndpoint?.let { clearEndpoint(it) }
    }

    private fun clearEndpoint(endpoint: ConnectedEndpoint) {
        connectedEndpoint = null
        _connected.value = false
        logRepository.info("CoordinatorClient", "Disconnected from ${endpoint.host}:${endpoint.port}")
    }

    private fun decodeOutputBounds(encodedBytes: ByteArray): Pair<Int, Int>? {
        return decodeEncodedImageBounds(encodedBytes)
    }

    private fun writeOutputBytes(file: File, encodedBytes: ByteArray): Result<Unit> {
        return runCatching {
            val parent = file.parentFile
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw IllegalStateException("Failed to create output directory: ${parent.absolutePath}")
            }
            FileOutputStream(file, false).use { outputStream ->
                outputStream.write(encodedBytes)
                outputStream.flush()
            }
        }
    }

    private data class ConnectedEndpoint(
        val host: String,
        val port: Int,
        val peerPubKeyB64: String
    )

    private companion object {
        private const val DEFAULT_OUTPUT_QUALITY = 95
        private const val MIN_TILE_SIZE = 16
    }
}
