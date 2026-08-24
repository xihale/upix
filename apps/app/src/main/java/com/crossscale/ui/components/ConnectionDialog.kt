package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import com.crossscale.R
import com.crossscale.ui.theme.Spacing

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
    var hostBlank by remember { mutableStateOf(false) }
    var portInvalid by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.connection_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                OutlinedTextField(
                    value = host,
                    onValueChange = {
                        host = it
                        hostBlank = false
                    },
                    singleLine = true,
                    isError = hostBlank,
                    label = { Text(stringResource(R.string.connection_host_label)) }
                )
                OutlinedTextField(
                    value = port,
                    onValueChange = {
                        port = it
                        portInvalid = false
                    },
                    singleLine = true,
                    isError = portInvalid,
                    supportingText = if (portInvalid) {
                        { Text(stringResource(R.string.connection_port_invalid)) }
                    } else {
                        null
                    },
                    // 端口只允许数字输入。
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    label = { Text(stringResource(R.string.connection_port_label)) }
                )
                Text(
                    stringResource(R.string.connection_manual_hint),
                    style = MaterialTheme.typography.bodySmall
                )
            }
        },
        confirmButton = {
            Button(
                onClick = {
                    // 本地校验：host 非空、端口为 1..65535，不通过则就地报错、不关框。
                    val trimmedHost = host.trim()
                    val portNumber = port.trim().toIntOrNull()
                    val portValid = portNumber != null && portNumber in 1..65535
                    hostBlank = trimmedHost.isBlank()
                    portInvalid = !portValid
                    if (trimmedHost.isNotBlank() && portValid) {
                        onConnect(trimmedHost, port.trim())
                        onDismiss()
                    }
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
