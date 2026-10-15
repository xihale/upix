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
import kotlinx.coroutines.CancellationException
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
            // 新端点：分块支持未知，恢复默认（>4MB 走分块）。
            chunkedSubmitUnsupported = false
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

    /**
     * 持久会话句柄（0=未打开）。RunEngine 的单循环串行访问是常态，但关闭/
     * 取消来自别的线程（DeviceViewModel.disconnect、onCleared、主线程取消），
     * 因此本字段的每次读写都在 [sessionOpGate] 内（见其注释的接收↔关闭协议）；
     * @Volatile 只是纵深防御，正确性不依赖它。
     */
    @Volatile
    private var nativeSessionHandle: Long = 0L

    // 单槽预取缓存：等当前图结果时后台读下一张原图字节（与 PC 推理重叠）。
    private val prefetchScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var prefetchJob: Job? = null

    private data class PrefetchedInput(val uri: Uri, val bytes: ByteArray, val format: String)

    @Volatile
    private var prefetched: PrefetchedInput? = null

    /**
     * 当前端点的 PC 不支持分块提交（收到 "truncated/interrupted" 应答或
     * 分块后立即摔线时置位；换端点/掉线时复位）。置位后大图直发单帧。
     */
    @Volatile
    private var chunkedSubmitUnsupported = false

    /** 一次 awaitResult 的原始结果（清理/降级决策交给外层）。 */
    private class AwaitOutcome(
        val bytes: ByteArray?,
        val reason: String,
        val taskRejected: Boolean,
        val userCanceled: Boolean
    )

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
        /** 本次上传是否走了分块格式（>4MB）。决定 awaitResult 的降级策略。 */
        internal var sentChunked = false

        /** 分块提交被拒后的单帧重发（旧 PC 降级路径）。 */
        private fun resendSingleFrame(): Boolean {
            return try {
                // 门闩内完成：防句柄在重发中途被关闭路径释放。
                synchronized(sessionOpGate) {
                    val handle = nativeSessionHandle
                    if (handle == 0L) {
                        false
                    } else {
                        NCNNEngine.nativeSendDistributedTask(
                            handle,
                            modelName,
                            tileSize,
                            tileOverlap,
                            inputFormat,
                            inputBytes,
                            outputFormat,
                            settings.normalizedQuality,
                            settings.toNativeJson(),
                            /*allowChunked=*/ false
                        )
                    }
                }
            } catch (e: UnsatisfiedLinkError) {
                logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
                false
            }
        }

        /**
         * 阻塞等待 PC 结果；进度帧通过 [onProgress] 流出（stage 0=推理
         * percent/eta，1=编码）。PC 侧任务失败（"Distributed task failed:"）
         * 保持会话与端点；传输层失败则关闭会话并清空端点。
         *
         * 取消（[cancelRemoteTask] 已请求）：以 CancellationException 抛出，
         * 而不是按普通失败处理——用户主动取消不应把当前图记为失败，也不应
         * 清掉已配对端点。会话视情况关闭（旧 PC 会摔线），端点保留。
         *
         * 分块降级：本任务是分块上传且失败呈现为 "truncated/interrupted"
         * per-task 错误（新版 PC）或分块后立即传输失败（旧 PC 直接摔线）时，
         * 若尺寸允许则自动降级为单帧重发一次（新 PC 同会话重发；旧 PC 重开
         * 会话重发，端点保留），并记住该端点不支持分块，后续大图直发单帧。
         */
        fun awaitResult(
            onProgress: (stage: Int, percent: Int, etaSeconds: Int) -> Unit
        ): Result<ByteArray> {
            var outcome = awaitResultOnce(onProgress)
            if (outcome.reason.isNotEmpty() && sentChunked &&
                inputBytes.size <= SINGLE_FRAME_LIMIT_BYTES
            ) {
                val rejectedChunking = outcome.taskRejected &&
                    (outcome.reason.contains("truncat", ignoreCase = true) ||
                        outcome.reason.contains("interrupted", ignoreCase = true))
                val transportAfterChunked = !outcome.taskRejected && !outcome.userCanceled
                if (rejectedChunking || transportAfterChunked) {
                    chunkedSubmitUnsupported = true
                    logRepository.warning(
                        "CoordinatorClient",
                        "Chunked submit rejected by PC (${outcome.reason}); retrying as a single frame"
                    )
                    outcome = if (rejectedChunking) {
                        // 新 PC：会话仍活着，直接同会话单帧重发。
                        if (resendSingleFrame()) awaitResultOnce(onProgress) else outcome
                    } else {
                        // 旧 PC 摔线：关会话重开（端点保留）再单帧重发。
                        closeSession()
                        val endpoint = connectedEndpoint
                        if (endpoint != null && openSession(endpoint) && resendSingleFrame()) {
                            awaitResultOnce(onProgress)
                        } else {
                            outcome
                        }
                    }
                }
            }

            // 取消路径（native "Distributed task canceled..." 或 PC 的
            // error="canceled"）：不算失败、不清端点。
            if (outcome.userCanceled) {
                closeSession()
                throw CancellationException("已取消（PC 已停止计算）")
            }
            if (outcome.bytes != null && outcome.bytes.isNotEmpty()) {
                return Result.success(outcome.bytes)
            }
            if (!outcome.taskRejected) {
                closeSession()
                connectedEndpoint?.let { clearEndpoint(it) }
            }
            val reason = outcome.reason
            val pcReason = reason.removePrefix("Distributed task failed: ").trim()
            val message = when {
                outcome.taskRejected && pcReason.isNotBlank() ->
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

        /** 一次完整的“等待结果”。不做任何会话/端点清理（交给 awaitResult）。 */
        private fun awaitResultOnce(
            onProgress: (stage: Int, percent: Int, etaSeconds: Int) -> Unit
        ): AwaitOutcome {
            // 句柄在门闩内取并同时标记“接收在途”：nativeClose 若在此期间
            // 到达会转交本次接收收尾释放（见 closeSession），否则会在 native
            // 解帧途中 delete 掉会话对象。门闩只覆盖取句柄这一瞬，随后的阻塞
            // 接收不持锁，取消/关闭不会被接收时长拖住。
            val handle = beginReceive()
            if (handle == 0L) {
                return AwaitOutcome(null, "Distributed session is closed",
                    taskRejected = false, userCanceled = false)
            }
            val encodedOutput = try {
                NCNNEngine.nativeReceiveDistributedResult(
                    handle,
                    NCNNEngine.DistributedProgressListener(onProgress)
                )
            } catch (e: UnsatisfiedLinkError) {
                logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
                null
            } finally {
                endReceive(handle)
            }
            if (encodedOutput != null && encodedOutput.isNotEmpty()) {
                return AwaitOutcome(encodedOutput, "", taskRejected = false, userCanceled = false)
            }
            val reason = runCatching { NCNNEngine.nativeGetLastError() }
                .getOrNull()
                ?.trim()
                .orEmpty()
            val taskRejected = reason.startsWith("Distributed task failed:")
            val userCanceled = reason.startsWith("Distributed task canceled") ||
                (taskRejected && reason.removePrefix("Distributed task failed:").trim() == "canceled")
            return AwaitOutcome(null, reason, taskRejected, userCanceled)
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
    ): Result<RemoteTask> = beginRemoteTaskImpl(
        input, modelName, tileSize, tileOverlap, outputFormat, recoverOnFailure = true
    )

    /**
     * 深度2发前窗口专用：语义同 [beginRemoteTask]，但任何失败都不做恢复——
     * 不重开、不清端点（那些动作会与阻塞中的 awaitResult 抢同一会话）。
     * 发前失败即批次失败，由引擎的串行路径统一上报终止。
     */
    fun beginRemoteTaskAhead(
        input: ResolvedImageInput,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        outputFormat: String
    ): Result<RemoteTask> = beginRemoteTaskImpl(
        input, modelName, tileSize, tileOverlap, outputFormat, recoverOnFailure = false
    )

    private fun beginRemoteTaskImpl(
        input: ResolvedImageInput,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        outputFormat: String,
        recoverOnFailure: Boolean
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
        // >4MB 走分块上传（削峰值内存），除非该端点已被确认不支持分块。
        val useChunked =
            !chunkedSubmitUnsupported && inputBytes.size > CHUNKED_SUBMIT_THRESHOLD_BYTES
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
        task.sentChunked = useChunked

        val send: (Long, Boolean) -> Boolean = { handle, allowChunked ->
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
                    settings.toNativeJson(),
                    allowChunked
                )
            } catch (e: UnsatisfiedLinkError) {
                logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
                false
            }
        }

        // 确保会话 + 发送同处一个临界区：句柄的判空/打开/使用不再跨临界区
        // 分步（既避免两次真并发上传各开一条会话把其中一条泄漏掉，也让取消/
        // 掉线路径的 closeSession 与整段上传互斥——句柄不会在 native 上传
        // 中途被删除，见 sessionOpGate 注释）。`send` 之外没有别的持有门闩的
        // 阻塞调用。
        val sent = synchronized(sessionOpGate) {
            if (nativeSessionHandle == 0L) {
                if (!recoverOnFailure) {
                    // 发前路径禁止在这里开新会话（与 awaitResult 抢句柄）。
                    return Result.failure(IllegalStateException("Distributed session is closed"))
                }
                if (!openSession(endpoint)) {
                    return Result.failure(IllegalStateException("PC 不可达：${lastNativeError()}"))
                }
            }
            send(nativeSessionHandle, useChunked)
        }
        if (sent) {
            return Result.success(task)
        }
        if (!recoverOnFailure) {
            return Result.failure(IllegalStateException(lastNativeError().ifBlank { "upload failed" }))
        }
        // 传输层失败：PC 可能重启（或是不支持会话循环的旧版）。重开一次再发；
        // 若首次是分块发送，则按“旧 PC 不支持分块”处理，重发强制单帧并记
        // 住该端点不支持分块（后续大图少付一次分块失败）。
        if (useChunked) {
            chunkedSubmitUnsupported = true
        }
        closeSession()
        val resent = if (openSession(endpoint)) withSessionHandle { send(it, false) } else null
        if (resent != true) {
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

    /**
     * 请求取消在途的远程任务（取消按钮接线）：向会话发 MSG_TASK_CANCEL 的
     * 请求标志。阻塞中的 awaitResult 会在一个轮询周期内发出取消帧，并在 PC
     * 停止推理、回出 "canceled" 终止应答后以 CancellationException 返回。
     * 无会话/无在途任务时为空操作（标志在新一次等待开始时清零）。
     */
    fun cancelRemoteTask() {
        // 句柄快照与 native 调用同在门闩内：发前上传可能正持锁上传，取消等
        // 待其帧界；延后释放的旧句柄也只有在本门闩内才确定还活着（释放方与
        // 接收收尾都在门闩内迁移状态），不会对已 delete 的会话对象置标志。
        synchronized(sessionOpGate) {
            // 取消落在「用户正等的那次接收」上：优先当前会话句柄（若它正在
            // 接收），否则取最新的在途接收句柄——掉线/断连已把 nativeSessionHandle
            // 摘掉、阻塞中的 nativeReceiveDistributedResult 还在轮询时，取消必须
            // 落到那个句柄上，否则接收永远等不到取消标志。没有接收在途时才是
            // 当前会话句柄（发/收间隙，标志由下一次接收开始时清零）。
            val current = nativeSessionHandle
            val handle = when {
                current != 0L && receiveInFlight.containsKey(current) -> current
                receiveInFlight.isNotEmpty() -> receiveInFlight.keys.last()
                else -> current
            }
            if (handle == 0L) {
                return
            }
            try {
                NCNNEngine.nativeCancelDistributedTask(handle)
            } catch (e: Throwable) {
                logRepository.warning("CoordinatorClient", "Remote task cancel failed: ${e.message}")
            }
        }
    }

    // 会话句柄生命周期门闩。深度2发前窗口里，上传跑在独立线程上，而失败/
    // 取消路径会在引擎协程线程上关闭会话——nativeClose 会删除句柄背后的
    // 对象，持着旧句柄调 nativeSend 就是 use-after-free。因此句柄的每一次
    // 状态迁移都在门闩内完成：打开、取用（发送/取消）、登记接收在途、注销
    // 接收、释放。阻塞式接收是唯一例外：门闩只包「取句柄 + 登记在途」这一
    // 瞬，随后的 native 接收不持锁，否则用户取消 / 掉线会被整段接收时长拖
    // 住（主线程调用 cancelProcessing 会直接卡死）。
    //
    // 接收↔关闭协议（两端的状态迁移都只在本门闩内做，因此可线性化）：
    //  - beginReceive：门闩内取句柄并 receiveInFlight[handle]++。
    //  - closeSession：门闩内摘掉 nativeSessionHandle；该句柄有在途接收
    //    就放进 deferredClose 转交接收线程收尾释放，否则当场释放。
    //  - endReceive：门闩内注销自己那个句柄，并取走它的延后关闭。
    // 「检查 + 迁移」必须同处一个临界区：只要读/写分处临界区内外，就会出现
    // (1) closeSession 检查到"有在途接收"后才写 deferred，而接收线程已在检查
    // 与写入之间取走 0 —— 那次关闭从此无人释放（永久泄漏）；(2) 反向交错时
    // 接收线程取走了新会话的延后句柄并 delete 掉它，而新接收正在 native 里用
    // 它（UAF）。因此这里用普通集合而非 AtomicLong 单槽，读写一律在门闩内。
    // 集合而非单槽还消除了句柄覆盖：掉线时旧会话的接收可能仍在 native 里阻塞
    // （接收方与关闭方分属不同 ViewModel/线程：DeviceViewModel.disconnect、
    // onCleared 都会关会话），用户重连后新会话另开句柄，两个在途句柄必须各记
    // 各的，旧 endReceive 只注销/释放旧句柄，永远碰不到新会话。
    private val sessionOpGate = Any()

    /** 门闩内取当前句柄执行 [block]；无会话返回 null。 */
    private inline fun <T> withSessionHandle(block: (Long) -> T): T? {
        synchronized(sessionOpGate) {
            val handle = nativeSessionHandle
            return if (handle == 0L) null else block(handle)
        }
    }

    /** handle → 该句柄上正在阻塞的接收次数（仅门闩内读写）。 */
    private val receiveInFlight = mutableMapOf<Long, Int>()

    /** 有在途接收、等接收线程收尾释放的句柄（仅门闩内读写）。 */
    private val deferredClose = mutableSetOf<Long>()

    /**
     * 门闩内取当前句柄并登记「接收在途」：返回后不持锁，阻塞接收不阻塞
     * 取消/掉线。登记与取句柄同处一个临界区，并发的 [closeSession] 只能看到
     * 两种一致状态之一——未登记（当场释放）或已登记（转交本次接收收尾释放）。
     */
    private fun beginReceive(): Long = synchronized(sessionOpGate) {
        val handle = nativeSessionHandle
        if (handle != 0L) {
            receiveInFlight[handle] = (receiveInFlight[handle] ?: 0) + 1
        }
        handle
    }

    /**
     * 门闩内注销本次接收并释放撞上它的那次延后关闭。只动 [handle] 自己的
     * 条目：旧会话的接收收尾绝不会释放（或注销）新会话的句柄。
     */
    private fun endReceive(handle: Long) {
        if (handle == 0L) {
            return
        }
        synchronized(sessionOpGate) {
            val remaining = (receiveInFlight[handle] ?: 1) - 1
            if (remaining > 0) {
                receiveInFlight[handle] = remaining
                return
            }
            receiveInFlight.remove(handle)
            if (deferredClose.remove(handle)) {
                // 该句柄的 native 接收已退出；关闭方在门闩内已先摘掉
                // nativeSessionHandle，发送路径再也拿不到它，此刻释放安全。
                closeNativeSession(handle)
            }
        }
    }

    private fun closeNativeSession(handle: Long) {
        try {
            NCNNEngine.nativeCloseDistributedSession(handle)
        } catch (e: Throwable) {
            logRepository.warning("CoordinatorClient", "Session close failed: ${e.message}")
        }
    }

    private fun openSession(endpoint: ConnectedEndpoint): Boolean {
        synchronized(sessionOpGate) {
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
            // CONNECT_RESPONSE 携带的能力位（AVIF 编解码 + 流水线深度）在这条
            // 连接上重新学到一次。
            PeerCapabilities.refresh()
            return true
        }
    }

    private fun closeSession() {
        synchronized(sessionOpGate) {
            val handle = nativeSessionHandle
            nativeSessionHandle = 0L
            if (handle == 0L) {
                return
            }
            if ((receiveInFlight[handle] ?: 0) > 0) {
                // 接收线程正阻塞在该句柄的 native 调用里：转交它收尾释放，绝不
                // 在 native 解帧途中 delete 会话对象。代价是该 socket 在本次
                // 接收结束前不关闭（PC 仍在算/仍在发进度帧），接收要么拿到结果、
                // 要么等用户取消后退出——两条路都会回到 endReceive 在有限时间内
                // 释放（native 侧接收有 200ms 轮询 + 取消 90s 排空上限）。
                deferredClose.add(handle)
            } else {
                closeNativeSession(handle)
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
        chunkedSubmitUnsupported = false
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

        /** 超过此大小的输入走分块上传（与 native SINGLE_FRAME_SUBMIT_BYTES 一致）。 */
        private const val CHUNKED_SUBMIT_THRESHOLD_BYTES: Int = 4 * 1024 * 1024

        /**
         * 单帧提交的字节上限（native MAX_SUBMIT_INPUT_BYTES：100MB 帧上限
         * 减去 struct + nonce/MAC 余量）。超过它的输入只能分块，无降级余地。
         */
        private const val SINGLE_FRAME_LIMIT_BYTES: Int = 100 * 1024 * 1024 - 512
    }
}
