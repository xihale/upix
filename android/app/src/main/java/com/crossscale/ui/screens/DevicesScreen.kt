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
    var qrPayload by remember { mutableStateOf("") }
    var showQrScanner by remember { mutableStateOf(false) }
    var actionDetailsExpanded by remember { mutableStateOf(false) }

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
                text = "设备协作",
                style = MaterialTheme.typography.headlineSmall
            )
            Text(
                text = "先自动发现，再用扫码/手动补充连接。保持列表简洁，详细连接说明在展开层。",
                style = MaterialTheme.typography.bodyMedium
            )

            LayeredSectionCard(
                title = "连接控制",
                summary = if (state.isDiscovering) "当前正在发现设备" else "当前未发现设备",
                guide = "推荐顺序：1) 开始发现 2) 选择设备连接 3) 若未发现则使用扫码或手动连接。",
                expanded = actionDetailsExpanded,
                onToggleExpanded = { actionDetailsExpanded = !actionDetailsExpanded },
                actions = {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = onStartDiscovery, modifier = Modifier.fillMaxWidth()) {
                            Text(if (state.isDiscovering) "发现中..." else "开始发现")
                        }
                        OutlinedButton(onClick = onStopDiscovery, modifier = Modifier.fillMaxWidth()) {
                            Text("停止发现")
                        }
                        OutlinedButton(
                            onClick = { showQrScanner = true },
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
                details = {
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
                }
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
