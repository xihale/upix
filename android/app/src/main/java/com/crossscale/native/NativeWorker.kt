package com.crossscale.native

class NativeWorker : AutoCloseable {
    private var workerPtr: Long = nativeCreate()

    fun connect(host: String, port: Int, authKey: String): Boolean {
        if (workerPtr == 0L) {
            return false
        }
        return nativeConnect(workerPtr, host, port, authKey)
    }

    fun disconnect() {
        if (workerPtr == 0L) {
            return
        }
        nativeDisconnect(workerPtr)
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
    private external fun nativeConnect(workerPtr: Long, host: String, port: Int, authKey: String): Boolean
    private external fun nativeDisconnect(workerPtr: Long)
    private external fun nativeRelease(workerPtr: Long)
}
