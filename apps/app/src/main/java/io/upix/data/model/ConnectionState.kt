package io.upix.data.model

import androidx.compose.runtime.Immutable

/**
 * 1:1 phone<->PC connection state. The phone connects to at most one PC.
 *
 * - [DISCONNECTED]: no PC paired/connected; image processing runs locally.
 * - [CONNECTING]: a pairing/handshake attempt is in progress.
 * - [CONNECTED]: a PC is paired and reachable; image processing is routed to it.
 */
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED
}

/**
 * The PC the phone is currently talking to. [fingerprint] is the user-verifiable
 * SHA256(pubkey) short fingerprint, shown in the UI for visual cross-check.
 */
@Immutable
data class RemoteEndpoint(
    val host: String,
    val port: Int,
    val fingerprint: String?
)
