package com.crossscale.data.model

data class DeviceProfile(
    val deviceId: String,
    val deviceName: String,
    val type: DeviceType,
    val tilesPerSecond: Float,
    val recommendedTileSize: Int,
    val maxBatchSize: Int,
    val role: DeviceRole = DeviceRole.MOBILE,
    val streamEncrypted: Boolean = false,
    val ipAddress: String,
    val port: Int,
    val status: DeviceStatus = DeviceStatus.DISCOVERED,
    val lastSeenEpochMillis: Long = System.currentTimeMillis(),
    val discoveredFingerprint: String? = null,   // from mDNS TXT id_fingerprint
    val discoveredPubKey: String? = null          // from mDNS TXT id_pubkey (base64)
)

enum class DeviceType {
    CPU,
    GPU
}

enum class DeviceRole {
    PC,
    MOBILE
}

enum class DeviceStatus {
    DISCOVERED,
    CONNECTING,   // pairing/connect in progress
    CONNECTED,
    OFFLINE
}
