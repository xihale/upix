package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.ui.components.ConnectionDialog
import com.crossscale.ui.components.DeviceCard
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
    var qrPayload by remember { mutableStateOf("") }
    var showQrScanner by remember { mutableStateOf(false) }

    if (showQrScanner) {
        QRScannerScreen(
            onPayloadScanned = { payload ->
                onConnectByQr(payload)
                showQrScanner = false
            },
            onClose = { showQrScanner = false }
        )
    } else {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = "设备",
                style = MaterialTheme.typography.headlineMedium
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onStartDiscovery) {
                    Text(if (state.isDiscovering) "发现中..." else "开始发现")
                }
                OutlinedButton(onClick = onStopDiscovery) {
                    Text("停止发现")
                }
                OutlinedButton(onClick = { onShowManualConnectDialog(true) }) {
                    Text("手动连接")
                }
            }
            OutlinedButton(
                onClick = { showQrScanner = true },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("扫码连接")
            }

            OutlinedTextField(
                value = qrPayload,
                onValueChange = { qrPayload = it },
                modifier = Modifier.fillMaxWidth(),
                label = { Text("连接串 / 二维码内容") },
                singleLine = true
            )
            OutlinedButton(
                onClick = { onConnectByQr(qrPayload) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("解析并连接")
            }

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
