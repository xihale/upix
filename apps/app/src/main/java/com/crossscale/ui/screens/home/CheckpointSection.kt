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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.viewmodel.ProcessingCheckpointSummary
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
            text = stringResource(R.string.checkpoint_title),
            style = MaterialTheme.typography.titleSmall,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = stringResource(
                R.string.checkpoint_summary,
                checkpointStatusLabel(checkpointSummary),
                checkpointSummary.pendingItems,
                checkpointSummary.failedItems
            ),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onResumeProcessing, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.checkpoint_resume))
            }
            OutlinedButton(
                onClick = onRetryFailedItems,
                enabled = processingUiState.canRetryFailedOnly,
                modifier = Modifier.weight(1f)
            ) {
                Text(stringResource(R.string.checkpoint_retry_failed))
            }
            OutlinedButton(onClick = onDiscardCheckpoint, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.checkpoint_discard))
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
                        Text(stringResource(R.string.checkpoint_resume))
                    }
                    if (item.isFailed) {
                        TextButton(onClick = { onRetryFailedItem(item.uri) }) {
                            Text(stringResource(R.string.common_retry))
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

/**
 * Resolves the checkpoint status text from the summary counts at the
 * consumption point. Mirrors the statusLabel derivation in
 * ProcessingMappers.toUiSummary (which resolves the same resources via
 * StringProvider on the viewmodel side).
 */
@Composable
private fun checkpointStatusLabel(summary: ProcessingCheckpointSummary): String {
    val statusRes = when {
        summary.pendingItems <= 0 && summary.failedItems <= 0 -> R.string.common_status_completed
        summary.failedItems > 0 -> R.string.checkpoint_status_resumable_with_failures
        else -> R.string.checkpoint_status_resumable
    }
    return stringResource(statusRes)
}
