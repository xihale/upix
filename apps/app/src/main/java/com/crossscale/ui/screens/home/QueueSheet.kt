package com.crossscale.ui.screens.home

import android.net.Uri
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Inbox
import androidx.compose.material.icons.outlined.Pause
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
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
import com.crossscale.ui.theme.valueStyleFor
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingUiState

internal fun countPendingQueueItems(queueItems: List<ProcessingQueueItemSummary>): Int {
    return queueItems.count { item -> item.status == PersistedQueueItemStatus.PENDING }
}

internal fun countFailedQueueItems(queueItems: List<ProcessingQueueItemSummary>): Int {
    return queueItems.count { item -> item.status == PersistedQueueItemStatus.FAILED }
}

/**
 * 处理队列 sheet：头部（标题/统计 + 官方 IconButton 家族动作）+ 列表。
 * 列表项 = M3 [ListItem] 承载内容，外层 [SwipeToDismissBox] 左滑移除；
 * 状态用容器色胶囊表达；运行中条目底部走官方不确定进度条。
 */
@Composable
internal fun QueueBottomSheetContent(
    processingUiState: ProcessingUiState,
    onStartBatchQueue: () -> Unit,
    onPauseBatchQueue: () -> Unit,
    onCancelProcessing: () -> Unit,
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
    // 清空为批量破坏性操作：先弹确认框再执行。
    var showClearConfirm by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 240.dp, max = 640.dp)
            .padding(horizontal = Spacing.xl),
        verticalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.queue_sheet_title),
                    style = MaterialTheme.typography.titleMedium
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
            }
            // 启停互斥：同一位置在 播放/暂停 间切换，强调色用 FilledTonal。
            if (processingUiState.isQueueRunning) {
                FilledTonalIconButton(onClick = onPauseBatchQueue) {
                    Icon(
                        imageVector = Icons.Outlined.Pause,
                        contentDescription = stringResource(R.string.queue_pause),
                        modifier = Modifier.size(20.dp)
                    )
                }
            } else {
                FilledTonalIconButton(
                    onClick = onStartBatchQueue,
                    enabled = !processingUiState.isProcessing && pendingQueueCount > 0
                ) {
                    Icon(
                        imageVector = Icons.Outlined.PlayArrow,
                        contentDescription = stringResource(R.string.queue_start),
                        modifier = Modifier.size(20.dp)
                    )
                }
            }
            IconButton(
                onClick = { showClearConfirm = true },
                enabled = queueItems.isNotEmpty()
            ) {
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
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 180.dp),
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(Spacing.sm)
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Inbox,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.outline,
                        modifier = Modifier.size(32.dp)
                    )
                    Text(
                        text = stringResource(R.string.queue_empty),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(Spacing.sm)
            ) {
                itemsIndexed(items = queueItems, key = { _, item -> item.id }) { index, queueItem ->
                    DismissibleQueueItem(
                        index = index,
                        queueItem = queueItem,
                        isProcessing = processingUiState.isProcessing,
                        modifier = Modifier.animateItem(),
                        onCancelProcessing = onCancelProcessing,
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

    if (showClearConfirm) {
        AlertDialog(
            onDismissRequest = { showClearConfirm = false },
            title = { Text(stringResource(R.string.queue_clear_confirm_title)) },
            text = { Text(stringResource(R.string.queue_clear_confirm_body, queueItems.size)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showClearConfirm = false
                        onClearBatchQueue()
                    }
                ) {
                    Text(stringResource(R.string.common_confirm))
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearConfirm = false }) {
                    Text(stringResource(R.string.common_cancel))
                }
            }
        )
    }
}

