package com.upix.nativelib

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
    external fun nativeProcessImageDistributed(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        inputFormat: String,
        inputImage: ByteArray,
        outputFormat: String,
        outputQuality: Int,
        encoderOptionsJson: String
    ): ByteArray?

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
