package com.crossscale.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.crossscale.AppContainer
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceStatus
import com.crossscale.data.model.DeviceType
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.PairingRepository
import com.crossscale.network.CoordinatorClient
import com.crossscale.network.MDNSDiscovery
import com.crossscale.network.JoinInviteParser
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class DeviceUiState(
    val devices: List<DeviceProfile> = emptyList(),
    val isDiscovering: Boolean = false,
    val showManualConnectDialog: Boolean = false,
    val manualConnectHost: String = "",
    val manualConnectPort: String = "9527"
)

class DeviceViewModel(
    private val deviceRepository: DeviceRepository,
    private val pairingRepository: PairingRepository,
    private val coordinatorClient: CoordinatorClient,
    private val mdnsDiscovery: MDNSDiscovery,
    private val logRepository: LogRepository
) : ViewModel() {
    private val _isDiscovering = MutableStateFlow(false)
    private val _showManualConnectDialog = MutableStateFlow(false)
    private val _manualConnectHost = MutableStateFlow("")
    private val _manualConnectPort = MutableStateFlow("9527")
    private var discoveryJob: Job? = null
    private val autoConnectingTargets = mutableSetOf<String>()

    val uiState: StateFlow<DeviceUiState> = combine(
        deviceRepository.devices,
        _isDiscovering,
        _showManualConnectDialog,
        _manualConnectHost,
        _manualConnectPort
    ) { devices, isDiscovering, showDialog, manualConnectHost, manualConnectPort ->
        DeviceUiState(
            devices = devices,
            isDiscovering = isDiscovering,
            showManualConnectDialog = showDialog,
            manualConnectHost = manualConnectHost,
            manualConnectPort = manualConnectPort
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
                    attemptAutoConnect(discovered)
                },
                onError = { message ->
                    logRepository.error("DeviceViewModel", message)
                }
            )
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
            pairingRepository.savePairing(invite.host, invite.port, invite.authKey)
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
            val normalizedHost = host.trim()
            val normalizedAuthKey = authKey.trim()
            pairingRepository.savePairing(normalizedHost, port, normalizedAuthKey)
            deviceRepository.addManualDevice(normalizedHost, port, normalizedAuthKey)
        } else {
            logRepository.error("DeviceViewModel", "Coordinator connect failed: $host:$port")
        }
    }

    fun connectDevice(deviceId: String) {
        val target = uiState.value.devices.firstOrNull { it.deviceId == deviceId }
        if (target == null) {
            logRepository.error("DeviceViewModel", "Unknown device: $deviceId")
            return
        }
        _manualConnectHost.value = target.ipAddress
        _manualConnectPort.value = target.port.toString()
        _showManualConnectDialog.value = true
        logRepository.info(
            "DeviceViewModel",
            "Pairing required for discovered device ${target.ipAddress}:${target.port}; awaiting manual auth key"
        )
    }

    fun disconnectDevice(deviceId: String) {
        deviceRepository.disconnectDevice(deviceId)
    }

    fun showManualConnectDialog(show: Boolean) {
        _showManualConnectDialog.value = show
        if (!show) {
            _manualConnectHost.value = ""
            _manualConnectPort.value = "9527"
        }
    }

    override fun onCleared() {
        super.onCleared()
        mdnsDiscovery.stop()
        coordinatorClient.disconnect()
        coordinatorClient.release()
    }

    private fun attemptAutoConnect(device: DeviceProfile) {
        val deviceId = device.deviceId
        val alreadyConnected = uiState.value.devices.any {
            it.deviceId == deviceId && it.status == DeviceStatus.CONNECTED
        }
        if (alreadyConnected) {
            return
        }
        if (!autoConnectingTargets.add(deviceId)) {
            return
        }

        viewModelScope.launch {
            try {
                val authKey = pairingRepository.getAuthKey(device.ipAddress, device.port)
                if (authKey.isNullOrBlank()) {
                    return@launch
                }

                val connected = coordinatorClient.connect(device.ipAddress, device.port, authKey)
                if (connected) {
                    deviceRepository.addManualDevice(device.ipAddress, device.port, authKey)
                    logRepository.info(
                        "DeviceViewModel",
                        "Auto-connected paired PC ${device.ipAddress}:${device.port}"
                    )
                }
            } finally {
                autoConnectingTargets.remove(deviceId)
            }
        }
    }

    companion object {
        fun factory(container: AppContainer): ViewModelProvider.Factory {
            return object : ViewModelProvider.Factory {
                @Suppress("UNCHECKED_CAST")
                override fun <T : ViewModel> create(modelClass: Class<T>): T {
                    return DeviceViewModel(
                        deviceRepository = container.deviceRepository,
                        pairingRepository = container.pairingRepository,
                        coordinatorClient = container.coordinatorClient,
                        mdnsDiscovery = container.mdnsDiscovery,
                        logRepository = container.logRepository
                    ) as T
                }
            }
        }
    }
}
