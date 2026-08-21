package com.crossscale.network

import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import com.crossscale.data.model.PcModel
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.NativeProcessResult
import com.crossscale.data.repository.ResolvedImageInput
import com.crossscale.data.repository.RGB_CHANNELS
import com.crossscale.data.repository.decodeRgb
import com.crossscale.data.repository.outputFormatFor
import com.crossscale.nativelib.NCNNEngine
import com.crossscale.nativelib.NativeWorker
import java.io.File
import java.io.FileOutputStream

class CoordinatorClient(
    context: Context,
    private val logRepository: LogRepository
) {
    private val appContext = context.applicationContext

    // Internal representation: carries the peer pubkey + stream flag needed to
    // re-run strict-MITM handshakes for each one-shot image submit. The public,
    // UI-facing endpoint shape (data.model.RemoteEndpoint) is owned by
    // DeviceViewModel, which derives it from these outcomes.
    private var connectedEndpoint: ConnectedEndpoint? = null

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
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
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
                peerPubKeyB64 = normalizedPeerPubKeyB64,
                wantsEncryptedStream = wantsEncryptedStream
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
                peerPubKeyB64 = learnedPubKey,
                streamEncrypted = wantsEncryptedStream
            )
            logRepository.info("CoordinatorClient", "Connected to $normalizedHost:$port")
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
     * Previously this read a persistent worker-session `running_` flag that the
     * one-shot pairing path never set, then self-cleared the endpoint via
     * disconnect() — silently forcing native fallback forever. Now it is simply
     * `connectedEndpoint != null`, which is exactly what the pairing path sets.
     */
    fun isConnected(): Boolean = connectedEndpoint != null

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
    fun uploadModelToPC(model: com.crossscale.data.model.ModelInfo): kotlin.Result<Unit> = withEndpoint(
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

        val decodedInput = decodeRgb(
            resolver = appContext.contentResolver,
            uri = input.uri
        ) { message ->
            logRepository.error("CoordinatorClient", message)
        } ?: return Result.failure(
            IllegalStateException("Failed to decode image input: ${input.displayName}")
        )

        val outputFormat = outputFormatFor(outputFile)
        val startedAt = System.currentTimeMillis()
        val encodedOutput = NCNNEngine.nativeProcessImageDistributed(
            host = endpoint.host,
            port = endpoint.port,
            peerPubKeyB64 = endpoint.peerPubKeyB64,
            wantsEncryptedStream = endpoint.streamEncrypted,
            modelName = modelName,
            tileSize = tileSize.coerceAtLeast(MIN_TILE_SIZE),
            tileOverlap = tileOverlap.coerceAtLeast(0),
            inputImage = decodedInput.rgbBytes,
            width = decodedInput.width,
            height = decodedInput.height,
            channels = RGB_CHANNELS,
            outputFormat = outputFormat,
            outputQuality = DEFAULT_OUTPUT_QUALITY
        )

        // null/empty output == PC unreachable / handshake failed / submit error.
        // This is the remote-fallback trigger: drop the endpoint (so the UI flips
        // to Native and the caller falls back for this image) and surface a
        // failure the caller can catch.
        if (encodedOutput == null || encodedOutput.isEmpty()) {
            clearEndpoint(endpoint)
            val reason = runCatching { NCNNEngine.nativeGetLastError() }
                .getOrNull()
                ?.trim()
                .orEmpty()
            val message = if (reason.isNotBlank()) {
                "PC 不可达，已切换为本地处理：$reason"
            } else {
                "PC 不可达，已切换为本地处理"
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
                inputWidth = decodedInput.width,
                inputHeight = decodedInput.height,
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
        logRepository.info("CoordinatorClient", "Disconnected from ${endpoint.host}:${endpoint.port}")
    }

    private fun decodeOutputBounds(encodedBytes: ByteArray): Pair<Int, Int>? {
        val options = BitmapFactory.Options().apply {
            inJustDecodeBounds = true
        }
        BitmapFactory.decodeByteArray(encodedBytes, 0, encodedBytes.size, options)
        if (options.outWidth <= 0 || options.outHeight <= 0) {
            return null
        }
        return options.outWidth to options.outHeight
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
        val peerPubKeyB64: String,
        val streamEncrypted: Boolean = false
    )

    private companion object {
        private const val DEFAULT_OUTPUT_QUALITY = 95
        private const val MIN_TILE_SIZE = 16
    }
}
