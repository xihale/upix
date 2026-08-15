package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import com.crossscale.R
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono

/**
 * TOFU 首次连接: 展示从握手学到的 PC 指纹(16 字符大写 hex), 让用户与 PC 终端
 * 显示的指纹肉眼核对。确认 → 调用方持久化信任; 取消 → 断开连接。
 */
@Composable
fun FingerprintConfirmDialog(
    fingerprint: String,
    host: String,
    port: Int,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.dialog_fingerprint_title)) },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(Spacing.md),
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    stringResource(R.string.dialog_fingerprint_body, host, port),
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center
                )
                Text(
                    text = fingerprint,
                    style = ValueMono,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth()
                )
                Text(
                    stringResource(R.string.dialog_fingerprint_trust_note),
                    style = MaterialTheme.typography.bodySmall,
                    textAlign = TextAlign.Center
                )
            }
        },
        confirmButton = {
            Button(onClick = onConfirm) { Text(stringResource(R.string.dialog_fingerprint_confirm)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) }
        }
    )
}
