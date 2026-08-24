package com.crossscale.ui.components

import androidx.compose.foundation.layout.size
import androidx.compose.material3.CircularProgressIndicator

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
