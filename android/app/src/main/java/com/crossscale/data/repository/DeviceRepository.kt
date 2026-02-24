package com.crossscale.data.repository

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
    private val logRepository: LogRepository
) {
    private val localDeviceId = "local-${Build.MODEL}-${Build.ID}"

    private val _devices = MutableStateFlow<List<DeviceProfile>>(emptyList())
    val devices: StateFlow<List<DeviceProfile>> = _devices.asStateFlow()
    val connectedDevices: Flow<List<DeviceProfile>> = devices.map { list ->
        list.filter { device -> device.status == DeviceStatus.CONNECTED }
    }

    init {
        registerLocalDevice()
    }

    fun updateDiscoveredDevices(discovered: List<DeviceProfile>) {
        _devices.update { current ->
            val local = current.firstOrNull { it.deviceId == localDeviceId } ?: createLocalDevice()
            val remoteCurrent = current.filter { it.deviceId != localDeviceId }
            val remoteById = remoteCurrent.associateBy { it.deviceId }.toMutableMap()

            for (device in discovered) {
                if (device.deviceId == localDeviceId) {
                    continue
                }
                val existing = remoteById[device.deviceId]
                remoteById[device.deviceId] = if (existing != null && existing.status == DeviceStatus.CONNECTED) {
                    device.copy(status = DeviceStatus.CONNECTED)
                } else {
                    device.copy(status = DeviceStatus.DISCOVERED)
                }
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

    fun addManualDevice(host: String, port: Int, authKey: String) {
        val normalizedAuthKey = authKey.trim()
        if (normalizedAuthKey.isBlank()) {
            logRepository.warning("DeviceRepository", "Manual connection without auth key for $host:$port")
        }
        val deviceId = "$host:$port"
        _devices.update { current ->
            val existing = current.firstOrNull { it.deviceId == deviceId }
            if (existing != null) {
                current.map { device ->
                    if (device.deviceId == deviceId) {
                        device.copy(
                            status = DeviceStatus.CONNECTED,
                            ipAddress = host,
                            port = port,
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
                    recommendedTileSize = 256,
                    maxBatchSize = 1,
                    weight = 1.0f,
                    ipAddress = host,
                    port = port,
                    status = DeviceStatus.CONNECTED
                )
            }
        }
        logRepository.info("DeviceRepository", "Manual device connected ($host:$port)")
    }

    fun disconnectDevice(deviceId: String) {
        _devices.update { current ->
            current.map { device ->
                if (device.deviceId == deviceId && device.deviceId != localDeviceId) {
                    device.copy(status = DeviceStatus.OFFLINE)
                } else {
                    device
                }
            }
        }
        logRepository.warning("DeviceRepository", "Device disconnected: $deviceId")
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
            recommendedTileSize = 256,
            maxBatchSize = 1,
            weight = 1.0f,
            ipAddress = "127.0.0.1",
            port = 9527,
            status = DeviceStatus.CONNECTED
        )
    }
}
