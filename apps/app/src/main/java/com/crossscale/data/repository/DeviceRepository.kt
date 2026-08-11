package com.crossscale.data.repository

import android.content.Context
import android.os.Build
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceStatus
import com.crossscale.data.model.DeviceType
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update

class DeviceRepository(
    context: Context,
    private val logRepository: LogRepository
) {
    private val localDeviceId = "local-${Build.MODEL}-${Build.ID}"
    private val prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val manualWeightOverrides = mutableMapOf<String, Float>()
    private var localRecommendedWeight: Float = DEFAULT_ANDROID_WEIGHT

    private val _devices = MutableStateFlow<List<DeviceProfile>>(emptyList())
    val devices: StateFlow<List<DeviceProfile>> = _devices.asStateFlow()
    val connectedDevices: Flow<List<DeviceProfile>> = devices.map { list ->
        list.filter { device -> device.status == DeviceStatus.CONNECTED }
    }

    init {
        restoreWeightPreferences()
        registerLocalDevice()
    }

    fun updateDiscoveredDevices(discovered: List<DeviceProfile>) {
        _devices.update { current ->
            val existingLocal = current.firstOrNull { it.deviceId == localDeviceId }
            val local = existingLocal?.copy(
                weight = manualWeightOverrides[localDeviceId] ?: localRecommendedWeight,
                recommendedTileSize = if (existingLocal.recommendedTileSize > 0) {
                    existingLocal.recommendedTileSize
                } else {
                    DEFAULT_ANDROID_RECOMMENDED_TILE_SIZE
                },
                maxBatchSize = if (existingLocal.maxBatchSize > 0) {
                    existingLocal.maxBatchSize
                } else {
                    DEFAULT_ANDROID_QUEUE_DEPTH
                }
            ) ?: createLocalDevice()
            val remoteCurrent = current.filter { it.deviceId != localDeviceId }
            val remoteById = remoteCurrent.associateBy { it.deviceId }.toMutableMap()

            for (device in discovered) {
                if (device.deviceId == localDeviceId) {
                    continue
                }
                val existing = remoteById[device.deviceId]
                val status = when (existing?.status) {
                    DeviceStatus.CONNECTED -> DeviceStatus.CONNECTED
                    DeviceStatus.CONNECTING -> DeviceStatus.CONNECTING
                    else -> DeviceStatus.DISCOVERED
                }
                val inferredWeight = inferRecommendedWeight(device)
                val resolvedWeight = manualWeightOverrides[device.deviceId]
                    ?: existing?.weight?.takeIf { it > 0.0f }
                    ?: device.weight.takeIf { it > 0.0f }
                    ?: inferredWeight
                val resolvedQueueDepth = existing?.maxBatchSize?.takeIf { it > 0 }
                    ?: device.maxBatchSize.takeIf { it > 0 }
                    ?: inferRecommendedQueueDepth(device)
                val resolvedTileSize = existing?.recommendedTileSize?.takeIf { it > 0 }
                    ?: device.recommendedTileSize.takeIf { it > 0 }
                    ?: inferRecommendedTileSize(device)
                remoteById[device.deviceId] = device.copy(
                    status = status,
                    type = existing?.type ?: device.type,
                    tilesPerSecond = existing?.tilesPerSecond ?: device.tilesPerSecond,
                    recommendedTileSize = resolvedTileSize,
                    maxBatchSize = resolvedQueueDepth,
                    weight = resolvedWeight,
                    discoveredFingerprint = device.discoveredFingerprint ?: existing?.discoveredFingerprint,
                    discoveredPubKey = device.discoveredPubKey ?: existing?.discoveredPubKey,
                    lastSeenEpochMillis = System.currentTimeMillis()
                )
            }

            listOf(local) + remoteById.values.sortedBy { it.deviceName }
        }
        logRepository.info("DeviceRepository", "Discovery updated, total devices=${_devices.value.size}")
    }

    fun connectDevice(deviceId: String) {
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == deviceId) {
                    device.copy(status = DeviceStatus.CONNECTED, lastSeenEpochMillis = System.currentTimeMillis())
                } else {
                    device
                }
            }
        }
        logRepository.info("DeviceRepository", "Device connected: $deviceId")
    }

    fun markConnecting(deviceId: String) {
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == deviceId) {
                    device.copy(status = DeviceStatus.CONNECTING, lastSeenEpochMillis = System.currentTimeMillis())
                } else {
                    device
                }
            }
        }
    }

    fun markFailed(deviceId: String) {
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == deviceId) {
                    device.copy(status = DeviceStatus.DISCOVERED, lastSeenEpochMillis = System.currentTimeMillis())
                } else {
                    device
                }
            }
        }
    }

    fun addManualDevice(host: String, port: Int, peerPubKeyB64: String, fingerprint: String? = null) {
        val deviceId = "$host:$port"
        val templateDevice = DeviceProfile(
            deviceId = deviceId,
            deviceName = "Remote@$host",
            type = DeviceType.GPU,
            tilesPerSecond = 0.0f,
            recommendedTileSize = DEFAULT_PC_DEDICATED_TILE_SIZE,
            maxBatchSize = DEFAULT_PC_DEDICATED_QUEUE_DEPTH,
            weight = DEFAULT_PC_DEDICATED_WEIGHT,
            ipAddress = host,
            port = port,
            status = DeviceStatus.CONNECTED,
            discoveredFingerprint = fingerprint,
            discoveredPubKey = peerPubKeyB64
        )
        val resolvedWeight = manualWeightOverrides[deviceId] ?: inferRecommendedWeight(templateDevice)
        val resolvedQueueDepth = inferRecommendedQueueDepth(templateDevice)
        val resolvedTileSize = inferRecommendedTileSize(templateDevice)
        var remoteBecameConnected = false
        _devices.update { current ->
            val existing = current.firstOrNull { it.deviceId == deviceId }
            remoteBecameConnected = existing == null || existing.status != DeviceStatus.CONNECTED
            if (existing != null) {
                current.map { device ->
                    if (device.deviceId == deviceId) {
                        device.copy(
                            status = DeviceStatus.CONNECTED,
                            ipAddress = host,
                            port = port,
                            weight = manualWeightOverrides[deviceId] ?: existing.weight,
                            maxBatchSize = if (existing.maxBatchSize > 0) existing.maxBatchSize else resolvedQueueDepth,
                            recommendedTileSize = if (existing.recommendedTileSize > 0) {
                                existing.recommendedTileSize
                            } else {
                                resolvedTileSize
                            },
                            discoveredFingerprint = fingerprint ?: existing.discoveredFingerprint,
                            discoveredPubKey = peerPubKeyB64.ifBlank { existing.discoveredPubKey ?: "" },
                            lastSeenEpochMillis = System.currentTimeMillis()
                        )
                    } else {
                        device
                    }
                }
            } else {
                current + DeviceProfile(
                    deviceId = deviceId,
                    deviceName = "Remote@$host",
                    type = DeviceType.GPU,
                    tilesPerSecond = 0.0f,
                    recommendedTileSize = resolvedTileSize,
                    maxBatchSize = resolvedQueueDepth,
                    weight = resolvedWeight,
                    ipAddress = host,
                    port = port,
                    status = DeviceStatus.CONNECTED,
                    discoveredFingerprint = fingerprint,
                    discoveredPubKey = peerPubKeyB64
                )
            }
        }
        if (remoteBecameConnected) {
            setLocalConnectionEnabled(enabled = false)
            logRepository.info(
                "DeviceRepository",
                "Remote device connected, local client disconnected by default"
            )
        }
        logRepository.info("DeviceRepository", "Manual device connected ($host:$port)")
    }

    fun disconnectDevice(deviceId: String) {
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == deviceId) {
                    device.copy(status = DeviceStatus.OFFLINE)
                } else {
                    device
                }
            }
        }
        logRepository.warning("DeviceRepository", "Device disconnected: $deviceId")
    }

    fun recordLocalThroughputSample(tilesPerSecond: Float) {
        recordThroughputSample(localDeviceId, tilesPerSecond)
    }

    fun recordThroughputSample(deviceId: String, tilesPerSecond: Float) {
        val normalizedSample = tilesPerSecond.coerceAtLeast(0.0f)
        if (normalizedSample <= 0.0f) {
            return
        }
        val manualWeight = manualWeightOverrides[deviceId]
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId != deviceId) {
                    device
                } else {
                    val smoothedTilesPerSecond = if (device.tilesPerSecond <= 0.0f) {
                        normalizedSample
                    } else {
                        (device.tilesPerSecond * 0.7f) + (normalizedSample * 0.3f)
                    }
                    device.copy(
                        tilesPerSecond = smoothedTilesPerSecond,
                        weight = manualWeight ?: smoothedTilesPerSecond,
                        lastSeenEpochMillis = System.currentTimeMillis()
                    )
                }
            }
        }
    }

    fun setDeviceWeight(deviceId: String, weight: Float) {
        val normalizedWeight = clampWeight(weight)
        if (deviceId.isBlank()) {
            return
        }

        manualWeightOverrides[deviceId] = normalizedWeight
        persistWeightOverride(deviceId, normalizedWeight)
        if (deviceId == localDeviceId) {
            localRecommendedWeight = normalizedWeight
            persistLocalRecommendedWeight(localRecommendedWeight)
        }

        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == deviceId) {
                    device.copy(
                        weight = normalizedWeight,
                        lastSeenEpochMillis = System.currentTimeMillis()
                    )
                } else {
                    device
                }
            }
        }
        logRepository.info(
            "DeviceRepository",
            "Manual weight updated: $deviceId -> ${"%.2f".format(normalizedWeight)}"
        )
    }

    fun isLocalDevice(deviceId: String): Boolean {
        return deviceId == localDeviceId
    }

    fun isLocalDeviceConnected(): Boolean {
        return _devices.value.any { device ->
            device.deviceId == localDeviceId && device.status == DeviceStatus.CONNECTED
        }
    }

    private fun setLocalConnectionEnabled(enabled: Boolean) {
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == localDeviceId) {
                    val targetStatus = if (enabled) DeviceStatus.CONNECTED else DeviceStatus.OFFLINE
                    if (device.status == targetStatus) {
                        device
                    } else {
                        device.copy(
                            status = targetStatus,
                            lastSeenEpochMillis = System.currentTimeMillis()
                        )
                    }
                } else {
                    device
                }
            }
        }
    }

    private fun registerLocalDevice() {
        _devices.value = listOf(createLocalDevice())
        logRepository.info("DeviceRepository", "Local device registered: ${Build.MODEL}")
    }

    private fun createLocalDevice(): DeviceProfile {
        return DeviceProfile(
            deviceId = localDeviceId,
            deviceName = "${Build.MANUFACTURER} ${Build.MODEL}",
            type = DeviceType.GPU,
            tilesPerSecond = 0.0f,
            recommendedTileSize = DEFAULT_ANDROID_RECOMMENDED_TILE_SIZE,
            maxBatchSize = DEFAULT_ANDROID_QUEUE_DEPTH,
            weight = localRecommendedWeight,
            ipAddress = "127.0.0.1",
            port = 9527,
            status = DeviceStatus.CONNECTED
        )
    }

    private fun inferRecommendedWeight(device: DeviceProfile): Float {
        val fingerprint = "${device.deviceName} ${device.deviceId}".lowercase()
        return when {
            fingerprint.contains("android") -> DEFAULT_ANDROID_WEIGHT
            fingerprint.contains("intel") ||
                fingerprint.contains("integrated") ||
                fingerprint.contains("uhd") ||
                device.type == DeviceType.CPU -> DEFAULT_PC_INTEGRATED_WEIGHT

            else -> DEFAULT_PC_DEDICATED_WEIGHT
        }
    }

    private fun inferRecommendedQueueDepth(device: DeviceProfile): Int {
        val fingerprint = "${device.deviceName} ${device.deviceId}".lowercase()
        return when {
            fingerprint.contains("android") -> DEFAULT_ANDROID_QUEUE_DEPTH
            fingerprint.contains("intel") ||
                fingerprint.contains("integrated") ||
                fingerprint.contains("uhd") ||
                device.type == DeviceType.CPU -> DEFAULT_PC_INTEGRATED_QUEUE_DEPTH

            else -> DEFAULT_PC_DEDICATED_QUEUE_DEPTH
        }
    }

    private fun inferRecommendedTileSize(device: DeviceProfile): Int {
        val fingerprint = "${device.deviceName} ${device.deviceId}".lowercase()
        return when {
            fingerprint.contains("android") -> DEFAULT_ANDROID_RECOMMENDED_TILE_SIZE
            fingerprint.contains("intel") ||
                fingerprint.contains("integrated") ||
                fingerprint.contains("uhd") ||
                device.type == DeviceType.CPU -> DEFAULT_PC_INTEGRATED_TILE_SIZE

            else -> DEFAULT_PC_DEDICATED_TILE_SIZE
        }
    }

    private fun clampWeight(weight: Float): Float {
        return weight.coerceIn(MIN_WEIGHT, MAX_WEIGHT)
    }

    private fun restoreWeightPreferences() {
        localRecommendedWeight = clampWeight(
            prefs.getFloat(PREF_LOCAL_WEIGHT, DEFAULT_ANDROID_WEIGHT)
        )

        manualWeightOverrides.clear()
        prefs.all.forEach { entry ->
            if (!entry.key.startsWith(PREF_DEVICE_WEIGHT_PREFIX)) {
                return@forEach
            }
            val deviceId = entry.key.removePrefix(PREF_DEVICE_WEIGHT_PREFIX)
            if (deviceId.isBlank()) {
                return@forEach
            }
            val parsedWeight = when (val value = entry.value) {
                is Float -> value
                is Number -> value.toFloat()
                is String -> value.toFloatOrNull()
                else -> null
            } ?: return@forEach
            manualWeightOverrides[deviceId] = clampWeight(parsedWeight)
        }

        if (manualWeightOverrides[localDeviceId] == null) {
            manualWeightOverrides[localDeviceId] = localRecommendedWeight
        }
    }

    private fun persistWeightOverride(deviceId: String, weight: Float) {
        prefs.edit()
            .putFloat(PREF_DEVICE_WEIGHT_PREFIX + deviceId, clampWeight(weight))
            .apply()
    }

    private fun persistLocalRecommendedWeight(weight: Float) {
        prefs.edit().putFloat(PREF_LOCAL_WEIGHT, clampWeight(weight)).apply()
    }

    companion object {
        private const val PREFS_NAME = "crossscale_device_weights"
        private const val PREF_LOCAL_WEIGHT = "local_recommended_weight"
        private const val PREF_DEVICE_WEIGHT_PREFIX = "device_weight_"

        private const val MIN_WEIGHT = 0.5f
        private const val MAX_WEIGHT = 20.0f

        private const val DEFAULT_PC_DEDICATED_WEIGHT = 5.0f
        private const val DEFAULT_PC_INTEGRATED_WEIGHT = 4.0f
        private const val DEFAULT_ANDROID_WEIGHT = 2.0f

        private const val DEFAULT_PC_DEDICATED_QUEUE_DEPTH = 5
        private const val DEFAULT_PC_INTEGRATED_QUEUE_DEPTH = 3
        private const val DEFAULT_ANDROID_QUEUE_DEPTH = 2

        private const val DEFAULT_PC_DEDICATED_TILE_SIZE = 384
        private const val DEFAULT_PC_INTEGRATED_TILE_SIZE = 320
        private const val DEFAULT_ANDROID_RECOMMENDED_TILE_SIZE = 256
    }
}
