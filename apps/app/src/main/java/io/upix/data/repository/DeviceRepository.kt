package io.upix.data.repository

import io.upix.data.model.ConnectionState
import io.upix.data.model.RemoteEndpoint
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Holds the single phone<->PC connection state.
 *
 * The multi-node device list, local-device-as-worker concepts and mDNS
 * discovery state are gone. This is now the reactive backbone for
 * [io.upix.viewmodel.DeviceViewModel] and the mode indicator.
 *
 * The authoritative "are we connected" boolean remains
 * [io.upix.network.CoordinatorClient.isConnected]; this repository only
 * mirrors that state reactively for UI observation.
 */
class DeviceRepository(
    private val logRepository: LogRepository
) {
    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _connectedEndpoint = MutableStateFlow<RemoteEndpoint?>(null)
    val connectedEndpoint: StateFlow<RemoteEndpoint?> = _connectedEndpoint.asStateFlow()

    fun setConnecting() {
        _connectionState.value = ConnectionState.CONNECTING
    }

    fun setConnected(endpoint: RemoteEndpoint) {
        _connectedEndpoint.value = endpoint
        _connectionState.value = ConnectionState.CONNECTED
        logRepository.info(
            "DeviceRepository",
            "Connected to PC ${endpoint.host}:${endpoint.port}"
        )
    }

    fun setDisconnected() {
        val previous = _connectedEndpoint.value
        _connectedEndpoint.value = null
        _connectionState.value = ConnectionState.DISCONNECTED
        if (previous != null) {
            logRepository.info(
                "DeviceRepository",
                "Disconnected from PC ${previous.host}:${previous.port}"
            )
        }
    }
}
