package com.crossscale.ui.screens.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.viewmodel.ProcessingUiState

@Composable
internal fun CheckpointSection(
    processingUiState: ProcessingUiState,
    onResumeProcessing: () -> Unit,
    onRetryFailedItems: () -> Unit,
    onResumePendingItem: (String) -> Unit,
    onRetryFailedItem: (String) -> Unit,
    onDiscardCheckpoint: () -> Unit
) {
    val checkpointSummary = processingUiState.checkpointSummary ?: return

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            text = "断点续传",
            style = MaterialTheme.typography.titleSmall,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = "${checkpointSummary.statusLabel} · 待处理 ${checkpointSummary.pendingItems} · 失败 ${checkpointSummary.failedItems}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onResumeProcessing, modifier = Modifier.weight(1f)) {
                Text("继续")
            }
            OutlinedButton(
                onClick = onRetryFailedItems,
                enabled = processingUiState.canRetryFailedOnly,
                modifier = Modifier.weight(1f)
            ) {
                Text("重试失败")
            }
            OutlinedButton(onClick = onDiscardCheckpoint, modifier = Modifier.weight(1f)) {
                Text("丢弃")
            }
        }

        checkpointSummary.pendingItemDetails.take(10).forEach { item ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = item.displayName,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.bodySmall
                )
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    TextButton(onClick = { onResumePendingItem(item.uri) }) {
                        Text("继续")
                    }
                    if (item.isFailed) {
                        TextButton(onClick = { onRetryFailedItem(item.uri) }) {
                            Text("重试")
                        }
                    }
                }
            }
            HorizontalDivider(
                color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerSubtleAlpha)
            )
        }
    }
}
