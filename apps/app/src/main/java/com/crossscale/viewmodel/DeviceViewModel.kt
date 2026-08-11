package com.crossscale.viewmodel

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.crossscale.AppContainer
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceRole
import com.crossscale.data.model.DeviceStatus
import com.crossscale.data.model.DeviceType
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.PairingRepository
import com.crossscale.data.repository.TrustedPC
import com.crossscale.network.CoordinatorClient
import com.crossscale.network.JoinInviteParser
import com.crossscale.network.MDNSDiscovery
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class DeviceUiState(
    val devices: List<DeviceProfile> = emptyList(),
    val isDiscovering: Boolean = false,
    val showManualConnectDialog: Boolean = false,
    val manualConnectHost: String = "",
    val manualConnectPort: String = "9527",
    val connectionError: String? = null
)

class DeviceViewModel(
    private val deviceRepository: DeviceRepository,
    private val pairingRepository: PairingRepository,
    private val coordinatorClient: CoordinatorClient,
    private val mdnsDiscovery: MDNSDiscovery,
    private val logRepository: LogRepository,
    private val appContext: Context
) : ViewModel() {
    private val _isDiscovering = MutableStateFlow(false)
    private val _showManualConnectDialog = MutableStateFlow(false)
    private val _manualConnectHost = MutableStateFlow("")
    private val _manualConnectPort = MutableStateFlow("9527")
    private val _connectionError = MutableStateFlow<String?>(null)
    private var discoveryJob: Job? = null
    private val autoConnectingTargets = mutableSetOf<String>()

    val uiState: StateFlow<DeviceUiState> = combine(
        deviceRepository.devices,
        _isDiscovering,
        _showManualConnectDialog,
        _manualConnectHost,
        _manualConnectPort,
        _connectionError
    ) { values ->
        @Suppress("UNCHECKED_CAST")
        DeviceUiState(
            devices = values[0] as List<DeviceProfile>,
            isDiscovering = values[1] as Boolean,
            showManualConnectDialog = values[2] as Boolean,
            manualConnectHost = values[3] as String,
            manualConnectPort = values[4] as String,
            connectionError = values[5] as String?
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
                        role = DeviceRole.PC,
                        ipAddress = service.host,
                        port = service.port,
                        discoveredFingerprint = service.fingerprint,
                        discoveredPubKey = service.identityPubKey
                    )
                    deviceRepository.updateDiscoveredDevices(listOf(discovered))
                    attemptAutoConnect(discovered)
                },
                onServiceLost = { service ->
                    logRepository.warning(
                        "DeviceViewModel",
                        "mDNS service lost: ${service.host}:${service.port}"
                    )
                    coordinatorClient.disconnectIfMatches(service.host, service.port)
                    deviceRepository.disconnectDevice("${service.host}:${service.port}")
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
            _connectionError.value = "无效或过期的二维码（请确认 PC 已升级到加密版本）"
            logRepository.error("DeviceViewModel", "Invalid/legacy QR payload")
            return
        }
        if (invite.identityPubKey.isBlank()) {
            _connectionError.value = "二维码缺少 PC 公钥"
            return
        }
        viewModelScope.launch {
            val wantsEncrypted = isStreamEncryptionEnabled()
            deviceRepository.markConnecting("${invite.host}:${invite.port}")
            val connected = connectToCoordinator(invite.host, invite.port, invite.identityPubKey, wantsEncrypted)
            if (connected) {
                pairingRepository.saveTrustedPC(
                    invite.host, invite.port,
                    TrustedPC(invite.identityPubKey, invite.fingerprint ?: "")
                )
                deviceRepository.addManualDevice(
                    invite.host, invite.port, invite.identityPubKey, invite.fingerprint, wantsEncrypted
                )
                logRepository.info("DeviceViewModel", "Connected from v2 QR to ${invite.host}:${invite.port}")
            } else {
                deviceRepository.markFailed("${invite.host}:${invite.port}")
                _connectionError.value = "扫码连接失败"
            }
        }
    }

    fun connectManual(host: String, portText: String, peerPubKeyB64: String) {
        val port = portText.toIntOrNull()
        if (host.isBlank() || port == null) {
            logRepository.error("DeviceViewModel", "Invalid manual address: $host:$portText")
            return
        }
        val normalizedHost = host.trim()
        val normalizedPeerPubKey = peerPubKeyB64.trim()
        val wantsEncrypted = isStreamEncryptionEnabled()
        viewModelScope.launch {
            deviceRepository.markConnecting("$normalizedHost:$port")
            val connected = connectToCoordinator(normalizedHost, port, normalizedPeerPubKey, wantsEncrypted)
            if (connected) {
                pairingRepository.saveTrustedPC(
                    normalizedHost, port,
                    TrustedPC(normalizedPeerPubKey, "")
                )
                deviceRepository.addManualDevice(
                    normalizedHost, port, normalizedPeerPubKey, null, wantsEncrypted
                )
            } else {
                deviceRepository.markFailed("$normalizedHost:$port")
                _connectionError.value = "连接失败，请检查 PC 是否在线"
                logRepository.error("DeviceViewModel", "Coordinator connect failed: $normalizedHost:$port")
            }
        }
    }

    fun connectDevice(deviceId: String) {
        val target = uiState.value.devices.firstOrNull { it.deviceId == deviceId } ?: run {
            logRepository.error("DeviceViewModel", "Unknown device: $deviceId")
            return
        }
        if (deviceRepository.isLocalDevice(deviceId)) {
            deviceRepository.connectDevice(deviceId)
            return
        }
        if (coordinatorClient.isConnectedTo(target.ipAddress, target.port)) {
            deviceRepository.connectDevice(deviceId)
            return
        }
        val trusted = pairingRepository.getTrustedPC(target.ipAddress, target.port)
        val peerPubKey = when {
            !target.discoveredPubKey.isNullOrBlank() -> target.discoveredPubKey
            trusted != null -> trusted.identityPubKey
            else -> null
        }
        if (peerPubKey.isNullOrBlank()) {
            // No pubkey available — fall back to manual entry.
            _manualConnectHost.value = target.ipAddress
            _manualConnectPort.value = target.port.toString()
            _showManualConnectDialog.value = true
            return
        }
        val wantsEncrypted = isStreamEncryptionEnabled()
        viewModelScope.launch {
            deviceRepository.markConnecting(deviceId)
            // CONNECT_REQUEST round-trip: PC shows approval prompt for first-time
            // peers, auto-approves trusted ones. Phone shows "等待 PC 批准..." while
            // CONNECTING. On denial we surface an error instead of asking the user.
            val approved = connectToCoordinator(target.ipAddress, target.port, peerPubKey, wantsEncrypted)
            if (approved) {
                // Persist trust so future connects skip the PC prompt.
                if (trusted == null ||
                    (target.discoveredPubKey.isNullOrBlank() &&
                        trusted.identityPubKey != peerPubKey)) {
                    pairingRepository.saveTrustedPC(
                        target.ipAddress, target.port,
                        TrustedPC(peerPubKey, target.discoveredFingerprint ?: "")
                    )
                }
                deviceRepository.addManualDevice(
                    target.ipAddress, target.port, peerPubKey,
                    target.discoveredFingerprint, wantsEncrypted
                )
                logRepository.info("DeviceViewModel", "PC approved + connected to ${target.ipAddress}:${target.port}")
            } else {
                deviceRepository.markFailed(deviceId)
                _connectionError.value = "PC 拒绝连接"
                logRepository.warning("DeviceViewModel", "PC denied connection from ${target.ipAddress}:${target.port}")
            }
        }
    }

    fun clearConnectionError() {
        _connectionError.value = null
    }

    fun disconnectDevice(deviceId: String) {
        val target = uiState.value.devices.firstOrNull { it.deviceId == deviceId }
        if (target == null) {
            logRepository.error("DeviceViewModel", "Unknown device for disconnect: $deviceId")
            return
        }
        if (deviceRepository.isLocalDevice(deviceId)) {
            deviceRepository.disconnectDevice(deviceId)
            logRepository.info("DeviceViewModel", "Local client disconnected")
            return
        }
        viewModelScope.launch {
            disconnectFromCoordinatorIfMatches(target.ipAddress, target.port)
            deviceRepository.disconnectDevice(deviceId)
        }
    }

    fun showManualConnectDialog(show: Boolean) {
        _showManualConnectDialog.value = show
        if (!show) {
            _manualConnectHost.value = ""
            _manualConnectPort.value = "9527"
        }
    }

    private fun isStreamEncryptionEnabled(): Boolean {
        return appContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(PREF_STREAM_ENCRYPTED, false)
    }

    override fun onCleared() {
        super.onCleared()
        mdnsDiscovery.stop()
        coordinatorClient.disconnect()
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
                val trusted = pairingRepository.getTrustedPC(device.ipAddress, device.port)
                if (trusted == null) return@launch
                // MITM guard: if mDNS advertised a pubkey, it must match the stored one.
                if (!device.discoveredPubKey.isNullOrBlank() &&
                    device.discoveredPubKey != trusted.identityPubKey) {
                    logRepository.warning(
                        "DeviceViewModel",
                        "Pubkey mismatch for ${device.ipAddress}:${device.port} — possible MITM, ignoring stored pairing"
                    )
                    _connectionError.value = "PC 身份已变更，请重新确认指纹"
                    return@launch
                }
                deviceRepository.markConnecting(deviceId)
                val connected = connectToCoordinator(
                    device.ipAddress, device.port, trusted.identityPubKey, isStreamEncryptionEnabled()
                )
                if (connected) {
                    deviceRepository.addManualDevice(
                        device.ipAddress, device.port, trusted.identityPubKey, trusted.fingerprint
                    )
                    logRepository.info("DeviceViewModel", "Auto-connected trusted PC ${device.ipAddress}:${device.port}")
                } else {
                    deviceRepository.markFailed(deviceId)
                }
            } finally {
                autoConnectingTargets.remove(deviceId)
            }
        }
    }

    private suspend fun connectToCoordinator(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean = false
    ): Boolean {
        return withContext(Dispatchers.IO) {
            // 新流程: handshake + CONNECT_REQUEST/RESPONSE pairing. PC-side
            // approval (auto for trusted peers, prompt for first-timers).
            coordinatorClient.requestConnection(host, port, peerPubKeyB64, wantsEncryptedStream)
        }
    }

    private suspend fun disconnectFromCoordinatorIfMatches(
        host: String,
        port: Int
    ) {
        withContext(Dispatchers.IO) {
            coordinatorClient.disconnectIfMatches(host, port)
        }
    }

    companion object {
        private const val PREFS_NAME = "crossscale_settings"
        private const val PREF_STREAM_ENCRYPTED = "stream_encrypted"

        fun factory(container: AppContainer): ViewModelProvider.Factory {
            return object : ViewModelProvider.Factory {
                @Suppress("UNCHECKED_CAST")
                override fun <T : ViewModel> create(modelClass: Class<T>): T {
                    return DeviceViewModel(
                        deviceRepository = container.deviceRepository,
                        pairingRepository = container.pairingRepository,
                        coordinatorClient = container.coordinatorClient,
                        mdnsDiscovery = container.mdnsDiscovery,
                        logRepository = container.logRepository,
                        appContext = container.appContext
                    ) as T
                }
            }
        }
    }
}
