package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceStatus
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun DeviceCard(
    device: DeviceProfile,
    onConnect: (String) -> Unit,
    onDisconnect: (String) -> Unit
) {
    val tpsLabel = if (device.tilesPerSecond > 0.0f) {
        "${"%.2f".format(device.tilesPerSecond)} tiles/s"
    } else {
        "未知"
    }
    val encryptionIcon = if (device.streamEncrypted) "🔒" else "🔓"

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = BorderlessTokens.ListItemVerticalPadding),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)
    ) {
        Text(
            text = "${device.deviceName} $encryptionIcon",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = "地址: ${device.ipAddress}:${device.port}",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        if (!device.discoveredFingerprint.isNullOrBlank()) {
            Text(
                text = "指纹: ${device.discoveredFingerprint}",
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace
            )
        }
        Text(
            text = "角色: ${device.role} · 类型: ${device.type} · 吞吐: $tpsLabel",
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = "状态: ${device.status}",
            style = MaterialTheme.typography.labelLarge
        )
        Row(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
            when (device.status) {
                DeviceStatus.CONNECTED -> {
                    OutlinedButton(onClick = { onDisconnect(device.deviceId) }) { Text("断开") }
                }
                DeviceStatus.CONNECTING -> {
                    Button(onClick = {}, enabled = false) { Text("连接中…") }
                }
                else -> {
                    Button(onClick = { onConnect(device.deviceId) }) {
                        Text(if (!device.discoveredPubKey.isNullOrBlank()) "一键连接" else "连接")
                    }
                }
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
    }
}
