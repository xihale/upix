package com.crossscale.viewmodel

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.Stable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.crossscale.AppContainer
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.data.repository.AppSettingsRepository
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.PairingRepository
import com.crossscale.data.repository.TrustedPC
import com.crossscale.network.CoordinatorClient
import com.crossscale.network.JoinInviteParser
import com.crossscale.util.StringProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.security.MessageDigest

@Immutable
data class PendingFingerprintConfirm(
    val host: String,
    val port: Int,
    val learnedPubKeyB64: String,
    val fingerprint: String
)

/**
 * Connection-screen UI state. The multi-device list and mDNS discovery toggle
 * are gone — mode/endpoint now come from [DeviceViewModel.connectionState] /
 * [DeviceViewModel.connectedEndpoint]. What remains here are the manual-connect
 * dialog flags, the error banner text and the TOFU fingerprint-confirm payload.
 */
@Immutable
data class DeviceUiState(
    val showManualConnectDialog: Boolean = false,
    val connectionError: String? = null,
    val pendingFingerprintConfirm: PendingFingerprintConfirm? = null
)

// Observable state lives in StateFlow properties; the class itself never
// mutates through public API, so it is safe to mark @Stable.
@Stable
class DeviceViewModel(
    private val deviceRepository: DeviceRepository,
    private val pairingRepository: PairingRepository,
    private val coordinatorClient: CoordinatorClient,
    private val appSettingsRepository: AppSettingsRepository,
    private val logRepository: LogRepository,
    private val stringProvider: StringProvider
) : ViewModel() {

    // ---- CONTRACT: observable connection state (consumed by UI via collectAsState) ----

    val connectionState: StateFlow<ConnectionState> = deviceRepository.connectionState

    val connectedEndpoint: StateFlow<RemoteEndpoint?> = deviceRepository.connectedEndpoint

    // ---- CONTRACT: connection actions ----

    /**
     * App-start auto-reconnect. Read the single last-paired PC and attempt a
     * strict-MITM reconnect. On success -> CONNECTED (Remote mode). On failure
     * -> DISCONNECTED (Native mode, surfaced via connectionError). No-op when
     * no PC has ever been paired.
     */
    fun tryAutoReconnect() {
        viewModelScope.launch {
            // EncryptedSharedPreferences 首次构建 + 读取都在 IO 线程，不碰主线程。
            val pc = withContext(Dispatchers.IO) { pairingRepository.getLastPairedPC() }
            if (pc == null) {
                logRepository.info("DeviceViewModel", "No last paired PC, staying disconnected")
                return@launch
            }
            val wantsEncrypted = isStreamEncryptionEnabled()
            deviceRepository.setConnecting()
            val learned = performConnection(pc.host, pc.port, pc.identityPubKey, wantsEncrypted)
            if (learned != null) {
                establishTrustedConnection(pc.host, pc.port, learned)
                logRepository.info(
                    "DeviceViewModel",
                    "Auto-reconnected to last PC ${pc.host}:${pc.port}"
                )
            } else {
                onConnectionFailure(
                    pc.host, pc.port,
                    stringProvider.getString(R.string.connection_error_auto_reconnect_failed)
                )
            }
        }
    }

    /**
     * Canonical connect entry. [peerPubKeyB64] non-null/non-blank => strict
     * MITM reconnect with a known key (trusted/QR), auto-persists + CONNECTED.
     * Blank/null => TOFU first contact: handshake succeeds, then the fingerprint
     * is surfaced for user confirmation; state stays CONNECTING until confirm.
     */
    fun requestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String?,
        wantsEncryptedStream: Boolean = isStreamEncryptionEnabled()
    ) {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port <= 0) {
            _connectionError.value = stringProvider.getString(R.string.connection_error_invalid_params)
            return
        }
        val knownKey = peerPubKeyB64?.trim().orEmpty()
        deviceRepository.setConnecting()
        viewModelScope.launch {
            val learned = performConnection(normalizedHost, port, knownKey, wantsEncryptedStream)
            if (learned == null) {
                onConnectionFailure(
                    normalizedHost, port,
                    stringProvider.getString(R.string.connection_error_connect_failed)
                )
                return@launch
            }
            if (knownKey.isBlank()) {
                surfaceFingerprintConfirm(normalizedHost, port, learned)
            } else {
                establishTrustedConnection(normalizedHost, port, learned)
            }
        }
    }

    fun disconnect() {
        val endpoint = connectedEndpoint.value
        coordinatorClient.disconnect()
        deviceRepository.setDisconnected()
        if (endpoint != null) {
            logRepository.info("DeviceViewModel", "Disconnected from ${endpoint.host}:${endpoint.port}")
        }
    }

    // ---- UI-facing connect helpers (QR / manual) ----

    fun connectByQrPayload(rawPayload: String) {
        val invite = JoinInviteParser.parse(rawPayload)
        if (invite == null) {
            _connectionError.value = stringProvider.getString(R.string.connection_error_invalid_qr)
            logRepository.error("DeviceViewModel", "Invalid/legacy QR payload")
            return
        }
        // QR is an authenticated OOB channel: if it carries the PC pubkey, do a
        // strict connect + persist directly (no fingerprint dialog). If the QR
        // is the bare host:port form, fall through to TOFU + confirm.
        requestConnection(
            host = invite.host,
            port = invite.port,
            peerPubKeyB64 = invite.identityPubKey,
            wantsEncryptedStream = isStreamEncryptionEnabled()
        )
    }

    fun connectManual(host: String, portText: String) {
        val port = portText.trim().toIntOrNull()
        if (host.isBlank() || port == null) {
            logRepository.error("DeviceViewModel", "Invalid manual address: $host:$portText")
            _connectionError.value = stringProvider.getString(R.string.connection_error_invalid_address)
            return
        }
        val normalizedHost = host.trim()
        viewModelScope.launch {
            // 信任表读取（EncryptedSharedPreferences）在 IO 线程执行。
            val trusted = withContext(Dispatchers.IO) {
                pairingRepository.getTrustedPC(normalizedHost, port)
            }
            requestConnection(
                host = normalizedHost,
                port = port,
                peerPubKeyB64 = trusted?.identityPubKey,
                wantsEncryptedStream = isStreamEncryptionEnabled()
            )
        }
    }

    /**
     * User confirmed the TOFU fingerprint. Persist trust + remember as the last
     * paired PC + flip to CONNECTED. The handshake already completed during the
     * TOFU requestConnection; this only commits persistence and state.
     */
    fun confirmFingerprintAndConnect() {
        val pending = _pendingFingerprintConfirm.value
        if (pending == null) {
            logRepository.warning("DeviceViewModel", "confirmFingerprint called with no pending request")
            return
        }
        _pendingFingerprintConfirm.value = null
        viewModelScope.launch {
            establishTrustedConnection(
                host = pending.host,
                port = pending.port,
                learnedPubKeyB64 = pending.learnedPubKeyB64,
                fingerprint = pending.fingerprint
            )
            logRepository.info(
                "DeviceViewModel",
                "Fingerprint confirmed, trusted PC saved: ${pending.host}:${pending.port} (fp=${pending.fingerprint})"
            )
        }
    }

    /**
     * User cancelled the fingerprint confirm. Drop the (post-handshake) link and
     * do not persist trust.
     */
    fun cancelFingerprintConfirm() {
        val pending = _pendingFingerprintConfirm.value ?: return
        _pendingFingerprintConfirm.value = null
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                coordinatorClient.disconnectIfMatches(pending.host, pending.port)
            }
            deviceRepository.setDisconnected()
            logRepository.info("DeviceViewModel", "Fingerprint confirm cancelled, dropped ${pending.host}:${pending.port}")
        }
    }

    fun showManualConnectDialog(show: Boolean) {
        _showManualConnectDialog.value = show
    }

    fun clearConnectionError() {
        _connectionError.value = null
    }

    // ---- dialog / error state (kept for the connection UI) ----

    private val _showManualConnectDialog = MutableStateFlow(false)
    private val _connectionError = MutableStateFlow<String?>(null)
    private val _pendingFingerprintConfirm = MutableStateFlow<PendingFingerprintConfirm?>(null)

    val uiState: StateFlow<DeviceUiState> = combine(
        _showManualConnectDialog,
        _connectionError,
        _pendingFingerprintConfirm
    ) { show, error, pending ->
        DeviceUiState(
            showManualConnectDialog = show,
            connectionError = error,
            pendingFingerprintConfirm = pending
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000L),
        initialValue = DeviceUiState()
    )

    // ---- internals ----

    init {
        // 真实掉线（如 PC 处理中途失联触发 clearEndpoint）要把 connectionState
        // 翻成 DISCONNECTED，否则 UI 会永远停留在“已连接”。
        viewModelScope.launch {
            var wasConnected = coordinatorClient.connected.value
            coordinatorClient.connected.collect { isConnected ->
                if (wasConnected && !isConnected &&
                    deviceRepository.connectionState.value == ConnectionState.CONNECTED
                ) {
                    deviceRepository.setDisconnected()
                }
                wasConnected = isConnected
            }
        }
    }

    private fun isStreamEncryptionEnabled(): Boolean {
        return appSettingsRepository.isStreamEncrypted()
    }

    /**
     * 指纹 = SHA256(pubkey) 前 8 字节的大写 hex (16 字符), 与 C++ 侧
     * crossscale::fingerprint() (identity.cpp:126) 完全一致, 供用户跨设备肉眼核对。
     */
    private fun computeFingerprint(pubKeyB64: String): String {
        return try {
            val raw = android.util.Base64.decode(pubKeyB64, android.util.Base64.DEFAULT)
            val digest = MessageDigest.getInstance("SHA-256").digest(raw)
            buildString {
                for (i in 0 until 8) {
                    append(String.format("%02X", digest[i]))
                }
            }
        } catch (e: Exception) {
            logRepository.error("DeviceViewModel", "computeFingerprint failed: ${e.message}")
            "????????????????"
        }
    }

    /** Pure handshake round-trip. Returns learned PC pubkey (base64) or null. */
    private suspend fun performConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): String? {
        return withContext(Dispatchers.IO) {
            coordinatorClient.requestConnection(host, port, peerPubKeyB64, wantsEncryptedStream)
        }
    }

    /** Strict/known-key success: persist trust + last paired PC, flip to CONNECTED. */
    private suspend fun establishTrustedConnection(
        host: String,
        port: Int,
        learnedPubKeyB64: String,
        fingerprint: String = computeFingerprint(learnedPubKeyB64)
    ) {
        // 信任表写入（EncryptedSharedPreferences）在 IO 线程执行。
        withContext(Dispatchers.IO) {
            pairingRepository.saveTrustedPC(host, port, TrustedPC(learnedPubKeyB64, fingerprint))
            pairingRepository.saveLastPairedPC(host, port, learnedPubKeyB64, fingerprint)
        }
        deviceRepository.setConnected(RemoteEndpoint(host = host, port = port, fingerprint = fingerprint))
    }

    /** TOFU first-contact success: surface fingerprint for user confirmation. */
    private fun surfaceFingerprintConfirm(host: String, port: Int, learnedPubKeyB64: String) {
        val fingerprint = computeFingerprint(learnedPubKeyB64)
        _pendingFingerprintConfirm.value = PendingFingerprintConfirm(
            host = host,
            port = port,
            learnedPubKeyB64 = learnedPubKeyB64,
            fingerprint = fingerprint
        )
        logRepository.info(
            "DeviceViewModel",
            "TOFU: learned PC identity at $host:$port, fp=$fingerprint — awaiting user confirm"
        )
    }

    private fun onConnectionFailure(host: String, port: Int, message: String) {
        deviceRepository.setDisconnected()
        _connectionError.value = message
        logRepository.warning("DeviceViewModel", "Connection to $host:$port failed: $message")
    }

    override fun onCleared() {
        super.onCleared()
        coordinatorClient.disconnect()
    }

    companion object {
        fun factory(container: AppContainer): ViewModelProvider.Factory = viewModelFactory {
            initializer {
                DeviceViewModel(
                    deviceRepository = container.deviceRepository,
                    pairingRepository = container.pairingRepository,
                    coordinatorClient = container.coordinatorClient,
                    appSettingsRepository = container.appSettingsRepository,
                    logRepository = container.logRepository,
                    stringProvider = container.stringProvider
                )
            }
        }
    }
}
