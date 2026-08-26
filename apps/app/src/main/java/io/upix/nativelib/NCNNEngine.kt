package io.upix.nativelib

object NCNNEngine {
    init {
        System.loadLibrary("upix-native")
    }

    external fun nativeInit(deviceType: Int): Long
    external fun nativeLoadModel(enginePtr: Long, modelPath: String): Boolean
    external fun nativeGetLastError(): String?
    external fun nativeCapabilities(): String

    /**
     * AVIF capabilities of the paired PC, learned from the latest successful
     * pairing handshake (CONNECT_RESPONSE caps fields). Same comma-token
     * format as [nativeCapabilities]; empty = nothing learned / no PC.
     */
    external fun nativePeerCapabilities(): String
    external fun nativeProcessImage(
        enginePtr: Long,
        inputImage: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        outputFormat: String,
        outputQuality: Int,
        encoderOptionsJson: String
    ): ByteArray?
    /**
     * PC 任务进度回调（PC→手机流式 TASK_PROGRESS）。[stage] 0=推理中（此时
     * [percent]/[etaSeconds] 有效），1=推理完成、PC 正在编码结果。
     */
    fun interface DistributedProgressListener {
        fun onProgress(stage: Int, percent: Int, etaSeconds: Int)
    }

    /**
     * 打开持久分布式会话：TCP 连接 + ECDH 握手 + 配对，整批任务只做一次。
     * 返回句柄（0=失败，详情走 [nativeGetLastError]）。
     */
    external fun nativeOpenDistributedSession(
        host: String,
        port: Int,
        peerPubKeyB64: String
    ): Long

    /**
     * 在打开的会话上上传一张图（seal + 发送 CLIENT_SUBMIT）。与接收拆分，
     * 调用方可在 PC 还在算第 N 张时先把第 N+1 张推上去。
     */
    external fun nativeSendDistributedTask(
        handle: Long,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        inputFormat: String,
        inputImage: ByteArray,
        outputFormat: String,
        outputQuality: Int,
        encoderOptionsJson: String
    ): Boolean

    /**
     * 阻塞等待当前任务结果，期间把 PC 的进度帧转发给 [progressListener]
     * （可空）。返回编码后的输出字节；null=失败（nativeGetLastError，
     * "Distributed task failed:" 前缀表示 PC 侧任务错误、会话仍可用）。
     * 内部以 ~200ms 轮询接收：[nativeCancelDistributedTask] 置位后，接收线程
     * 自己向 PC 发 MSG_TASK_CANCEL，并在 PC 回出终止响应（"canceled"）后尽快
     * 返回 "Distributed task canceled..." 错误。
     */
    external fun nativeReceiveDistributedResult(
        handle: Long,
        progressListener: DistributedProgressListener?
    ): ByteArray?

    /**
     * 请求取消该会话上在途的远程任务（线程安全、非阻塞）：仅置取消标志，
     * 由阻塞中的 [nativeReceiveDistributedResult] 在一个轮询周期内发出
     * MSG_TASK_CANCEL 并尽快以"已取消"返回。无在途任务时为空操作。
     */
    external fun nativeCancelDistributedTask(handle: Long): Boolean

    /** 关闭并释放会话句柄；0 与重复关闭安全。 */
    external fun nativeCloseDistributedSession(handle: Long)

    /**
     * Decodes an encoded image buffer (AVIF path; used when the platform
     * decoder is unavailable) into raw pixels. [outMeta] receives
     * `[width, height, channels]`; returns the pixel bytes or null.
     */
    external fun nativeDecodeImage(encodedImage: ByteArray, outMeta: LongArray): ByteArray?

    external fun nativeGetScaleFactor(enginePtr: Long): Int
    external fun nativeRelease(enginePtr: Long)

    /**
     * Diagnostic: runs one whole-image inference and returns a per-blob
     * value-range report, marking the first layer with non-finite output.
     */
    external fun nativeDebugBlobScan(
        enginePtr: Long,
        inputImage: ByteArray,
        width: Int,
        height: Int,
        channels: Int
    ): String?

    /**
     * Diagnostic: redirects the process's stderr to [path] so ncnn's
     * NCNN_LOGE output (stderr, invisible in logcat) can be pulled.
     */
    external fun nativeRedirectStderr(path: String): Boolean

    /**
     * Scans encoded image bytes for this tool's own SR XMP marker.
     * Returns "model|scale|ts" for a marked input, null otherwise
     * (including unreadable input — callers treat that as "process").
     */
    external fun nativeProbeSrMarker(encodedImage: ByteArray): String?
}

/**
 * Compile-time native build capabilities, probed once from
 * [NCNNEngine.nativeCapabilities] (comma-separated tokens built into
 * libupix-native.so; see the avif-encoder/avif-decoder options in the root
 * xmake.lua). `false` in the default lean build:
 *
 *  - [avifEncode]  on-device AVIF output (libavif+aom, +~4.9 MB arm64 .so)
 *  - [avifDecode]  native AVIF input decode (libavif+dav1d, +~0.9 MB), the
 *    fallback below the platform decoder's API 31 floor
 */
object NativeCapabilities {
    val avifEncode: Boolean by lazy { probe("avif-encode") }
    val avifDecode: Boolean by lazy { probe("avif-decode") }

    private fun probe(capability: String): Boolean = runCatching {
        NCNNEngine.nativeCapabilities()
            .split(',')
            .any { it.trim().equals(capability, ignoreCase = true) }
    }.getOrDefault(false)
}

/**
 * AVIF capabilities of the currently paired PC, refreshed from the native
 * layer after each pairing handshake ([NCNNEngine.nativePeerCapabilities]).
 * The native side clears the flags when a new pairing attempt starts, so a
 * stale PC's support never outlives its connection. Consumers:
 *
 *  - [CoordinatorClient] input preflight (AVIF input needs PC avif-decode)
 *  - output-format gating (AVIF output needs PC avif-encode)
 */
object PeerCapabilities {
    @Volatile
    var avifEncode: Boolean = false
        private set

    @Volatile
    var avifDecode: Boolean = false
        private set

    /** Re-read the comma-token string the last handshake published. */
    fun refresh() {
        val caps = runCatching { NCNNEngine.nativePeerCapabilities() }.getOrDefault("")
        avifEncode = caps.split(',').any { it.trim().equals("avif-encode", true) }
        avifDecode = caps.split(',').any { it.trim().equals("avif-decode", true) }
    }

    /** Forget everything (disconnect / switching endpoints). */
    fun reset() {
        avifEncode = false
        avifDecode = false
    }
}
