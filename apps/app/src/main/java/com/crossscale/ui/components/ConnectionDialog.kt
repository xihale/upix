package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.unit.dp

@Composable
fun ConnectionDialog(
    show: Boolean,
    initialHost: String,
    initialPort: String,
    onDismiss: () -> Unit,
    onConnect: (String, String) -> Unit
) {
    if (!show) {
        return
    }

    var host by remember { mutableStateOf(initialHost) }
    var port by remember { mutableStateOf(initialPort) }

    LaunchedEffect(show, initialHost, initialPort) {
        if (show) {
            host = initialHost
            port = initialPort
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("手动连接") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = host,
                    onValueChange = { host = it },
                    singleLine = true,
                    label = { Text("PC IP 地址") }
                )
                OutlinedTextField(
                    value = port,
                    onValueChange = { port = it },
                    singleLine = true,
                    label = { Text("端口") }
                )
                Text(
                    "首次连接会在握手后展示 PC 指纹供你核对。",
                    style = androidx.compose.material3.MaterialTheme.typography.bodySmall
                )
            }
        },
        confirmButton = {
            Button(
                onClick = {
                    onConnect(host, port)
                    onDismiss()
                }
            ) {
                Text("连接")
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("取消")
            }
        }
    )
}
