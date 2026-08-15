package com.crossscale.ui.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.ui.theme.Spacing

/**
 * Bottom sheet 内的动作项：主标签 + 可选副标签，点击涟漪保留。
 * （原 SettingsRow/SwitchRow 无调用点，已由 ConfigRow/ConfigSwitchRow 取代并删除。）
 */
@Composable
fun SheetActionItem(label: String, subtitle: String? = null, onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .heightIn(min = 48.dp)
            .padding(horizontal = Spacing.xl, vertical = Spacing.sm)
    ) {
        Text(text = label, style = MaterialTheme.typography.bodyLarge)
        subtitle?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}
