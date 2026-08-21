package com.crossscale.ui.screens

import android.content.Intent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.automirrored.outlined.ReceiptLong
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.components.SectionHeader
import com.crossscale.ui.theme.Spacing

/**
 * 设置屏：分组卡列表（组间留白 Spacing.lg，组内行间留白，不用 Divider）。
 */
@Composable
fun SettingsScreen(
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
            .verticalScroll(rememberScrollState())
            .padding(horizontal = Spacing.xl)
            .padding(top = Spacing.md, bottom = Spacing.xxl),
        verticalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        Text(
            text = stringResource(R.string.common_settings),
            style = MaterialTheme.typography.titleLarge
        )
        Text(
            text = stringResource(
                R.string.settings_current_model,
                selectedModelName ?: stringResource(R.string.common_not_selected)
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        SectionHeader(text = stringResource(R.string.settings_section_general))
        SettingsGroupCard {
            SettingsEntryRow(
                icon = Icons.Outlined.Memory,
                title = stringResource(R.string.common_model_management),
                onClick = onOpenModels
            )
            SettingsEntryRow(
                icon = Icons.AutoMirrored.Outlined.ReceiptLong,
                title = stringResource(R.string.common_logs),
                onClick = onOpenLogs
            )
            SettingsEntryRow(
                icon = Icons.AutoMirrored.Outlined.HelpOutline,
                title = stringResource(R.string.common_help),
                onClick = onOpenHelp
            )
        }

        SectionHeader(text = stringResource(R.string.settings_section_transfer))
        SettingsGroupCard {
            // 加密数据流传输开关: 控制图像/结果数据流是否加密(吞吐优先时关闭)。
            // 控制帧始终加密; 仅大 payload 走 stream 路径。
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp)
                    .padding(horizontal = Spacing.md, vertical = Spacing.sm),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Outlined.Lock,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(20.dp)
                )
                Column(
                    modifier = Modifier
                        .weight(1f)
                        .padding(start = Spacing.md),
                    verticalArrangement = Arrangement.spacedBy(1.dp)
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
        }

        SectionHeader(text = stringResource(R.string.settings_section_about))
        SettingsGroupCard {
            val context = LocalContext.current
            val versionName = remember {
                runCatching {
                    context.packageManager.getPackageInfo(context.packageName, 0).versionName
                }.getOrNull()
            }
            SettingsEntryRow(
                icon = Icons.Outlined.Info,
                title = stringResource(R.string.settings_about_version, versionName ?: "-")
            )
            SettingsEntryRow(
                icon = Icons.Outlined.Link,
                title = stringResource(R.string.settings_about_project),
                subtitle = PROJECT_URL.removePrefix("https://"),
                onClick = {
                    runCatching {
                        context.startActivity(
                            Intent(Intent.ACTION_VIEW, android.net.Uri.parse(PROJECT_URL))
                        )
                    }
                }
            )
        }
    }
}

/** 开源项目地址（关于页展示并可跳转）。 */
private const val PROJECT_URL = "https://github.com/xihale/crossscale"

@Composable
private fun SettingsGroupCard(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier.padding(vertical = Spacing.sm),
            verticalArrangement = Arrangement.spacedBy(Spacing.xs)
        ) {
            content()
        }
    }
}

@Composable
private fun SettingsEntryRow(
    icon: ImageVector,
    title: String,
    subtitle: String? = null,
    onClick: (() -> Unit)? = null
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .then(
                if (onClick != null) {
                    Modifier.clickable(onClick = onClick)
                } else {
                    Modifier
                }
            )
            .padding(horizontal = Spacing.md, vertical = Spacing.sm),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(20.dp)
        )
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(start = Spacing.md),
            verticalArrangement = Arrangement.spacedBy(1.dp)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        if (onClick != null) {
            Icon(
                imageVector = Icons.Outlined.ChevronRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(20.dp)
            )
        }
    }
}
