package com.crossscale.ui.screens.home

import android.net.Uri
import androidx.compose.foundation.Image
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Pause
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono
import com.crossscale.ui.theme.pressable
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingUiState

internal fun countPendingQueueItems(queueItems: List<ProcessingQueueItemSummary>): Int {
    return queueItems.count { item -> item.status == PersistedQueueItemStatus.PENDING }
}

internal fun countFailedQueueItems(queueItems: List<ProcessingQueueItemSummary>): Int {
    return queueItems.count { item -> item.status == PersistedQueueItemStatus.FAILED }
}

/**
 * 处理队列 sheet：头部（标题/数量/启停/清空）+ 紧凑列表项。
 * 每项右侧最多 2 个图标操作；条目间用留白，不再使用 Divider / 按钮墙。
 */
@Composable
internal fun QueueBottomSheetContent(
    processingUiState: ProcessingUiState,
    onStartBatchQueue: () -> Unit,
    onPauseBatchQueue: () -> Unit,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit,
    onClearBatchQueue: () -> Unit,
    onDismiss: () -> Unit
) {
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = remember(queueItems) { countPendingQueueItems(queueItems) }
    val failedQueueCount = remember(queueItems) { countFailedQueueItems(queueItems) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 240.dp, max = 640.dp)
            .padding(horizontal = Spacing.xl),
        verticalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = stringResource(R.string.queue_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.weight(1f)
            )
            Text(
                text = stringResource(
                    R.string.queue_sheet_summary,
                    pendingQueueCount,
                    failedQueueCount
                ),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            IconButton(onClick = onStartBatchQueue, enabled = !processingUiState.isProcessing && pendingQueueCount > 0) {
                Icon(
                    imageVector = Icons.Outlined.PlayArrow,
                    contentDescription = stringResource(R.string.queue_start),
                    modifier = Modifier.size(20.dp)
                )
            }
            IconButton(onClick = onPauseBatchQueue, enabled = processingUiState.isQueueRunning) {
                Icon(
                    imageVector = Icons.Outlined.Pause,
                    contentDescription = stringResource(R.string.queue_pause),
                    modifier = Modifier.size(20.dp)
                )
            }
            IconButton(onClick = onClearBatchQueue, enabled = queueItems.isNotEmpty()) {
                Icon(
                    imageVector = Icons.Outlined.DeleteSweep,
                    contentDescription = stringResource(R.string.queue_clear),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(20.dp)
                )
            }
            IconButton(onClick = onDismiss) {
                Icon(
                    imageVector = Icons.Outlined.Close,
                    contentDescription = stringResource(R.string.queue_close_cd),
                    modifier = Modifier.size(20.dp)
                )
            }
        }

        if (queueItems.isEmpty()) {
            Text(
                text = stringResource(R.string.queue_empty),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = Spacing.lg)
            )
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(Spacing.xs)
            ) {
                itemsIndexed(items = queueItems, key = { _, item -> item.id }) { index, queueItem ->
                    QueueItemRow(
                        index = index,
                        queueItem = queueItem,
                        isProcessing = processingUiState.isProcessing,
                        modifier = Modifier.animateItem(),
                        onRetryQueuedBatch = onRetryQueuedBatch,
                        onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                        onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                        onRemoveQueuedBatch = onRemoveQueuedBatch
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(Spacing.lg))
    }
}

