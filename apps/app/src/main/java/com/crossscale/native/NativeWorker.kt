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
     * 新连接流程: 仅做 ECDH 握手 + CONNECT_REQUEST/RESPONSE 配对(不传输图像)。
     * PC 侧对可信 peer 自动批准, 首次 peer 弹 [y/n] 提示。返回 true 表示 PC 批准,
     * 之后图像提交复用已建立的信任。这是 "连接" 按钮走的主路径。
     */
    fun requestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): Boolean {
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
    ): Boolean
    private external fun nativeLoadIdentity(identityPath: String): Boolean
    private external fun nativeDisconnect(workerPtr: Long)
    private external fun nativeIsConnected(workerPtr: Long): Boolean
    private external fun nativeGetLastError(workerPtr: Long): String
    private external fun nativeRelease(workerPtr: Long)
}
