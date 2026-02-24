package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.DeviceProfile
import com.crossscale.data.model.DeviceStatus

@Composable
fun DeviceCard(
    device: DeviceProfile,
    onConnect: (String) -> Unit,
    onDisconnect: (String) -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = device.deviceName,
                style = MaterialTheme.typography.titleMedium
            )
            Text(
                text = "地址: ${device.ipAddress}:${device.port}",
                style = MaterialTheme.typography.bodyMedium
            )
            Text(
                text = "类型: ${device.type}  权重: ${"%.2f".format(device.weight)}",
                style = MaterialTheme.typography.bodyMedium
            )
            Text(
                text = "状态: ${device.status}",
                style = MaterialTheme.typography.labelLarge
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
        }
    }
}
