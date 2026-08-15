package com.crossscale.ui.screens.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.outlined.List
import androidx.compose.material.icons.outlined.Restore
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.theme.Spacing

/**
 * 底部悬浮 Dock：不贴屏，浮在滚动内容之上。
 * 主 CTA + 可选恢复备份入口 + 队列入口。
 * Scaffold 的 bottomBar(NavigationBar) 已消费导航栏 insets，
 * 故这里不再叠加 navigationBarsPadding，仅保留底部 16dp 悬浮间距。
 */
@Composable
internal fun BottomActionDock(
    modifier: Modifier = Modifier,
    isProcessing: Boolean,
    hasBackupAvailable: Boolean = false,
    hasQueueItems: Boolean = false,
    onShowQueue: () -> Unit,
    onStart: () -> Unit,
    onRestore: () -> Unit = {}
) {
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .padding(bottom = 16.dp),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh
    ) {
        Row(
            modifier = Modifier.padding(Spacing.md),
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            verticalAlignment = Alignment.CenterVertically
        ) {
            val ctaLabel = stringResource(
                if (isProcessing) R.string.home_continue_queue else R.string.home_start_upscaling
            )
            Button(
                onClick = onStart,
                modifier = Modifier
                    .weight(1f)
                    .height(52.dp),
                shape = CircleShape,
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary
                )
            ) {
                Icon(
                    imageVector = Icons.Filled.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp)
                )
                Text(text = ctaLabel)
            }

            if (hasBackupAvailable && !isProcessing && !hasQueueItems) {
                IconButton(
                    onClick = onRestore,
                    modifier = Modifier.size(36.dp)
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Restore,
                        contentDescription = stringResource(R.string.home_restore_backup),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(20.dp)
                    )
                }
            }

            IconButton(
                onClick = onShowQueue,
                modifier = Modifier.size(36.dp)
            ) {
                Icon(
                    imageVector = Icons.Outlined.List,
                    contentDescription = stringResource(R.string.home_open_queue_cd),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(20.dp)
                )
            }
        }
    }
}
