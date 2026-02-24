package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.viewmodel.ProcessingUiState
import kotlin.math.roundToInt

@Composable
fun ProcessingProgress(uiState: ProcessingUiState) {
    val task = uiState.task ?: return

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = "任务状态: ${task.status.toDisplayLabel()}",
            style = MaterialTheme.typography.titleSmall
        )
        LinearProgressIndicator(
            progress = task.progress.coerceIn(0.0f, 1.0f),
            modifier = Modifier.fillMaxWidth()
        )
        Text(
            text = "进度: ${(task.progress * 100.0f).roundToInt()}%",
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = "已处理: ${task.completedItems}/${task.totalItems}",
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = "待处理: ${task.pendingItems}/${task.totalItems}",
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = "失败: ${task.failedItems}",
            style = MaterialTheme.typography.bodyMedium
        )
        if (task.failedItemNames.isNotEmpty()) {
            val previewNames = task.failedItemNames.take(5)
            Text(
                text = "失败文件(${previewNames.size}/${task.failedItemNames.size}): ${previewNames.joinToString(separator = ", ")}",
                style = MaterialTheme.typography.bodySmall
            )
        }
        if (task.failureDetails.isNotEmpty()) {
            Text(
                text = "失败详情:",
                style = MaterialTheme.typography.bodyMedium
            )
            val previewFailures = task.failureDetails.take(3)
            previewFailures.forEach { detail ->
                Text(
                    text = "• ${detail.displayName.ifBlank { detail.uri }}: ${detail.errorMessage} (${detail.attempts}次)",
                    style = MaterialTheme.typography.bodySmall
                )
            }
            if (task.failureDetails.size > previewFailures.size) {
                val moreCount = task.failureDetails.size - previewFailures.size
                Text(
                    text = "还有 $moreCount 条失败记录",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
        task.currentItemName?.let { itemName ->
            Text(
                text = "当前文件: $itemName",
                style = MaterialTheme.typography.bodySmall
            )
        }
        task.outputDirectory?.let { outputDirectory ->
            Text(
                text = "输出目录: $outputDirectory",
                style = MaterialTheme.typography.bodySmall
            )
        }
        task.lastErrorMessage?.let { errorMessage ->
            Text(
                text = "最近错误: $errorMessage",
                style = MaterialTheme.typography.bodySmall
            )
        }
    }
}

private fun ProcessingStatus.toDisplayLabel(): String {
    return when (this) {
        ProcessingStatus.IDLE -> "空闲"
        ProcessingStatus.RUNNING -> "处理中"
        ProcessingStatus.COMPLETED -> "已完成"
        ProcessingStatus.FAILED -> "失败"
        ProcessingStatus.CANCELED -> "已取消"
    }
}
