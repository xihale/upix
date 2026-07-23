package com.crossscale.native

class NativeWorker : AutoCloseable {
    init {
        System.loadLibrary("crossscale-native")
    }

    private var workerPtr: Long = nativeCreate()

    fun connect(host: String, port: Int, authKey: String, modelsDirectory: String): Boolean {
        if (workerPtr == 0L) {
            return false
        }
        return nativeConnect(workerPtr, host, port, authKey, modelsDirectory)
    }

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
        authKey: String,
        modelsDirectory: String
    ): Boolean
    private external fun nativeDisconnect(workerPtr: Long)
    private external fun nativeIsConnected(workerPtr: Long): Boolean
    private external fun nativeGetLastError(workerPtr: Long): String
    private external fun nativeRelease(workerPtr: Long)
}
