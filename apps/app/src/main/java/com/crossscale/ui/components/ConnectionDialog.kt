package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R

@Composable
fun ConnectionDialog(
    show: Boolean,
    onDismiss: () -> Unit,
    onConnect: (String, String) -> Unit
) {
    if (!show) {
        return
    }

    var host by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("9527") }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.connection_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = host,
                    onValueChange = { host = it },
                    singleLine = true,
                    label = { Text(stringResource(R.string.connection_host_label)) }
                )
                OutlinedTextField(
                    value = port,
                    onValueChange = { port = it },
                    singleLine = true,
                    label = { Text(stringResource(R.string.connection_port_label)) }
                )
                Text(
                    stringResource(R.string.connection_manual_hint),
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
                Text(stringResource(R.string.connection_title))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.common_cancel))
            }
        }
    )
}