/** 左滑移除包装：背景为 errorContainer + 删除图标；运行中禁止滑动。 */
@Composable
private fun DismissibleQueueItem(
    index: Int,
    queueItem: ProcessingQueueItemSummary,
    isProcessing: Boolean,
    modifier: Modifier = Modifier,
    onCancelProcessing: () -> Unit,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit
) {
    val canRemove = queueItem.status != PersistedQueueItemStatus.RUNNING
    val dismissState = rememberSwipeToDismissBoxState(
        confirmValueChange = { value ->
            if (value == SwipeToDismissBoxValue.EndToStart && canRemove) {
                onRemoveQueuedBatch(queueItem.id)
                true
            } else {
                false
            }
        }
    )

    SwipeToDismissBox(
        state = dismissState,
        modifier = modifier,
        enableDismissFromStartToEnd = false,
        enableDismissFromEndToStart = canRemove,
        backgroundContent = {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .clip(MaterialTheme.shapes.medium)
                    .background(MaterialTheme.colorScheme.errorContainer),
                contentAlignment = Alignment.CenterEnd
            ) {
                Icon(
                    imageVector = Icons.Outlined.Delete,
                    contentDescription = stringResource(R.string.queue_remove),
                    tint = MaterialTheme.colorScheme.onErrorContainer,
                    modifier = Modifier
                        .padding(end = Spacing.lg)
                        .size(24.dp)
                )
            }
        }
    ) {
        QueueItemCard(
            index = index,
            queueItem = queueItem,
            isProcessing = isProcessing,
            onCancelProcessing = onCancelProcessing,
            onRetryQueuedBatch = onRetryQueuedBatch,
            onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
            onOpenQueuedBatchOutput = onOpenQueuedBatchOutput
        )
    }
}

