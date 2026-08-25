package com.upix.nativelib

object NCNNEngine {
    init {
        System.loadLibrary("upix-native")
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
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        inputFormat: String,
        inputImage: ByteArray,
        outputFormat: String,
        outputQuality: Int
    ): ByteArray?
    external fun nativeGetScaleFactor(enginePtr: Long): Int
    external fun nativeRelease(enginePtr: Long)

    /**
     * Scans encoded image bytes for this tool's own SR XMP marker.
     * Returns "model|scale|ts" for a marked input, null otherwise
     * (including unreadable input — callers treat that as "process").
     */
    external fun nativeProbeSrMarker(encodedImage: ByteArray): String?
}
