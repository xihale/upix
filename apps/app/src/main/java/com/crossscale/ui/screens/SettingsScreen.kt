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
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import com.crossscale.R
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun SettingsScreen(
    modelDirectoryPath: String,
    selectedModelName: String?,
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit,
    onOpenHelp: () -> Unit,
    isStreamEncrypted: Boolean,
    onStreamEncryptedChange: (Boolean) -> Unit
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
            text = stringResource(R.string.common_settings),
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )

        Text(
            text = stringResource(
                R.string.settings_current_model,
                selectedModelName ?: stringResource(R.string.common_not_selected)
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha))

        SettingsLine(
            title = stringResource(R.string.common_model_management),
            subtitle = stringResource(R.string.settings_model_management_subtitle),
            onClick = onOpenModels
        )
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))

        SettingsLine(
            title = stringResource(R.string.common_logs),
            subtitle = stringResource(R.string.settings_logs_subtitle),
            onClick = onOpenLogs
        )
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))

        SettingsLine(
            title = stringResource(R.string.common_help),
            subtitle = stringResource(R.string.settings_help_subtitle),
            onClick = onOpenHelp
        )

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))

        // 加密数据流传输开关: 控制图像/结果数据流是否加密(吞吐优先时关闭)。
        // 控制帧始终加密; 仅大 payload 走 stream 路径。
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
                .padding(vertical = BorderlessTokens.ListItemVerticalPadding),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing / 2)
            ) {
                Text(
                    text = stringResource(R.string.settings_stream_encryption_title),
                    style = MaterialTheme.typography.bodyLarge
                )
                Text(
                    text = stringResource(R.string.settings_stream_encryption_subtitle),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = isStreamEncrypted,
                onCheckedChange = onStreamEncryptedChange
            )
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
        Text(
            text = stringResource(R.string.common_model_directory, modelDirectoryPath),
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
            text = stringResource(R.string.settings_enter),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary
        )
    }
}
