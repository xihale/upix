package com.crossscale.ui.screens.home

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.outlined.List
import androidx.compose.material.icons.outlined.Restore
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.HorizontalFloatingToolbar
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.theme.Spacing
import androidx.compose.foundation.layout.Spacer

/**
 * 底部悬浮操作区：MD3 Expressive 官方 HorizontalFloatingToolbar 模式——
 * 主 CTA（Button，56dp = Expressive L 档）作 leadingContent，恢复/队列入口
 * 为 toolbar 内 IconButton（自带 48dp 触控目标与容器投影）。
 * Scaffold 的 bottomBar(NavigationBar) 已消费导航栏 insets，
 * 故这里只保留底部悬浮间距。
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
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
    Box(
        modifier = modifier.fillMaxWidth(),
        contentAlignment = Alignment.BottomCenter
    ) {
        val ctaLabel = stringResource(
            if (isProcessing) R.string.home_processing else R.string.home_start_upscaling
        )

        HorizontalFloatingToolbar(
            expanded = false,
            modifier = Modifier.padding(start = Spacing.lg, end = Spacing.lg, bottom = Spacing.lg),
            leadingContent = {
                Button(
                    onClick = onStart,
                    enabled = !isProcessing,
                    modifier = Modifier.height(56.dp)
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
            },
            content = {
                // 恢复入口只看备份可用性：队列存在不应劫持恢复操作。
                if (hasBackupAvailable && !isProcessing) {
                    IconButton(onClick = onRestore) {
                        Icon(
                            imageVector = Icons.Outlined.Restore,
                            contentDescription = stringResource(R.string.home_restore_backup)
                        )
                    }
                }

                IconButton(onClick = onShowQueue) {
                    BadgedBox(
                        badge = {
                            if (queueCount > 0) {
                                Badge {
                                    Text(text = if (queueCount > 99) "99+" else queueCount.toString())
                                }
                            }
                        }
                    ) {
                        Icon(
                            imageVector = Icons.Outlined.List,
                            contentDescription = stringResource(R.string.home_open_queue_cd)
                        )
                    }
                }
            }
        )
    }
}
