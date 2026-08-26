package com.upix.ui.components

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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import com.upix.R
import com.upix.ui.theme.Spacing

@Composable
fun ConnectionDialog(
    show: Boolean,
    onDismiss: () -> Unit,
    onConnect: (String, String) -> Unit
) {
    if (!show) {
        return
    }

    // remember 位置保持在早退之后：与重构前 dialog 相同的生命周期——隐藏即离开组合，字段值每次打开都重置；组合内 remember、非 saveable，属有意为之。
    val fields = rememberHostPortFields()

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.connection_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                OutlinedTextField(
                    value = fields.host,
                    onValueChange = fields::onHostChange,
                    singleLine = true,
                    isError = fields.hostBlank,
                    label = { Text(stringResource(R.string.connection_host_label)) }
                )
                OutlinedTextField(
                    value = fields.port,
                    onValueChange = fields::onPortChange,
                    singleLine = true,
                    isError = fields.portInvalid,
                    supportingText = if (fields.portInvalid) {
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
                    val connected = fields.validate()
                    if (connected != null) {
                        onConnect(connected.first, connected.second)
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
