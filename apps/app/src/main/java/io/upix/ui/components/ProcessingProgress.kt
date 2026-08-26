package com.upix.ui.components

import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import com.upix.R
import com.upix.data.model.ProcessingStatus
import com.upix.data.model.ProcessingTask
import com.upix.ui.theme.Spacing
import com.upix.ui.theme.smoothProgress
import com.upix.ui.theme.valueStyleFor

/**
 * 舞台遮罩进度：处理中覆盖在图片舞台之上。
 * 大号进度条 + 一句话阶段 + 已完成/总数 + 取消；细节字段收成单行 mono。
 * 只接收 [task]（而非整个 UI state）以缩小上游重组面。
 */
@Composable
fun StageProgressOverlay(
    task: ProcessingTask?,
    onCancel: () -> Unit
) {
    val task = task ?: return

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.55f))
    ) {
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .padding(horizontal = Spacing.lg, vertical = Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            // 进度数值做 300ms 线性平滑（lambda 版 API）；百分比文字保持即时。
            val animatedProgress = smoothProgress(task.progress.coerceIn(0.0f, 1.0f))
            LinearProgressIndicator(
                progress = { animatedProgress },
                modifier = Modifier.fillMaxWidth()
            )
            Text(
                text = stringResource(task.status.toDisplayLabelRes()),
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurface
            )
            Text(
                text = stringResource(
                    R.string.progress_completed_items,
                    task.completedItems,
                    task.totalItems
                ),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            if (task.skippedItems > 0) {
                Text(
                    text = stringResource(R.string.processing_progress_skipped, task.skippedItems),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            task.currentItemName?.let { itemName ->
                // 文件/模型名属机器值：ASCII 走 ValueMono，含 CJK 回退系统字阶（§2.1/§2.2）。
                Text(
                    text = itemName,
                    style = valueStyleFor(itemName),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            task.lastErrorMessage?.let { errorMessage ->
                // 错误文案可能含中文：必须经 valueStyleFor 做 CJK 回退（§2.2）。
                Text(
                    text = errorMessage,
                    style = valueStyleFor(errorMessage),
                    color = MaterialTheme.colorScheme.error,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            TextButton(
                onClick = onCancel,
                colors = ButtonDefaults.textButtonColors(
                    contentColor = MaterialTheme.colorScheme.error
                )
            ) {
                Text(stringResource(R.string.common_cancel))
            }
        }
    }
}

@StringRes
private fun ProcessingStatus.toDisplayLabelRes(): Int {
    return when (this) {
        ProcessingStatus.IDLE -> R.string.progress_status_idle
        ProcessingStatus.RUNNING -> R.string.common_status_processing
        ProcessingStatus.COMPLETED -> R.string.common_status_completed
        ProcessingStatus.FAILED -> R.string.common_status_failed
        ProcessingStatus.CANCELED -> R.string.progress_status_canceled
    }
}
