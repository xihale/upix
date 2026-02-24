package com.crossscale.data.model

data class DeviceProfile(
    val deviceId: String,
    val deviceName: String,
    val type: DeviceType,
    val tilesPerSecond: Float,
    val recommendedTileSize: Int,
    val maxBatchSize: Int,
    val weight: Float,
    val ipAddress: String,
    val port: Int,
    val status: DeviceStatus = DeviceStatus.DISCOVERED,
    val lastSeenEpochMillis: Long = System.currentTimeMillis()
)

enum class DeviceType {
    CPU,
    GPU
}

enum class DeviceStatus {
    DISCOVERED,
    CONNECTED,
    OFFLINE
}