@Composable
private fun QueueItemRow(
    index: Int,
    queueItem: ProcessingQueueItemSummary,
    isProcessing: Boolean,
    modifier: Modifier = Modifier,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit
) {
    val canOpenOutput = queueItem.status == PersistedQueueItemStatus.COMPLETED &&
        !queueItem.lastOutputUri.isNullOrBlank()
    val unnamedInputLabel = stringResource(R.string.queue_unnamed_input)
    // Decoding a (possibly multi-source) spec parses JSON and URIs; only
    // recompute when the source itself changes, not on every recomposition.
    val sourceLabel = remember(queueItem.source, unnamedInputLabel) {
        inputSourceLabel(queueItem.source, unnamedInputLabel)
    }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .pressable(enabled = canOpenOutput) { onOpenQueuedBatchOutput(queueItem.id) }
            .padding(vertical = Spacing.xs),
        verticalArrangement = Arrangement.spacedBy(Spacing.xs)
    ) {
        Row(
            horizontalArrangement = Arrangement.spacedBy(Spacing.md),
            verticalAlignment = Alignment.CenterVertically
        ) {
            QueueItemThumbnail(source = queueItem.source)
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp)
            ) {
                Text(
                    text = "#${index + 1} $sourceLabel",
                    style = ValueMono,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                Text(
                    text = stringResource(R.string.queue_allocation_label, allocationSummary(queueItem)),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            QueueStatusChip(status = queueItem.status)
        }

        if (queueItem.status == PersistedQueueItemStatus.FAILED) {
            Text(
                text = queueItem.lastErrorMessage.orEmpty(),
                style = ValueMono.copy(fontSize = MaterialTheme.typography.labelSmall.fontSize),
                color = MaterialTheme.colorScheme.error,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 68.dp)
            )
        }

        Row(
            horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
            verticalAlignment = Alignment.CenterVertically
        ) {
            if (queueItem.status == PersistedQueueItemStatus.FAILED) {
                TextButton(
                    onClick = { onRetryQueuedBatchFailedOnly(queueItem.id) },
                    enabled = !isProcessing,
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = Spacing.sm)
                ) {
                    Text(
                        text = stringResource(R.string.queue_retry_failed_only),
                        style = MaterialTheme.typography.labelMedium
                    )
                }
            }
            Spacer(modifier = Modifier.weight(1f))
            if (queueItem.status == PersistedQueueItemStatus.FAILED) {
                QueueItemAction(
                    iconVector = Icons.Outlined.Refresh,
                    contentDescriptionRes = R.string.common_retry,
                    onClick = { onRetryQueuedBatch(queueItem.id) },
                    enabled = !isProcessing
                )
            }
            if (canOpenOutput) {
                QueueItemAction(
                    iconVector = Icons.Outlined.FolderOpen,
                    contentDescriptionRes = R.string.queue_open_output,
                    onClick = { onOpenQueuedBatchOutput(queueItem.id) }
                )
            }
            QueueItemAction(
                iconVector = Icons.Outlined.Close,
                contentDescriptionRes = R.string.queue_remove,
                onClick = { onRemoveQueuedBatch(queueItem.id) },
                enabled = queueItem.status != PersistedQueueItemStatus.RUNNING
            )
        }

        if (queueItem.status == PersistedQueueItemStatus.RUNNING) {
            LinearProgressIndicator(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(4.dp)
                    .clip(MaterialTheme.shapes.extraSmall)
            )
        }
    }
}

@Composable
private fun QueueItemAction(
    iconVector: androidx.compose.ui.graphics.vector.ImageVector,
    contentDescriptionRes: Int,
    onClick: () -> Unit,
    enabled: Boolean = true
) {
    IconButton(onClick = onClick, enabled = enabled, modifier = Modifier.size(32.dp)) {
        Icon(
            imageVector = iconVector,
            contentDescription = stringResource(contentDescriptionRes),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(18.dp)
        )
    }
}

/** 小号状态 chip：排队=surfaceVariant / 进行中=primary / 完成=primary 淡 / 失败=error 淡。 */
@Composable
private fun QueueStatusChip(status: PersistedQueueItemStatus) {
    val (container, content, labelRes) = when (status) {
        PersistedQueueItemStatus.PENDING -> Triple(
            MaterialTheme.colorScheme.surfaceVariant,
            MaterialTheme.colorScheme.onSurfaceVariant,
            R.string.queue_status_pending
        )
        PersistedQueueItemStatus.RUNNING -> Triple(
            MaterialTheme.colorScheme.primary,
            MaterialTheme.colorScheme.onPrimary,
            R.string.common_status_processing
        )
        PersistedQueueItemStatus.COMPLETED -> Triple(
            MaterialTheme.colorScheme.primary.copy(alpha = 0.16f),
            MaterialTheme.colorScheme.primary,
            R.string.common_status_completed
        )
        PersistedQueueItemStatus.FAILED -> Triple(
            MaterialTheme.colorScheme.error.copy(alpha = 0.16f),
            MaterialTheme.colorScheme.error,
            R.string.common_status_failed
        )
    }
    Surface(shape = MaterialTheme.shapes.extraSmall, color = container, contentColor = content) {
        Text(
            text = stringResource(labelRes),
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.padding(horizontal = Spacing.sm, vertical = 2.dp)
        )
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
            .size(56.dp)
            .clip(MaterialTheme.shapes.small),
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
            Text(text = stringResource(R.string.queue_no_image), style = MaterialTheme.typography.labelSmall)
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
