package io.upix.ui.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp

import java.util.Locale

/**
 * 组件包共享的小型样式模式：行内忙碌指示器、字节可读化。
 * 收口在此避免各组件内联复制同一份 dp/sp/字重参数。
 */

/** 行内小号忙碌指示器（20dp，2dp 描边）：模型卡上传/下载、远程列表刷新等共用。 */
@Composable
fun BusyIndicator(modifier: Modifier = Modifier) {
    CircularProgressIndicator(
        modifier = modifier.size(20.dp),
        strokeWidth = 2.dp
    )
}

/** 字节数转可读大小（B/KB/MB），模型卡片与远程下载列表共用。 */
internal fun formatSizeBytes(bytes: Long): String = when {
    bytes >= 1024 * 1024 -> String.format(Locale.ROOT, "%.1fMB", bytes / 1024f / 1024f)
    bytes >= 1024 -> String.format(Locale.ROOT, "%.0fKB", bytes / 1024f)
    else -> "${bytes}B"
}

/** Sheet 内单选项：M3 官方 ListItem + RadioButton 单选列表模式。 */
@Composable
internal fun SelectSheetItem(
    label: String,
    sublabel: String? = null,
    selected: Boolean,
    labelStyle: TextStyle? = null,
    onClick: () -> Unit
) {
    ListItem(
        supportingContent = if (sublabel != null) {
            {
                Text(
                    text = sublabel,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        } else {
            null
        },
        trailingContent = { RadioButton(selected = selected, onClick = null) },
        modifier = Modifier
            .fillMaxWidth()
            // clickable 自带 mergeDescendants：行内文字与 RadioButton 的选中态
            // 合并为一个可聚焦项；role 让 TalkBack 以单选项语义播报。
            .clickable(role = Role.RadioButton, onClick = onClick)
    ) {
        Text(text = label, style = labelStyle ?: MaterialTheme.typography.bodyLarge)
    }
}
