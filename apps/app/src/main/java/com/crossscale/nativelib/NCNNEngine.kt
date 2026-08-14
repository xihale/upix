package com.crossscale.nativelib

object NCNNEngine {
    init {
        System.loadLibrary("crossscale-native")
    }
    
    external fun nativeInit(deviceType: Int): Long
    external fun nativeLoadModel(enginePtr: Long, modelPath: String): Boolean
    external fun nativeGetLastError(): String?
    external fun nativeProcessImage(
        enginePtr: Long,
        inputImage: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        outputFormat: String,
        outputQuality: Int
    ): ByteArray?
    external fun nativeProcessImageDistributed(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        inputImage: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        outputFormat: String,
        outputQuality: Int
    ): ByteArray?
    external fun nativeBenchmark(enginePtr: Long): Float
    external fun nativeGetScaleFactor(enginePtr: Long): Int
    external fun nativeGetDevices(): Array<String>
    external fun nativeRelease(enginePtr: Long)
}
