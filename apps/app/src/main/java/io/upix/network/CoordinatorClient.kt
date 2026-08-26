package io.upix.network

import android.content.Context
import android.net.Uri
import io.upix.data.model.PcModel
import io.upix.data.repository.EncoderSettings
import io.upix.data.repository.LogRepository
import io.upix.data.repository.NativeProcessResult
import io.upix.data.repository.ResolvedImageInput
import io.upix.data.repository.decodeEncodedImageBounds
import io.upix.data.repository.isAvifName
import io.upix.data.repository.readEncodedBytes
import io.upix.nativelib.NCNNEngine
import io.upix.nativelib.NativeCapabilities
import io.upix.nativelib.PeerCapabilities
import io.upix.nativelib.NativeWorker
import java.io.File
import java.io.FileOutputStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

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
            // The pairing handshake just succeeded: the CONNECT_RESPONSE carried
            // the PC's AVIF codec flags — pick them up for format gating.
            PeerCapabilities.refresh()
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
    fun uploadModelToPC(model: io.upix.data.model.ModelInfo): kotlin.Result<Unit> = withEndpoint(
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

    // ── 远程处理：持久会话 + 深度 1 流水线 ──────────────────────────────
    // 一个批次只做一次握手/配对（nativeOpenDistributedSession），之后每张图
    // send（上传）→ receive（收结果+进度）→ persist（写盘）三段拆开：
    // 上传 N+1 与 PC 推理 N 重叠，写盘 N 与 PC 解码/推理 N+1 重叠。

    /** 持久会话句柄（0=未打开）。RunEngine 的单循环串行访问，无并发。 */
    @Volatile
    private var nativeSessionHandle: Long = 0L

    // 单槽预取缓存：等当前图结果时后台读下一张原图字节（与 PC 推理重叠）。
    private val prefetchScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var prefetchJob: Job? = null

    private data class PrefetchedInput(val uri: Uri, val bytes: ByteArray, val format: String)

    @Volatile
    private var prefetched: PrefetchedInput? = null

    /** 后台预读下一张图的原始编码字节；beginRemoteTask 命中即免同步读。 */
    fun prefetchRemoteInput(input: ResolvedImageInput) {
        if (prefetched?.uri == input.uri) {
            return
        }
        val target = input.uri
        val displayName = input.displayName
        prefetchJob?.cancel()
        prefetchJob = prefetchScope.launch {
            val read = readEncodedBytes(
                resolver = appContext.contentResolver,
                uri = target,
                displayName = displayName
            ) { message ->
                logRepository.error("CoordinatorClient", message)
            } ?: return@launch
            prefetched = PrefetchedInput(target, read.first, read.second)
        }
    }

    /** 一张已上传、等待结果的远程任务（数据由 beginRemoteTask 快照持有）。 */
    inner class RemoteTask internal constructor(
        internal val input: ResolvedImageInput,
        private val inputBounds: Pair<Int, Int>,
        private val inputFormat: String,
        private val inputBytes: ByteArray,
        private val outputFormat: String,
        private val settings: EncoderSettings,
        private val modelName: String,
        private val tileSize: Int,
        private val tileOverlap: Int
    ) {
        /**
         * 阻塞等待 PC 结果；进度帧通过 [onProgress] 流出（stage 0=推理
         * percent/eta，1=编码）。PC 侧任务失败（"Distributed task failed:"）
         * 保持会话与端点；传输层失败则关闭会话并清空端点。
         */
        fun awaitResult(
            onProgress: (stage: Int, percent: Int, etaSeconds: Int) -> Unit
        ): Result<ByteArray> {
            val handle = nativeSessionHandle
            if (handle == 0L) {
                return Result.failure(IllegalStateException("Distributed session is closed"))
            }
            val encodedOutput = try {
                NCNNEngine.nativeReceiveDistributedResult(
                    handle,
                    NCNNEngine.DistributedProgressListener(onProgress)
                )
            } catch (e: UnsatisfiedLinkError) {
                logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
                null
            }
            if (encodedOutput == null || encodedOutput.isEmpty()) {
                val reason = runCatching { NCNNEngine.nativeGetLastError() }
                    .getOrNull()
                    ?.trim()
                    .orEmpty()
                val taskRejected = reason.startsWith("Distributed task failed:")
                if (!taskRejected) {
                    closeSession()
                    connectedEndpoint?.let { clearEndpoint(it) }
                }
                val pcReason = reason.removePrefix("Distributed task failed: ").trim()
                val message = when {
                    taskRejected && pcReason.isNotBlank() ->
                        if (pcReason.contains("avif", ignoreCase = true)) {
                            "$pcReason（远端 AVIF 需要 PC 安装 libavif，如 pacman -S libavif）"
                        } else {
                            "PC 任务失败：$pcReason"
                        }
                    reason.isNotBlank() -> "PC 不可达：$reason"
                    else -> "PC 不可达"
                }
                logRepository.error("CoordinatorClient", message)
                return Result.failure(IllegalStateException(message))
            }
            return Result.success(encodedOutput)
        }

        /** 把 PC 返回的编码字节写盘并组装结果（纯本地 IO，可与其他事并行）。 */
        fun persist(encodedOutput: ByteArray, outputFile: File): Result<NativeProcessResult> {
            val outputBounds = decodeEncodedImageBounds(encodedOutput)
                ?: return Result.failure(IllegalStateException("Distributed output is not decodable"))
            val writeResult = writeOutputBytes(outputFile, encodedOutput)
            if (writeResult.isFailure) {
                return Result.failure(
                    writeResult.exceptionOrNull()
                        ?: IllegalStateException("Failed to persist distributed output")
                )
            }
            logRepository.info(
                "CoordinatorClient",
                "Remote processed ${input.displayName} -> ${outputFile.absolutePath}"
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
    }

    /**
     * 读字节（命中预取则直接用）→ 校验 → 确保/复用会话 → 上传（CLIENT_SUBMIT）。
     * 传输失败时重开会话重发一次（容忍 PC 重启/旧版 PC 用完即关），再失败
     * 才视为掉线（清端点）。
     */
    fun beginRemoteTask(
        input: ResolvedImageInput,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        outputFormat: String
    ): Result<RemoteTask> {
        val endpoint = connectedEndpoint
            ?: return Result.failure(IllegalStateException("No connected coordinator endpoint"))

        // Preflight: an AVIF input needs SOMEONE on the path that can decode it.
        // Remote mode ships the original bytes — only the PC's avif-decode
        // matters here. Reject before uploading instead of failing mid-task.
        if (isAvifName(input.displayName) && !PeerCapabilities.avifDecode) {
            val message = "PC 端不支持 AVIF 解码：在 PC 上安装 libavif（如 pacman -S libavif）后重试"
            logRepository.error("CoordinatorClient", message)
            return Result.failure(IllegalStateException(message))
        }

        // Ship the ORIGINAL encoded bytes; the PC decodes them. A 12 MP photo
        // uploads as ~4 MB of JPEG instead of ~36 MB of raw RGB, and the
        // phone never holds a full-size pixel buffer for distributed jobs.
        val cached = prefetched
        prefetched = null
        val (inputBytes, inputFormat) = if (cached != null && cached.uri == input.uri) {
            cached.bytes to cached.format
        } else {
            readEncodedBytes(
                resolver = appContext.contentResolver,
                uri = input.uri,
                displayName = input.displayName
            ) { message ->
                logRepository.error("CoordinatorClient", message)
            } ?: return Result.failure(
                IllegalStateException("Failed to read image input: ${input.displayName}")
            )
        }

        // Header-only bounds read: no full decode, keeps the phone thin.
        val inputBounds = decodeEncodedImageBounds(inputBytes)
            ?: return Result.failure(
                IllegalStateException("Input is not a decodable image: ${input.displayName}")
            )

        val settings = encoderSettings
        // 0 is the protocol's "auto" sentinel: pass it through untouched so
        // the PC picks the tile (TileSizeOptimizer, cached per model); any
        // explicit size is clamped to the 16px protocol minimum.
        val normalizedTileSize =
            if (tileSize == 0) 0 else tileSize.coerceAtLeast(MIN_TILE_SIZE)
        val normalizedOverlap = tileOverlap.coerceAtLeast(0)
        val task = RemoteTask(
            input = input,
            inputBounds = inputBounds,
            inputFormat = inputFormat,
            inputBytes = inputBytes,
            outputFormat = outputFormat,
            settings = settings,
            modelName = modelName,
            tileSize = normalizedTileSize,
            tileOverlap = normalizedOverlap
        )

        val send: (Long) -> Boolean = { handle ->
            try {
                NCNNEngine.nativeSendDistributedTask(
                    handle,
                    modelName,
                    normalizedTileSize,
                    normalizedOverlap,
                    inputFormat,
                    inputBytes,
                    outputFormat,
                    settings.normalizedQuality,
                    settings.toNativeJson()
                )
            } catch (e: UnsatisfiedLinkError) {
                logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
                false
            }
        }

        if (nativeSessionHandle == 0L && !openSession(endpoint)) {
            return Result.failure(IllegalStateException("PC 不可达：${lastNativeError()}"))
        }
        if (send(nativeSessionHandle)) {
            return Result.success(task)
        }
        // 传输层失败：PC 可能重启（或是不支持会话循环的旧版）。重开一次再发。
        closeSession()
        if (!openSession(endpoint) || !send(nativeSessionHandle)) {
            val reason = lastNativeError()
            closeSession()
            clearEndpoint(endpoint)
            val message = if (reason.isNotBlank()) "PC 不可达：$reason" else "PC 不可达"
            logRepository.error("CoordinatorClient", message)
            return Result.failure(IllegalStateException(message))
        }
        return Result.success(task)
    }

    /** 批次结束时关闭会话并丢弃预取（RunEngine 的 finally 调用）。 */
    fun endRemoteSession() {
        prefetchJob?.cancel()
        prefetchJob = null
        prefetched = null
        closeSession()
    }

    private fun openSession(endpoint: ConnectedEndpoint): Boolean {
        nativeSessionHandle = try {
            NCNNEngine.nativeOpenDistributedSession(
                endpoint.host,
                endpoint.port,
                endpoint.peerPubKeyB64
            )
        } catch (e: UnsatisfiedLinkError) {
            logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
            0L
        } catch (e: Exception) {
            logRepository.error("CoordinatorClient", "Session open failed: ${e.message}")
            0L
        }
        if (nativeSessionHandle == 0L) {
            return false
        }
        // CONNECT_RESPONSE 携带的 AVIF 能力位在这条连接上重新学到一次。
        PeerCapabilities.refresh()
        return true
    }

    private fun closeSession() {
        val handle = nativeSessionHandle
        nativeSessionHandle = 0L
        if (handle != 0L) {
            try {
                NCNNEngine.nativeCloseDistributedSession(handle)
            } catch (e: Throwable) {
                logRepository.warning("CoordinatorClient", "Session close failed: ${e.message}")
            }
        }
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
        // 端点失效：挂在其上的持久会话一并关闭。
        closeSession()
        prefetched = null
        connectedEndpoint = null
        _connected.value = false
        PeerCapabilities.reset()
        logRepository.info("CoordinatorClient", "Disconnected from ${endpoint.host}:${endpoint.port}")
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

    // 高级编码参数由 MainViewModel 推送（与本地编码同一来源）；快照语义。
    @Volatile
    private var encoderSettings = EncoderSettings()

    fun updateEncoderSettings(settings: EncoderSettings) {
        encoderSettings = settings
    }

    private companion object {
        private const val MIN_TILE_SIZE = 16
    }
}
