package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CheckCircle
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.LinkOff
import androidx.compose.material.icons.outlined.QrCodeScanner
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.ui.components.ConnectionDialog
import com.crossscale.ui.components.CrossScaleTopAppBar
import com.crossscale.ui.components.FingerprintConfirmDialog
import com.crossscale.ui.scanner.QRScannerScreen
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.MaxContentWidth
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono
import com.crossscale.viewmodel.DeviceUiState

/**
 * 连接屏（严格 1:1 手机↔PC）：单一状态卡承载全部状态——状态徽标（唯一强调色点）+
 * 状态标题 + 等宽字体地址/指纹 + 主操作。未连接时主按钮给扫码（首选路径），
 * 手动输入为安静的全宽文字按钮；错误条独立于状态卡之上，安静可忽略。
 */
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
        Scaffold(
            topBar = {
                CrossScaleTopAppBar(title = stringResource(R.string.connection_title))
            },
            containerColor = MaterialTheme.colorScheme.background,
            // 顶层 app shell 已统一消费系统栏 insets，这里不再叠加。
            contentWindowInsets = WindowInsets(0, 0, 0, 0)
        ) { innerPadding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding),
                contentAlignment = Alignment.TopCenter
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .widthIn(max = MaxContentWidth)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = LocalScreenMargin.current)
                        .padding(top = Spacing.md, bottom = Spacing.xxl),
                    verticalArrangement = Arrangement.spacedBy(Spacing.lg)
                ) {
                    state.connectionError?.let { err ->
                        ConnectionErrorBanner(
                            message = err,
                            onDismiss = onClearConnectionError
                        )
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

/** 错误条：errorContainer 卡 + 图标 + 可忽略；不抢状态卡的主视觉。 */
@Composable
private fun ConnectionErrorBanner(
    message: String,
    onDismiss: () -> Unit
) {
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
            Icon(
                imageVector = Icons.Outlined.ErrorOutline,
                contentDescription = null,
                modifier = Modifier.size(18.dp)
            )
            Text(
                text = message,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.weight(1f)
            )
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.connection_ignore))
            }
        }
    }
}

/** 状态卡通用骨架：居中一列，徽标 → 标题 → 详情 → 操作。 */
@Composable
private fun StatusCard(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(Spacing.xxl),
            verticalArrangement = Arrangement.spacedBy(Spacing.md),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            content()
        }
    }
}

/** 圆形 tonal 状态徽标：连接态用 primaryContainer（页面唯一强调），其余用中性容器。 */
@Composable
private fun StatusBadge(
    container: Color,
    contentColor: Color,
    icon: ImageVector
) {
    Surface(
        shape = CircleShape,
        color = container,
        contentColor = contentColor,
        modifier = Modifier.size(56.dp)
    ) {
        Box(contentAlignment = Alignment.Center) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                modifier = Modifier.size(26.dp)
            )
        }
    }
}

/** 地址与指纹统一走等宽字体，地址一行截断、指纹可长按选中肉眼核对。 */
@Composable
private fun EndpointDetails(endpoint: RemoteEndpoint) {
    Text(
        text = "${endpoint.host}:${endpoint.port}",
        style = ValueMono,
        color = MaterialTheme.colorScheme.onSurface,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        textAlign = TextAlign.Center
    )
    endpoint.fingerprint?.takeIf { fp -> fp.isNotBlank() }?.let { fp ->
        SelectionContainer {
            Text(
                text = stringResource(R.string.connection_fingerprint, fp),
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = ValueMono.fontFamily),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center
            )
        }
    }
}

@Composable
private fun ConnectedStatusCard(
    endpoint: RemoteEndpoint?,
    onDisconnect: () -> Unit
) {
    StatusCard {
        StatusBadge(
            container = MaterialTheme.colorScheme.primaryContainer,
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
            icon = Icons.Outlined.CheckCircle
        )
        Text(
            text = stringResource(R.string.connection_status_connected),
            style = MaterialTheme.typography.titleMedium
        )
        if (endpoint != null) {
            EndpointDetails(endpoint)
        }
        // 断开是低风险可逆操作：§9.3 安静文字动作即可，不占用强调容器。
        TextButton(
            onClick = onDisconnect,
            modifier = Modifier.padding(top = Spacing.xs)
        ) {
            Text(stringResource(R.string.connection_disconnect))
        }
    }
}

@Composable
private fun ConnectingCard() {
    StatusCard {
        CircularProgressIndicator(
            modifier = Modifier.size(28.dp),
            strokeWidth = 3.dp
        )
        Text(
            text = stringResource(R.string.connection_connecting_title),
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun DisconnectedActions(
    onScanQr: () -> Unit,
    onManual: () -> Unit
) {
    StatusCard {
        StatusBadge(
            container = MaterialTheme.colorScheme.surfaceContainerHighest,
            contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
            icon = Icons.Outlined.LinkOff
        )
        Text(
            text = stringResource(R.string.connection_status_disconnected),
            style = MaterialTheme.typography.titleMedium
        )
        // 首选路径（扫码）为实心主按钮，手动输入退居全宽安静文字按钮（§9.3）；纵向等宽更从容。
        Button(
            onClick = onScanQr,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = Spacing.sm)
        ) {
            Icon(
                imageVector = Icons.Outlined.QrCodeScanner,
                contentDescription = null,
                modifier = Modifier.size(18.dp)
            )
            Spacer(modifier = Modifier.width(Spacing.xs))
            Text(stringResource(R.string.connection_scan_qr))
        }
        TextButton(onClick = onManual, modifier = Modifier.fillMaxWidth()) {
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
