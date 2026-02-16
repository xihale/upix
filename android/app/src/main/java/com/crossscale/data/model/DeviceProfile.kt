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
    val port: Int
)

enum class DeviceType {
    CPU,
    GPU
}
