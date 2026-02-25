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
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceStatus
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun DeviceCard(
    device: DeviceProfile,
    onConnect: (String) -> Unit,
    onDisconnect: (String) -> Unit,
    onAdjustWeight: (String, Float) -> Unit
) {
    val effectiveWeight = if (device.weight > 0.0f) {
        device.weight
    } else {
        1.0f
    }
    val weightLabel = if (device.weight > 0.0f) {
        "${"%.2f".format(device.weight)}"
    } else {
        "未知"
    }
    val tpsLabel = if (device.tilesPerSecond > 0.0f) {
        "${"%.2f".format(device.tilesPerSecond)} tiles/s"
    } else {
        "未知"
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = BorderlessTokens.ListItemVerticalPadding),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)
    ) {
        Text(
            text = device.deviceName,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = "地址: ${device.ipAddress}:${device.port}",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = "类型: ${device.type} · 权重: $weightLabel · 吞吐: $tpsLabel",
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = "状态: ${device.status}",
            style = MaterialTheme.typography.labelLarge
        )
        Row(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
            OutlinedButton(
                onClick = {
                    onAdjustWeight(
                        device.deviceId,
                        (effectiveWeight - 0.5f).coerceAtLeast(0.5f)
                    )
                }
            ) {
                Text("-权重")
            }
            OutlinedButton(
                onClick = {
                    onAdjustWeight(
                        device.deviceId,
                        (effectiveWeight + 0.5f).coerceAtMost(20.0f)
                    )
                }
            ) {
                Text("+权重")
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
            if (device.status == DeviceStatus.CONNECTED) {
                OutlinedButton(onClick = { onDisconnect(device.deviceId) }) {
                    Text("断开")
                }
            } else {
                Button(onClick = { onConnect(device.deviceId) }) {
                    Text("连接")
                }
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
    }
}
