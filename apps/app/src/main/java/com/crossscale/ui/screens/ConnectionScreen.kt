package com.crossscale.ui.screens

import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.crossscale.ui.components.ConnectionDialog
import com.crossscale.ui.components.FingerprintConfirmDialog
import com.crossscale.ui.scanner.QRScannerScreen
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.data.model.ConnectionState
import com.crossscale.viewmodel.DeviceUiState
import com.crossscale.data.model.RemoteEndpoint

/**
 * 连接屏（原多设备列表）：严格 1:1 手机↔PC。
 * 已连 → 状态卡（endpoint/指纹/断开）；连接中 → 提示；未连 → 扫码 / 手动输入。
 * 始终展示醒目说明卡，告诉用户远程 vs 本地的差异与首次配对流程。
 */
@ExperimentalGetImage
@Composable
fun ConnectionScreen(
    state: DeviceUiState,
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?,
    onDisconnect: () -> Unit,
    onShowManualConnectDialog: (Boolean) -> Unit,
    onManualConnect: (String, String) -> Unit,
    onConnectByQr: (String) -> Unit,
    onClearConnectionError: () -> Unit,
    onConfirmFingerprint: () -> Unit,
    onCancelFingerprint: () -> Unit
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
                .verticalScroll(rememberScrollState())
                .padding(
                    horizontal = BorderlessTokens.ScreenPaddingHorizontal,
                    vertical = BorderlessTokens.ScreenPaddingVertical
                ),
            verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionSpacing)
        ) {
            Text(
                text = "连接 PC",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold
            )

            state.connectionError?.let { err ->
                Surface(
                    color = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    shape = RoundedCornerShape(16.dp),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = err,
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.weight(1f)
                        )
                        TextButton(onClick = onClearConnectionError) { Text("忽略") }
                    }
                }
            }

            when (connectionState) {
                ConnectionState.CONNECTED -> ConnectedStatusCard(
                    endpoint = connectedEndpoint,
                    onDisconnect = onDisconnect
                )
                ConnectionState.CONNECTING -> ConnectingCard()
                ConnectionState.DISCONNECTED -> DisconnectedActions(
                    onScanQr = { showQrScanner.value = true },
                    onManual = { onShowManualConnectDialog(true) }
                )
            }

            HelpCard()

            Spacer(modifier = Modifier.height(24.dp))
        }
    }

    ConnectionDialog(
        show = state.showManualConnectDialog,
        initialHost = state.manualConnectHost,
        initialPort = state.manualConnectPort,
        onDismiss = { onShowManualConnectDialog(false) },
        onConnect = onManualConnect
    )

    state.pendingFingerprintConfirm?.let { pending ->
        FingerprintConfirmDialog(
            fingerprint = pending.fingerprint,
            host = pending.host,
            port = pending.port,
            onConfirm = onConfirmFingerprint,
            onDismiss = onCancelFingerprint
        )
    }
}

@Composable
private fun ConnectedStatusCard(
    endpoint: RemoteEndpoint?,
    onDisconnect: () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = Color(0xFFD7F4DE),
            contentColor = Color(0xFF1B5E20)
        )
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = "✅ 已连接 PC",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
            endpoint?.let {
                Text(
                    text = "${it.host}:${it.port}",
                    style = MaterialTheme.typography.bodyLarge,
                    fontFamily = FontFamily.Monospace
                )
                it.fingerprint?.takeIf { fp -> fp.isNotBlank() }?.let { fp ->
                    Text(
                        text = "指纹：$fp",
                        style = MaterialTheme.typography.labelMedium,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
            Spacer(modifier = Modifier.height(8.dp))
            OutlinedButton(
                onClick = onDisconnect,
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(14.dp)
            ) {
                Text("断开连接")
            }
        }
    }
}

@Composable
private fun ConnectingCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = Color(0xFFFFE8C2),
            contentColor = Color(0xFF8A5A00)
        )
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                CircularProgressIndicator(
                    modifier = Modifier.size(22.dp),
                    strokeWidth = 2.dp
                )
                Text(
                    text = "正在连接 PC…",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
            }
            Text(
                text = "首次连接需在 PC 端批准并核对指纹。",
                style = MaterialTheme.typography.bodySmall
            )
        }
    }
}

@Composable
private fun DisconnectedActions(
    onScanQr: () -> Unit,
    onManual: () -> Unit
) {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Button(
            onClick = onScanQr,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight),
            shape = RoundedCornerShape(16.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer
            )
        ) {
            Text(
                text = "📷 扫码连接",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
        }

        OutlinedButton(
            onClick = onManual,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight),
            shape = RoundedCornerShape(16.dp)
        ) {
            Text(
                text = "手动输入 IP:port",
                style = MaterialTheme.typography.titleMedium
            )
        }
    }
}

/**
 * 醒目说明卡：解释远程 vs 本地的差异、首次配对流程、自动重连。
 * 用户明确要求「一定要给好说明，明显一些」。
 */
@Composable
private fun HelpCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer,
            contentColor = MaterialTheme.colorScheme.onSecondaryContainer
        )
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Text(
                text = "ℹ️ 使用说明",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = "连接 PC 后，图片将发送到 PC 用 GPU 超分辨率处理（更快）；" +
                    "未连接时手机本地处理。" +
                    "首次连接需在 PC 端批准并核对指纹。" +
                    "下次启动 App 会自动重连上次使用的 PC。",
                style = MaterialTheme.typography.bodyMedium
            )
        }
    }
}

