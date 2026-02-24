package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.ui.components.ConnectionDialog
import com.crossscale.ui.components.DeviceCard
import com.crossscale.ui.components.LayeredSectionCard
import com.crossscale.ui.scanner.QRScannerScreen
import com.crossscale.viewmodel.DeviceUiState

@Composable
fun DevicesScreen(
    state: DeviceUiState,
    onStartDiscovery: () -> Unit,
    onStopDiscovery: () -> Unit,
    onConnectDevice: (String) -> Unit,
    onDisconnectDevice: (String) -> Unit,
    onShowManualConnectDialog: (Boolean) -> Unit,
    onManualConnect: (String, String, String) -> Unit,
    onConnectByQr: (String) -> Unit
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
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            LayeredSectionCard(
                title = "连接控制",
                summary = if (state.isDiscovering) "当前正在发现设备" else "当前未发现设备",
                guide = null,
                expanded = false,
                onToggleExpanded = {},
                showDetailsToggle = false,
                actions = {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = onStartDiscovery, modifier = Modifier.fillMaxWidth()) {
                            Text(if (state.isDiscovering) "发现中..." else "开始发现")
                        }
                        OutlinedButton(onClick = onStopDiscovery, modifier = Modifier.fillMaxWidth()) {
                            Text("停止发现")
                        }
                        OutlinedButton(
                            onClick = { showQrScanner.value = true },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("扫码连接")
                        }
                        OutlinedButton(
                            onClick = { onShowManualConnectDialog(true) },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("手动连接")
                        }
                    }
                },
                details = {}
            )

            Text(
                text = "可用设备（${state.devices.size}）",
                style = MaterialTheme.typography.titleMedium
            )

            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                items(items = state.devices, key = { it.deviceId }) { device ->
                    DeviceCard(
                        device = device,
                        onConnect = onConnectDevice,
                        onDisconnect = onDisconnectDevice
                    )
                }
            }
        }
    }

    ConnectionDialog(
        show = state.showManualConnectDialog,
        onDismiss = { onShowManualConnectDialog(false) },
        onConnect = onManualConnect
    )
}
