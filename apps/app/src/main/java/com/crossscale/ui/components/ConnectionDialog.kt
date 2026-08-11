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
    onConnect: (String, String, String) -> Unit
) {
    if (!show) {
        return
    }

    var host by remember { mutableStateOf(initialHost) }
    var port by remember { mutableStateOf(initialPort) }
    var peerPubKey by remember { mutableStateOf("") }

    LaunchedEffect(show, initialHost, initialPort) {
        if (show) {
            host = initialHost
            port = initialPort
            peerPubKey = ""
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
                    label = { Text("Host") }
                )
                OutlinedTextField(
                    value = port,
                    onValueChange = { port = it },
                    singleLine = true,
                    label = { Text("Port") }
                )
                OutlinedTextField(
                    value = peerPubKey,
                    onValueChange = { peerPubKey = it },
                    singleLine = true,
                    label = { Text("PC 公钥 (base64)") }
                )
            }
        },
        confirmButton = {
            Button(
                onClick = {
                    onConnect(host, port, peerPubKey)
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
