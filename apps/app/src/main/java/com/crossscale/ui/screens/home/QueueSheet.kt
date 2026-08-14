package com.crossscale.ui.screens.home

import android.net.Uri
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.data.thumbnails.decodeQueueThumbnailWithLimit
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingUiState

internal fun countPendingQueueItems(queueItems: List<ProcessingQueueItemSummary>): Int {
    return queueItems.count { item -> item.status == PersistedQueueItemStatus.PENDING }
}

internal fun countFailedQueueItems(queueItems: List<ProcessingQueueItemSummary>): Int {
    return queueItems.count { item -> item.status == PersistedQueueItemStatus.FAILED }
}

@Composable
internal fun QueueBottomSheetContent(
    processingUiState: ProcessingUiState,
    onStartBatchQueue: () -> Unit,
    onPauseBatchQueue: () -> Unit,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit,
    onClearBatchQueue: () -> Unit
) {
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = remember(queueItems) { countPendingQueueItems(queueItems) }
    val failedQueueCount = remember(queueItems) { countFailedQueueItems(queueItems) }
    val queueRunningLabel = if (processingUiState.isQueueRunning) {
        stringResource(R.string.queue_running)
    } else {
        stringResource(R.string.queue_paused)
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 300.dp, max = 640.dp)
            .padding(horizontal = BorderlessTokens.ScreenPaddingHorizontal),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text(
            text = stringResource(R.string.queue_details_title),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = stringResource(R.string.queue_status_summary, queueRunningLabel, pendingQueueCount, failedQueueCount),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = onStartBatchQueue,
                enabled = !processingUiState.isProcessing && pendingQueueCount > 0,
                modifier = Modifier.weight(1f)
            ) {
                Text(stringResource(R.string.queue_start))
            }
            OutlinedButton(
                onClick = onPauseBatchQueue,
                enabled = processingUiState.isQueueRunning,
                modifier = Modifier.weight(1f)
            ) {
                Text(stringResource(R.string.queue_pause))
            }
            OutlinedButton(
                onClick = onClearBatchQueue,
                enabled = queueItems.isNotEmpty(),
                modifier = Modifier.weight(1f)
            ) {
                Text(stringResource(R.string.queue_clear))
            }
        }

        if (queueItems.isEmpty()) {
            Text(
                text = stringResource(R.string.queue_empty),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 16.dp)
            )
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                itemsIndexed(items = queueItems, key = { _, item -> item.id }) { index, queueItem ->
                    QueueItemRow(
                        index = index,
                        queueItem = queueItem,
                        isProcessing = processingUiState.isProcessing,
                        onRetryQueuedBatch = onRetryQueuedBatch,
                        onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                        onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                        onRemoveQueuedBatch = onRemoveQueuedBatch
                    )
                    HorizontalDivider(
                        color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(16.dp))
    }
}

