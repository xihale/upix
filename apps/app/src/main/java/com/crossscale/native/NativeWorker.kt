package com.crossscale.native

class NativeWorker : AutoCloseable {
    init {
        System.loadLibrary("crossscale-native")
    }

    private var workerPtr: Long = nativeCreate()

    fun connect(host: String, port: Int, peerPubKeyB64: String, modelsDirectory: String): Boolean {
        if (workerPtr == 0L) {
            return false
        }
        return nativeConnect(workerPtr, host, port, peerPubKeyB64, modelsDirectory)
    }

    /**
     * 新连接流程: ECDH 握手 + CONNECT_REQUEST/RESPONSE 配对(不传输图像)。
     * PC 侧对可信 peer 自动批准, 首次 peer 弹 [y/n] 提示。
     *
     * peerPubKeyB64 为空时进入 TOFU 模式: 握手期间学到 PC 身份公钥, 返回其 base64。
     * 非空时严格 MITM 校验, 成功返回传入公钥。失败返回 null。
     * 调用方应将返回的公钥持久化, 以便后续连接/图像处理恢复严格校验。
     */
    fun requestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): String? {
        return nativeRequestConnection(host, port, peerPubKeyB64, wantsEncryptedStream)
    }

    fun loadIdentity(identityPath: String): Boolean = nativeLoadIdentity(identityPath)

    fun disconnect() {
        if (workerPtr == 0L) {
            return
        }
        nativeDisconnect(workerPtr)
    }

    fun isConnected(): Boolean {
        if (workerPtr == 0L) {
            return false
        }
        return nativeIsConnected(workerPtr)
    }

    fun lastError(): String {
        if (workerPtr == 0L) {
            return ""
        }
        return nativeGetLastError(workerPtr)
    }

    fun release() {
        if (workerPtr == 0L) {
            return
        }
        nativeRelease(workerPtr)
        workerPtr = 0L
    }

    override fun close() {
        release()
    }

    private external fun nativeCreate(): Long
    private external fun nativeConnect(
        workerPtr: Long,
        host: String,
        port: Int,
        peerPubKeyB64: String,
        modelsDirectory: String
    ): Boolean
    private external fun nativeRequestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): String?
    private external fun nativeLoadIdentity(identityPath: String): Boolean
    private external fun nativeDisconnect(workerPtr: Long)
    private external fun nativeIsConnected(workerPtr: Long): Boolean
    private external fun nativeGetLastError(workerPtr: Long): String
    private external fun nativeRelease(workerPtr: Long)
}
