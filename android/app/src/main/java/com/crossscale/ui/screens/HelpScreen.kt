package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.ui.components.LayeredSectionCard

@Composable
fun HelpScreen() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(text = "使用帮助", style = MaterialTheme.typography.titleMedium)
        LayeredSectionCard(
            title = "快速开始",
            summary = "4 步完成一次超分任务",
            guide = null,
            expanded = false,
            onToggleExpanded = {},
            showDetailsToggle = false,
            modifier = Modifier.fillMaxWidth(),
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(text = "1. 在主页添加图片或文件夹。", style = MaterialTheme.typography.bodyMedium)
                    Text(text = "2. 在设置 → 模型管理中选择模型。", style = MaterialTheme.typography.bodyMedium)
                    Text(text = "3. 输出默认是 /sdcard/Pictures/CrossScale。", style = MaterialTheme.typography.bodyMedium)
                    Text(text = "4. 点击开始超分后会自动入队并启动。", style = MaterialTheme.typography.bodyMedium)
                }
            },
            details = {}
        )
        Text(
            text = "提示：覆盖原图时建议保留备份（默认开启）。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}