@Composable
private fun QueueItemCard(
    index: Int,
    queueItem: ProcessingQueueItemSummary,
    isProcessing: Boolean,
    onCancelProcessing: () -> Unit,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit
) {
    val canOpenOutput = queueItem.status == PersistedQueueItemStatus.COMPLETED &&
        !queueItem.lastOutputUri.isNullOrBlank()
    val isFailed = queueItem.status == PersistedQueueItemStatus.FAILED
    val isRunning = queueItem.status == PersistedQueueItemStatus.RUNNING
    val unnamedInputLabel = stringResource(R.string.queue_unnamed_input)
    // Decoding a (possibly multi-source) spec parses JSON and URIs; only
    // recompute when the source itself changes, not on every recomposition.
    val sourceLabel = remember(queueItem.source, unnamedInputLabel) {
        inputSourceLabel(queueItem.source, unnamedInputLabel)
    }
    // Allocation summary resolves 5-6 string resources and formats them;
    // resolve the resources outside remember, then cache the assembled string
    // keyed on the item fields + resolved labels.
    val allocationSummary = rememberAllocationSummary(queueItem)
    val errorText = queueItem.lastErrorMessage

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .pressable(enabled = canOpenOutput) { onOpenQueuedBatchOutput(queueItem.id) },
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainerLowest
    ) {
        Column {
            ListItem(
                leadingContent = { QueueItemThumbnail(source = queueItem.source) },
                headlineContent = {
                    Text(
                        text = "#${index + 1} $sourceLabel",
                        style = valueStyleFor(sourceLabel),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                },
                supportingContent = {
                    Column {
                        Text(
                            text = stringResource(R.string.queue_allocation_label, allocationSummary),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        if (isFailed && !errorText.isNullOrBlank()) {
                            Text(
                                text = errorText,
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.error,
                                maxLines = 2,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    }
                },
                trailingContent = { QueueStatusPill(status = queueItem.status) },
                colors = ListItemDefaults.colors(containerColor = Color.Transparent)
            )

            if (isFailed || canOpenOutput) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(start = Spacing.md, end = Spacing.sm, bottom = Spacing.xs),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    if (isFailed) {
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
                    if (isFailed) {
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
                }
            }

            if (isRunning) {
                // 取消入口在处理队列内：终止当前运行中的批次。
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(start = Spacing.md, end = Spacing.sm, bottom = Spacing.xs),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Spacer(modifier = Modifier.weight(1f))
                    QueueItemAction(
                        iconVector = Icons.Outlined.Close,
                        contentDescriptionRes = R.string.queue_cancel,
                        onClick = onCancelProcessing
                    )
                }
            }

            if (isRunning) {
                LinearProgressIndicator(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(2.dp)
                )
            }
        }
    }
}

@Composable
private fun QueueItemAction(
    iconVector: ImageVector,
    contentDescriptionRes: Int,
    onClick: () -> Unit,
    enabled: Boolean = true
) {
    // 不覆盖尺寸：IconButton 默认保证 48dp 最小触控目标（MD3 无障碍要求）。
    IconButton(onClick = onClick, enabled = enabled) {
        Icon(
            imageVector = iconVector,
            contentDescription = stringResource(contentDescriptionRes),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(18.dp)
        )
    }
}

/** 状态胶囊：M3 容器色角色（中性=variant / 运行=primary / 完成=secondary / 失败=error）。 */
@Composable
private fun QueueStatusPill(status: PersistedQueueItemStatus) {
    val containerColor: Color
    val contentColor: Color
    val labelRes: Int
    when (status) {
        PersistedQueueItemStatus.PENDING -> {
            containerColor = MaterialTheme.colorScheme.surfaceVariant
            contentColor = MaterialTheme.colorScheme.onSurfaceVariant
            labelRes = R.string.queue_status_pending
        }
        PersistedQueueItemStatus.RUNNING -> {
            containerColor = MaterialTheme.colorScheme.primaryContainer
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer
            labelRes = R.string.common_status_processing
        }
        PersistedQueueItemStatus.COMPLETED -> {
            containerColor = MaterialTheme.colorScheme.secondaryContainer
            contentColor = MaterialTheme.colorScheme.onSecondaryContainer
            labelRes = R.string.common_status_completed
        }
        PersistedQueueItemStatus.FAILED -> {
            containerColor = MaterialTheme.colorScheme.errorContainer
            contentColor = MaterialTheme.colorScheme.onErrorContainer
            labelRes = R.string.common_status_failed
        }
    }
    Surface(shape = CircleShape, color = containerColor, contentColor = contentColor) {
        Text(
            text = stringResource(labelRes),
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.padding(horizontal = Spacing.sm, vertical = Spacing.xs)
        )
    }
}

@Composable
private fun QueueItemThumbnail(source: String) {
    val context = LocalContext.current
    val bitmapState by produceState(
        initialValue = ThumbnailResult(bitmap = null, settled = false),
        source
    ) {
        value = ThumbnailResult(
            bitmap = runCatching {
                decodeQueueThumbnailWithLimit(context = context, source = source)
            }.getOrNull(),
            settled = true
        )
    }

    Box(
        modifier = Modifier
            .size(48.dp)
            .clip(MaterialTheme.shapes.small),
        contentAlignment = Alignment.Center
    ) {
        val result = bitmapState
        when {
            // 解码完成前不渲染占位文案，避免「无预览」闪一下再被图片替换。
            !result.settled -> Unit
            result.bitmap != null -> Image(
                bitmap = result.bitmap,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop
            )
            else -> Text(
                text = stringResource(R.string.queue_no_image),
                style = MaterialTheme.typography.labelSmall
            )
        }
    }
}

/**
 * allocation summary 缓存：stringResource 只能在 composable 上下文调用，
 * 因此先在 remember 外解析全部资源串，再用纯函数拼接并按 item 字段缓存，
 * 避免每次重组重复 5-6 次资源查找 + 格式化。
 */
@Composable
private fun rememberAllocationSummary(item: ProcessingQueueItemSummary): String {
    val modeDistributed = stringResource(R.string.queue_mode_distributed)
    val modeStandalone = stringResource(R.string.queue_mode_standalone)
    val expectedCountPattern = stringResource(R.string.queue_expected_count)
    val expectedPending = stringResource(R.string.queue_expected_pending)
    val overwriteOriginalLabel = stringResource(R.string.common_overwrite_original)
    val writeToOutputLabel = stringResource(R.string.queue_write_to_output)
    val backupSuffixLabel = stringResource(R.string.queue_backup_suffix)
    val summaryPattern = stringResource(R.string.queue_allocation_summary)
    return remember(
        item.distributedMode,
        item.expectedItems,
        item.overwriteOriginal,
        item.backupOriginal,
        modeDistributed,
        modeStandalone,
        expectedCountPattern,
        expectedPending,
        overwriteOriginalLabel,
        writeToOutputLabel,
        backupSuffixLabel,
        summaryPattern
    ) {
        val mode = if (item.distributedMode) modeDistributed else modeStandalone
        val expected = item.expectedItems
            ?.let { count -> String.format(expectedCountPattern, count) }
            ?: expectedPending
        val overwrite = if (item.overwriteOriginal) overwriteOriginalLabel else writeToOutputLabel
        val backup = if (item.overwriteOriginal && item.backupOriginal) backupSuffixLabel else ""
        String.format(summaryPattern, mode, expected, overwrite, backup)
    }
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
