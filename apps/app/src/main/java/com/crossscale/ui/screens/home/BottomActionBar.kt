package com.crossscale.ui.screens.home

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.interaction.MutableInteractionSource
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
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.theme.EnterMotionSpec
import com.crossscale.ui.theme.EnterOffsetSpec
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.pressScale

/**
 * 底部悬浮 Dock：不贴屏，浮在滚动内容之上。
 * 主 CTA（青色圆角胶囊）+ 可选恢复备份入口 + 队列入口（角标显示待办数）。
 * Scaffold 的 bottomBar(NavigationBar) 已消费导航栏 insets，
 * 故这里不再叠加 navigationBarsPadding，仅保留底部 16dp 悬浮间距。
 */
@Composable
internal fun BottomActionDock(
    modifier: Modifier = Modifier,
    isProcessing: Boolean,
    pendingQueueCount: Int,
    failedQueueCount: Int,
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
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        shadowElevation = 8.dp
    ) {
        Row(
            modifier = Modifier.padding(Spacing.md),
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 主 CTA：M3 Button 自带涟漪/按压态，这里仅叠加统一的按压缩放
            // （同一 interactionSource 同时喂给 Button 与 pressScale，不丢原生反馈）。
            val ctaInteractionSource = remember { MutableInteractionSource() }
            val ctaLabel = stringResource(
                if (isProcessing) R.string.home_continue_queue else R.string.home_start_upscaling
            )
            Button(
                onClick = onStart,
                modifier = Modifier
                    .weight(1f)
                    .height(52.dp)
                    .pressScale(ctaInteractionSource),
                interactionSource = ctaInteractionSource,
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
                // 只包文字：文案切换做 fade + 约 8dp 上滑（内容高的 1/3），Dock 本体不动。
                AnimatedContent(
                    targetState = ctaLabel,
                    transitionSpec = {
                        (fadeIn(EnterMotionSpec) + slideInVertically(EnterOffsetSpec) { it / 3 }) togetherWith
                            (fadeOut(EnterMotionSpec) + slideOutVertically(EnterOffsetSpec) { it / 3 })
                    },
                    label = "dockCtaLabel"
                ) { label ->
                    Text(text = label)
                }
            }

            if (hasBackupAvailable && !isProcessing && !hasQueueItems) {
                IconButton(
                    onClick = onRestore,
                    modifier = Modifier.size(36.dp)
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Restore,
                        contentDescription = stringResource(R.string.home_restore_backup),
                        tint = MaterialTheme.colorScheme.tertiary,
                        modifier = Modifier.size(20.dp)
                    )
                }
            }

            BadgedBox(
                badge = {
                    val activeCount = pendingQueueCount + failedQueueCount
                    if (activeCount > 0) {
                        Badge(
                            containerColor = if (failedQueueCount > 0) {
                                MaterialTheme.colorScheme.error
                            } else {
                                MaterialTheme.colorScheme.primary
                            },
                            contentColor = MaterialTheme.colorScheme.onPrimary
                        ) {
                            Text(activeCount.toString())
                        }
                    }
                }
            ) {
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
}
