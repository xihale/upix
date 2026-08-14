package com.crossscale.ui.components

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
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
            text = stringResource(R.string.progress_task_status, stringResource(task.status.toDisplayLabelRes())),
            style = MaterialTheme.typography.titleSmall
        )
        LinearProgressIndicator(
            progress = task.progress.coerceIn(0.0f, 1.0f),
            modifier = Modifier.fillMaxWidth()
        )
        Text(
            text = stringResource(R.string.progress_progress_percent, (task.progress * 100.0f).roundToInt()),
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = stringResource(R.string.progress_completed_items, task.completedItems, task.totalItems),
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = stringResource(R.string.progress_pending_items, task.pendingItems, task.totalItems),
            style = MaterialTheme.typography.bodyMedium
        )
        Text(
            text = stringResource(R.string.progress_failed_count, task.failedItems),
            style = MaterialTheme.typography.bodyMedium
        )
        if (task.failedItemNames.isNotEmpty()) {
            val previewNames = task.failedItemNames.take(5)
            Text(
                text = stringResource(
                    R.string.progress_failed_files,
                    previewNames.size,
                    task.failedItemNames.size,
                    previewNames.joinToString(separator = ", ")
                ),
                style = MaterialTheme.typography.bodySmall
            )
        }
        if (task.failureDetails.isNotEmpty()) {
            Text(
                text = stringResource(R.string.progress_failure_details),
                style = MaterialTheme.typography.bodyMedium
            )
            val previewFailures = task.failureDetails.take(3)
            previewFailures.forEach { detail ->
                Text(
                    text = stringResource(
                        R.string.progress_failure_detail_item,
                        detail.displayName.ifBlank { detail.uri },
                        detail.errorMessage,
                        detail.attempts
                    ),
                    style = MaterialTheme.typography.bodySmall
                )
            }
            if (task.failureDetails.size > previewFailures.size) {
                val moreCount = task.failureDetails.size - previewFailures.size
                Text(
                    text = stringResource(R.string.progress_more_failures, moreCount),
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
        task.currentItemName?.let { itemName ->
            Text(
                text = stringResource(R.string.progress_current_file, itemName),
                style = MaterialTheme.typography.bodySmall
            )
        }
        task.lastProcessedItemName?.let { itemName ->
            val processorLabel = task.lastProcessorLabel ?: stringResource(R.string.progress_unknown_node)
            Text(
                text = stringResource(R.string.progress_last_completed, itemName, processorLabel),
                style = MaterialTheme.typography.bodySmall
            )
        }
        if (task.recentProcessorAttributions.isNotEmpty()) {
            val previewAttributions = task.recentProcessorAttributions.takeLast(3)
            Text(
                text = stringResource(
                    R.string.progress_recent_attributions,
                    previewAttributions.joinToString(separator = " | ")
                ),
                style = MaterialTheme.typography.bodySmall
            )
        }
        task.outputDirectory?.let { outputDirectory ->
            Text(
                text = stringResource(R.string.progress_output_directory, outputDirectory),
                style = MaterialTheme.typography.bodySmall
            )
        }
        task.lastErrorMessage?.let { errorMessage ->
            Text(
                text = stringResource(R.string.progress_last_error, errorMessage),
                style = MaterialTheme.typography.bodySmall
            )
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
