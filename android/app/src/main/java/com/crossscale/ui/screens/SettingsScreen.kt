package com.crossscale.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun SettingsScreen(
    modelDirectoryPath: String,
    selectedModelName: String?,
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit,
    onOpenHelp: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(
                horizontal = BorderlessTokens.ScreenPaddingHorizontal,
                vertical = BorderlessTokens.ScreenPaddingVertical
            ),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)
    ) {
        Text(
            text = "设置",
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )

        Text(
            text = "当前模型：${selectedModelName ?: "未选择"}",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha))

        SettingsLine(title = "模型管理", subtitle = "下载、删除与切换模型", onClick = onOpenModels)
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))

        SettingsLine(title = "日志", subtitle = "查看运行和错误日志", onClick = onOpenLogs)
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))

        SettingsLine(title = "使用帮助", subtitle = "快速开始与常见问题", onClick = onOpenHelp)

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
        Text(
            text = "模型目录：$modelDirectoryPath",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
private fun SettingsLine(
    title: String,
    subtitle: String,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            .padding(vertical = BorderlessTokens.ListItemVerticalPadding),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing / 2)
        ) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge)
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Text(
            text = "进入",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary
        )
    }
}
