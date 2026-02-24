package com.crossscale.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.crossscale.AppContainer
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceType
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.LogRepository
import com.crossscale.network.CoordinatorClient
import com.crossscale.network.MDNSDiscovery
import com.crossscale.network.JoinInviteParser
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class DeviceUiState(
    val devices: List<DeviceProfile> = emptyList(),
    val isDiscovering: Boolean = false,
    val showManualConnectDialog: Boolean = false
)

class DeviceViewModel(
    private val deviceRepository: DeviceRepository,
    private val coordinatorClient: CoordinatorClient,
    private val mdnsDiscovery: MDNSDiscovery,
    private val logRepository: LogRepository
) : ViewModel() {
    private val _isDiscovering = MutableStateFlow(false)
    private val _showManualConnectDialog = MutableStateFlow(false)
    private var discoveryJob: Job? = null

    val uiState: StateFlow<DeviceUiState> = combine(
        deviceRepository.devices,
        _isDiscovering,
        _showManualConnectDialog
    ) { devices, isDiscovering, showDialog ->
        DeviceUiState(
            devices = devices,
            isDiscovering = isDiscovering,
            showManualConnectDialog = showDialog
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000L),
        initialValue = DeviceUiState()
    )

    fun startDiscovery() {
        if (_isDiscovering.value) {
            return
        }
        logRepository.info("DeviceViewModel", "Start mDNS discovery")
        _isDiscovering.value = true
        discoveryJob?.cancel()
        discoveryJob = viewModelScope.launch {
            mdnsDiscovery.start(
                onServiceFound = { service ->
                    val discovered = DeviceProfile(
                        deviceId = "${service.host}:${service.port}",
                        deviceName = service.name,
                        type = DeviceType.GPU,
                        tilesPerSecond = 0.0f,
                        recommendedTileSize = 256,
                        maxBatchSize = 1,
                        weight = 1.0f,
                        ipAddress = service.host,
                        port = service.port
                    )
                    deviceRepository.updateDiscoveredDevices(listOf(discovered))
                },
                onError = { message ->
                    logRepository.error("DeviceViewModel", message)
                }
            )

            // mDNS may need time before first callback, provide a fallback mock seed.
            delay(900L)
            if (uiState.value.devices.size <= 1) {
                deviceRepository.updateDiscoveredDevices(mockDiscoveredDevices())
            }
        }
    }

    fun stopDiscovery() {
        discoveryJob?.cancel()
        mdnsDiscovery.stop()
        _isDiscovering.value = false
        logRepository.info("DeviceViewModel", "Stop mDNS discovery")
    }

    fun connectByQrPayload(rawPayload: String) {
        val invite = JoinInviteParser.parse(rawPayload)
        if (invite == null) {
            logRepository.error("DeviceViewModel", "Invalid QR payload: $rawPayload")
            return
        }
        val connected = coordinatorClient.connect(invite.host, invite.port, invite.authKey)
        if (connected) {
            deviceRepository.addManualDevice(invite.host, invite.port, invite.authKey)
            logRepository.info("DeviceViewModel", "Connected from QR invite to ${invite.host}:${invite.port}")
        } else {
            logRepository.error("DeviceViewModel", "QR invite connect failed: ${invite.host}:${invite.port}")
        }
    }

    fun connectManual(host: String, portText: String, authKey: String) {
        val port = portText.toIntOrNull()
        if (host.isBlank() || port == null) {
            logRepository.error("DeviceViewModel", "Invalid manual address: $host:$portText")
            return
        }
        val connected = coordinatorClient.connect(host.trim(), port, authKey.trim())
        if (connected) {
            deviceRepository.addManualDevice(host.trim(), port, authKey.trim())
        } else {
            logRepository.error("DeviceViewModel", "Coordinator connect failed: $host:$port")
        }
    }

    fun connectDevice(deviceId: String) {
        deviceRepository.connectDevice(deviceId)
    }

    fun disconnectDevice(deviceId: String) {
        deviceRepository.disconnectDevice(deviceId)
    }

    fun showManualConnectDialog(show: Boolean) {
        _showManualConnectDialog.value = show
    }

    override fun onCleared() {
        super.onCleared()
        mdnsDiscovery.stop()
        coordinatorClient.disconnect()
        coordinatorClient.release()
    }

    private fun mockDiscoveredDevices(): List<DeviceProfile> {
        return listOf(
            DeviceProfile(
                deviceId = "pc-01",
                deviceName = "Desktop RTX",
                type = DeviceType.GPU,
                tilesPerSecond = 30.5f,
                recommendedTileSize = 512,
                maxBatchSize = 4,
                weight = 3.2f,
                ipAddress = "192.168.1.10",
                port = 9527
            ),
            DeviceProfile(
                deviceId = "laptop-02",
                deviceName = "Laptop CPU",
                type = DeviceType.CPU,
                tilesPerSecond = 6.8f,
                recommendedTileSize = 256,
                maxBatchSize = 1,
                weight = 0.9f,
                ipAddress = "192.168.1.21",
                port = 9527
            )
        )
    }

    companion object {
        fun factory(container: AppContainer): ViewModelProvider.Factory {
            return object : ViewModelProvider.Factory {
                @Suppress("UNCHECKED_CAST")
                override fun <T : ViewModel> create(modelClass: Class<T>): T {
                    return DeviceViewModel(
                        deviceRepository = container.deviceRepository,
                        coordinatorClient = container.coordinatorClient,
                        mdnsDiscovery = container.mdnsDiscovery,
                        logRepository = container.logRepository
                    ) as T
                }
            }
        }
    }
}
