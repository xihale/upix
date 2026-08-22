package com.crossscale.ui.screens.home

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.outlined.List
import androidx.compose.material.icons.outlined.Restore
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.pressScale

/**
 * 底部悬浮操作区：主 CTA 药丸按钮（带投影 + 按压缩放）+ 圆形队列入口（角标计数）。
 * 去掉整条底板容器，让按钮直接浮在滚动内容之上，更轻、更聚焦。
 * Scaffold 的 bottomBar(NavigationBar) 已消费导航栏 insets，
 * 故这里不再叠加 navigationBarsPadding，仅保留底部 16dp 悬浮间距。
 */
@Composable
internal fun BottomActionDock(
    modifier: Modifier = Modifier,
    isProcessing: Boolean,
    queueCount: Int = 0,
    hasBackupAvailable: Boolean = false,
    onShowQueue: () -> Unit,
    onStart: () -> Unit,
    onRestore: () -> Unit = {}
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(start = Spacing.xl, end = Spacing.xl, bottom = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(Spacing.md),
        verticalAlignment = Alignment.CenterVertically
    ) {
        val ctaInteraction = remember { MutableInteractionSource() }
        val ctaLabel = stringResource(
            if (isProcessing) R.string.home_processing else R.string.home_start_upscaling
        )
        Button(
            onClick = onStart,
            enabled = !isProcessing,
            modifier = Modifier
                .weight(1f)
                .height(56.dp)
                .pressScale(ctaInteraction),
            shape = CircleShape,
            interactionSource = ctaInteraction,
            elevation = ButtonDefaults.buttonElevation(
                defaultElevation = 8.dp,
                pressedElevation = 2.dp
            ),
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primary,
                contentColor = MaterialTheme.colorScheme.onPrimary
            )
        ) {
            if (isProcessing) {
                CircularProgressIndicator(
                    modifier = Modifier.size(22.dp),
                    strokeWidth = 2.5.dp
                )
            } else {
                Icon(
                    imageVector = Icons.Filled.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(22.dp)
                )
            }
            Spacer(modifier = Modifier.width(Spacing.sm))
            Text(text = ctaLabel, style = MaterialTheme.typography.titleMedium)
        }

        // 恢复入口只看备份可用性：队列存在不应劫持恢复操作。
        if (hasBackupAvailable && !isProcessing) {
            DockCircleEntry(
                icon = Icons.Outlined.Restore,
                contentDescription = stringResource(R.string.home_restore_backup),
                onClick = onRestore
            )
        }

        DockCircleEntry(
            icon = Icons.Outlined.List,
            contentDescription = stringResource(R.string.home_open_queue_cd),
            onClick = onShowQueue
        ) {
            if (queueCount > 0) {
                Badge {
                    Text(text = if (queueCount > 99) "99+" else queueCount.toString())
                }
            }
        }
    }
}

/** 圆形次级入口：tonal 圆面 + 细描边，可选角标内容。 */
@Composable
private fun DockCircleEntry(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    badgeContent: @Composable BoxScope.() -> Unit = {}
) {
    Surface(
        modifier = Modifier.size(56.dp),
        shape = CircleShape,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
    ) {
        IconButton(onClick = onClick, modifier = Modifier.fillMaxSize()) {
            BadgedBox(badge = badgeContent) {
                Icon(
                    imageVector = icon,
                    contentDescription = contentDescription,
                    modifier = Modifier.size(22.dp),
                    tint = MaterialTheme.colorScheme.onSurface
                )
            }
        }
    }
}
