package com.upix.ui.screens.home

import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.automirrored.outlined.List
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
import com.upix.R
import com.upix.ui.theme.EnterMotionSpec
import com.upix.ui.theme.Spacing
import com.upix.ui.theme.pressScale
import androidx.compose.foundation.layout.Spacer

/**
 * 底部悬浮操作区：Surface 药丸容器（非 HorizontalFloatingToolbar——其 collapsed
 * 态在部分版本会折叠掉全部内容，导致主 CTA 不可见）。主 CTA（Button，
 * [DockCtaHeight] = Expressive L 档）永远直接可见：空闲为实底主色「开始超分」；
 * 处理中变为错误红「停止」（[onStop]），不禁用——用户无需进队列面板即可随时中止。
 * 恢复/队列入口为药丸内 IconButton（自带 48dp 触控目标）。
 * Scaffold 的 bottomBar(NavigationBar) 已消费导航栏 insets，
 * 故这里只保留底部悬浮间距。
 */

/** 主 CTA 高度（56dp = MD3 Expressive L 档；spec Appendix A / §9.3）。 */
internal val DockCtaHeight = 56.dp

/** 药丸容器圆角：全高圆弧，与 MD3 floating toolbar 视觉一致。 */
private val DockContainerShape = RoundedCornerShape(percent = 50)

@Composable
internal fun BottomActionDock(
    modifier: Modifier = Modifier,
    isProcessing: Boolean,
    queueCount: Int = 0,
    hasBackupAvailable: Boolean = false,
    onShowQueue: () -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onRestore: () -> Unit = {}
) {
    Box(
        modifier = modifier.fillMaxWidth(),
        contentAlignment = Alignment.BottomCenter
    ) {
        // 按压缩放与按钮共用同一 interactionSource（Motion token：100ms 0.97）。
        val ctaInteractionSource = remember { MutableInteractionSource() }

        Surface(
            shape = DockContainerShape,
            color = MaterialTheme.colorScheme.surfaceContainer,
            contentColor = MaterialTheme.colorScheme.onSurface,
            shadowElevation = 6.dp,
            modifier = Modifier.padding(start = Spacing.lg, end = Spacing.lg, bottom = Spacing.lg)
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                // 左侧贴 CTA（按钮自带内边距），右侧留出图标触控呼吸空间。
                modifier = Modifier.padding(start = Spacing.xs, end = Spacing.xs)
            ) {
                // 同一 CTA 承载开始/停止两个互斥状态：空闲主色可点，处理中
                // 变错误红仍可点（点击即取消），避免「转圈不可控」的死端。
                Button(
                    onClick = if (isProcessing) onStop else onStart,
                    colors = if (isProcessing) {
                        ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.error,
                            contentColor = MaterialTheme.colorScheme.onError
                        )
                    } else {
                        ButtonDefaults.buttonColors()
                    },
                    interactionSource = ctaInteractionSource,
                    modifier = Modifier
                        .height(DockCtaHeight)
                        .pressScale(ctaInteractionSource)
                ) {
                    // 空闲↔停止切换走统一纯 fade，不跳变。
                    AnimatedContent(
                        targetState = isProcessing,
                        transitionSpec = {
                            fadeIn(EnterMotionSpec) togetherWith fadeOut(EnterMotionSpec)
                        },
                        label = "ctaStateFade"
                    ) { processing ->
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                imageVector = if (processing) Icons.Filled.Stop else Icons.Filled.PlayArrow,
                                contentDescription = null,
                                modifier = Modifier.size(22.dp)
                            )
                            Spacer(modifier = Modifier.width(Spacing.sm))
                            Text(
                                text = stringResource(
                                    if (processing) R.string.home_stop_processing else R.string.home_start_upscaling
                                ),
                                style = MaterialTheme.typography.titleMedium
                            )
                        }
                    }
                }

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
                                // 计数是信息不是告警：primaryContainer 取代默认 error 红。
                                Badge(
                                    containerColor = MaterialTheme.colorScheme.primaryContainer,
                                    contentColor = MaterialTheme.colorScheme.onPrimaryContainer
                                ) {
                                    Text(text = if (queueCount > 99) "99+" else queueCount.toString())
                                }
                            }
                        }
                    ) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Outlined.List,
                            contentDescription = stringResource(R.string.home_open_queue_cd)
                        )
                    }
                }
            }
        }
    }
}
