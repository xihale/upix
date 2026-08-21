package com.crossscale.ui.screens

import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.QrCodeScanner
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.ui.components.ConnectionDialog
import com.crossscale.ui.components.FingerprintConfirmDialog
import com.crossscale.ui.scanner.QRScannerScreen
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono
import com.crossscale.viewmodel.DeviceUiState

/**
 * 连接屏（严格 1:1 手机↔PC）：
 * 顶部大状态卡（状态标题 + 地址/指纹 + 操作按钮），下方使用说明分区卡。
 * 已连 → 标题 + 断开；连接中 → spinner；断开 → 扫码/手动入口。
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
                .padding(horizontal = Spacing.xl)
                .padding(top = Spacing.md, bottom = Spacing.xxl),
            verticalArrangement = Arrangement.spacedBy(Spacing.lg)
        ) {
            Text(
                text = stringResource(R.string.connection_title),
                style = MaterialTheme.typography.titleLarge
            )

            state.connectionError?.let { err ->
                Surface(
                    color = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    shape = MaterialTheme.shapes.medium,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(
                                start = Spacing.lg,
                                end = Spacing.sm,
                                top = Spacing.sm,
                                bottom = Spacing.sm
                            ),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
                    ) {
                        Text(
                            text = err,
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.weight(1f)
                        )
                        TextButton(onClick = onClearConnectionError) {
                            Text(stringResource(R.string.connection_ignore))
                        }
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
        }
    }

    ConnectionDialog(
        show = state.showManualConnectDialog,
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

/** 状态主卡通用骨架：标题 + 副信息 + 底部操作区；状态差异只用文字与必要 spinner 表达。 */
@Composable
private fun StatusCardScaffold(
    title: String,
    subtitle: String? = null,
    progressInstead: Boolean = false,
    actions: @Composable () -> Unit
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier.padding(Spacing.xl),
            verticalArrangement = Arrangement.spacedBy(Spacing.md),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            if (progressInstead) {
                CircularProgressIndicator(
                    modifier = Modifier.size(24.dp),
                    strokeWidth = 2.5.dp
                )
            }
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            actions()
        }
    }
}

@Composable
private fun ConnectedStatusCard(
    endpoint: RemoteEndpoint?,
    onDisconnect: () -> Unit
) {
    StatusCardScaffold(
        title = stringResource(R.string.connection_status_connected),
        subtitle = endpoint?.let { "${it.host}:${it.port}" }
    ) {
        Column(
            verticalArrangement = Arrangement.spacedBy(Spacing.md),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            endpoint?.fingerprint?.takeIf { fp -> fp.isNotBlank() }?.let { fp ->
                Text(
                    text = stringResource(R.string.connection_fingerprint, fp),
                    style = ValueMono,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1
                )
            }
            FilledTonalButton(onClick = onDisconnect) {
                Text(stringResource(R.string.connection_disconnect))
            }
        }
    }
}

@Composable
private fun ConnectingCard() {
    StatusCardScaffold(
        progressInstead = true,
        title = stringResource(R.string.connection_connecting_title)
    ) {
    }
}

@Composable
private fun DisconnectedActions(
    onScanQr: () -> Unit,
    onManual: () -> Unit
) {
    StatusCardScaffold(
        title = stringResource(R.string.connection_status_disconnected)
    ) {
        Row(
            horizontalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            FilledTonalButton(onClick = onScanQr) {
                Icon(
                    imageVector = Icons.Outlined.QrCodeScanner,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(Spacing.xs))
                Text(stringResource(R.string.connection_scan_qr))
            }
            OutlinedButton(onClick = onManual) {
                Icon(
                    imageVector = Icons.Outlined.Edit,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(Spacing.xs))
                Text(stringResource(R.string.connection_manual_input))
            }
        }
    }
}
