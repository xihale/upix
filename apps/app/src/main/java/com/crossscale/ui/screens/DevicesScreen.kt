package com.crossscale.ui.screens

import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import com.crossscale.ui.components.ConnectionDialog
import com.crossscale.ui.components.DeviceCard
import com.crossscale.ui.components.FingerprintConfirmDialog
import com.crossscale.ui.scanner.QRScannerScreen
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.viewmodel.DeviceUiState

@ExperimentalGetImage
@Composable
fun DevicesScreen(
    state: DeviceUiState,
    onStartDiscovery: () -> Unit,
    onStopDiscovery: () -> Unit,
    onConnectDevice: (String) -> Unit,
    onDisconnectDevice: (String) -> Unit,
    onAdjustDeviceWeight: (String, Float) -> Unit,
    onShowManualConnectDialog: (Boolean) -> Unit,
    onManualConnect: (String, String, String) -> Unit,
    onConnectByQr: (String) -> Unit,
    onConfirmTrust: (String) -> Unit,
    onCancelFingerprintConfirm: () -> Unit,
    onClearConnectionError: () -> Unit
) {
    val showQrScanner = remember { mutableStateOf(false) }

    if (showQrScanner.value) {
        QRScannerScreen(
            onPayloadScanned = { payload ->
                onConnectByQr(payload)
                showQrScanner.value = false
            },
            onClose = { showQrScanner.value = false }
        )
    } else {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(
                    horizontal = BorderlessTokens.ScreenPaddingHorizontal,
                    vertical = BorderlessTokens.ScreenPaddingVertical
                ),
            verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionSpacing)
        ) {
            state.connectionError?.let { err ->
                Row(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
                    Text(
                        text = err,
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.weight(1f)
                    )
                    TextButton(onClick = onClearConnectionError) { Text("忽略") }
                }
            }
            Text(
                text = "连接控制",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = BorderlessTokens.SectionTitleWeight
            )
            Text(
                text = if (state.isDiscovering) "当前正在发现设备" else "当前未发现设备",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            HorizontalDivider(
                color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha)
            )

            Button(
                onClick = onStartDiscovery,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            ) {
                Text(if (state.isDiscovering) "发现中..." else "开始发现")
            }
            OutlinedButton(
                onClick = onStopDiscovery,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            ) {
                Text("停止发现")
            }
            OutlinedButton(
                onClick = { showQrScanner.value = true },
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            ) {
                Text("扫码连接")
            }
            OutlinedButton(
                onClick = { onShowManualConnectDialog(true) },
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            ) {
                Text("手动连接")
            }

            HorizontalDivider(
                color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha)
            )
            Text(
                text = "可用设备（${state.devices.size}）",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = BorderlessTokens.SectionTitleWeight
            )

            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(BorderlessTokens.ListItemSpacing)
            ) {
                items(items = state.devices, key = { it.deviceId }) { device ->
                    DeviceCard(
                        device = device,
                        onConnect = onConnectDevice,
                        onDisconnect = onDisconnectDevice,
                        onAdjustWeight = onAdjustDeviceWeight
                    )
                }
            }
        }
    }

    ConnectionDialog(
        show = state.showManualConnectDialog,
        initialHost = state.manualConnectHost,
        initialPort = state.manualConnectPort,
        onDismiss = { onShowManualConnectDialog(false) },
        onConnect = onManualConnect
    )

    state.pendingFingerprintConfirm?.let { device ->
        FingerprintConfirmDialog(
            deviceName = device.deviceName,
            fingerprint = device.discoveredFingerprint ?: "未知",
            onConfirm = { onConfirmTrust(device.deviceId) },
            onDismiss = onCancelFingerprintConfirm
        )
    }
}
