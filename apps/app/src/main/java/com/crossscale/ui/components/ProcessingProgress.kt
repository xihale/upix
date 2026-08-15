package com.crossscale.ui.components

import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono
import com.crossscale.viewmodel.ProcessingUiState
import kotlin.math.roundToInt

/**
 * 舞台遮罩进度：处理中覆盖在图片舞台之上。
 * 大号进度条 + 一句话阶段 + 已完成/总数 + 取消；细节字段收成单行 mono。
 */
@Composable
fun StageProgressOverlay(
    uiState: ProcessingUiState,
    onCancel: () -> Unit
) {
    val task = uiState.task ?: return

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.55f))
    ) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(
                    Brush.verticalGradient(
                        colors = listOf(
                            MaterialTheme.colorScheme.surface.copy(alpha = 0.0f),
                            MaterialTheme.colorScheme.surface.copy(alpha = 0.75f)
                        )
                    )
                )
        )
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .padding(horizontal = Spacing.lg, vertical = Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            LinearProgressIndicator(
                progress = task.progress.coerceIn(0.0f, 1.0f),
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
            Text(
                text = stringResource(
                    R.string.progress_compact_summary,
                    (task.progress * 100.0f).roundToInt(),
                    task.pendingItems,
                    task.totalItems,
                    task.failedItems
                ),
                style = ValueMono.copy(fontSize = MaterialTheme.typography.labelSmall.fontSize),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            task.currentItemName?.let { itemName ->
                Text(
                    text = itemName,
                    style = ValueMono.copy(fontSize = MaterialTheme.typography.labelSmall.fontSize),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            task.lastErrorMessage?.let { errorMessage ->
                Text(
                    text = errorMessage,
                    style = ValueMono.copy(fontSize = MaterialTheme.typography.labelSmall.fontSize),
                    color = MaterialTheme.colorScheme.error,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            TextButton(
                onClick = onCancel,
                colors = androidx.compose.material3.ButtonDefaults.textButtonColors(
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