@Composable
private fun QueueItemRow(
    index: Int,
    queueItem: ProcessingQueueItemSummary,
    isProcessing: Boolean,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit
) {
    val queueItemStatusRes = when (queueItem.status) {
        PersistedQueueItemStatus.PENDING -> R.string.queue_status_pending
        PersistedQueueItemStatus.RUNNING -> R.string.common_status_processing
        PersistedQueueItemStatus.FAILED -> R.string.common_status_failed
        PersistedQueueItemStatus.COMPLETED -> R.string.common_status_completed
    }
    val canOpenOutput = queueItem.status == PersistedQueueItemStatus.COMPLETED &&
        !queueItem.lastOutputUri.isNullOrBlank()
    val unnamedInputLabel = stringResource(R.string.queue_unnamed_input)
    // Decoding a (possibly multi-source) spec parses JSON and URIs; only
    // recompute when the source itself changes, not on every recomposition.
    val sourceLabel = remember(queueItem.source, unnamedInputLabel) {
        inputSourceLabel(queueItem.source, unnamedInputLabel)
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = canOpenOutput) { onOpenQueuedBatchOutput(queueItem.id) }
            .padding(vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            QueueItemThumbnail(source = queueItem.source)
            Column(modifier = Modifier.weight(1f)) {
                Text(text = "#${index + 1} $sourceLabel", style = MaterialTheme.typography.bodyMedium)
                Text(
                    text = stringResource(R.string.queue_allocation_label, allocationSummary(queueItem)),
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }

        Text(
            text = stringResource(R.string.queue_status_label, stringResource(queueItemStatusRes)),
            style = MaterialTheme.typography.bodySmall
        )

        if (queueItem.status == PersistedQueueItemStatus.FAILED) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { onRetryQueuedBatch(queueItem.id) }, enabled = !isProcessing) {
                    Text(stringResource(R.string.common_retry))
                }
                OutlinedButton(onClick = { onRetryQueuedBatchFailedOnly(queueItem.id) }, enabled = !isProcessing) {
                    Text(stringResource(R.string.queue_retry_failed_only))
                }
            }
        }
        if (canOpenOutput) {
            OutlinedButton(onClick = { onOpenQueuedBatchOutput(queueItem.id) }) {
                Text(stringResource(R.string.queue_open_output))
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(
                onClick = { onRemoveQueuedBatch(queueItem.id) },
                enabled = queueItem.status != PersistedQueueItemStatus.RUNNING
            ) {
                Text(stringResource(R.string.queue_remove))
            }
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = queueItem.source,
                style = MaterialTheme.typography.bodySmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f)
            )
        }
    }
}

@Composable
private fun QueueItemThumbnail(source: String) {
    val context = LocalContext.current
    val bitmapState by produceState<ImageBitmap?>(initialValue = null, source) {
        value = runCatching {
            decodeQueueThumbnailWithLimit(context = context, source = source)
        }.getOrNull()
    }

    Box(
        modifier = Modifier
            .height(56.dp)
            .fillMaxWidth(0.22f)
            .clip(RoundedCornerShape(8.dp)),
        contentAlignment = Alignment.Center
    ) {
        val imageBitmap = bitmapState
        if (imageBitmap != null) {
            Image(
                bitmap = imageBitmap,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop
            )
        } else {
            Text(text = stringResource(R.string.queue_no_image), style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun allocationSummary(item: ProcessingQueueItemSummary): String {
    val mode = if (item.distributedMode) {
        stringResource(R.string.queue_mode_distributed)
    } else {
        stringResource(R.string.queue_mode_standalone)
    }
    val expected = item.expectedItems?.let { count ->
        stringResource(R.string.queue_expected_count, count)
    } ?: stringResource(R.string.queue_expected_pending)
    val overwrite = if (item.overwriteOriginal) {
        stringResource(R.string.common_overwrite_original)
    } else {
        stringResource(R.string.queue_write_to_output)
    }
    val backup = if (item.overwriteOriginal && item.backupOriginal) {
        stringResource(R.string.queue_backup_suffix)
    } else {
        ""
    }
    return stringResource(R.string.queue_allocation_summary, mode, expected, overwrite, backup)
}

private fun inputSourceLabel(source: String, unnamedInputLabel: String): String {
    if (source.isBlank()) {
        return unnamedInputLabel
    }
    val decodedSources = InputSourceSpec.decode(source).orEmpty()
    if (decodedSources.isNotEmpty()) {
        val firstLabel = inputSourceLabel(decodedSources.first(), unnamedInputLabel)
        val remainingCount = decodedSources.size - 1
        return if (remainingCount > 0) "$firstLabel +$remainingCount" else firstLabel
    }
    val parsedUri = runCatching { Uri.parse(source) }.getOrNull()
    val lastSegment = parsedUri?.lastPathSegment
    if (!lastSegment.isNullOrBlank()) {
        return lastSegment
    }
    val fallback = source.substringAfterLast('/', source)
    return if (fallback.isBlank()) source else fallback
}
