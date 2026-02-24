package com.crossscale.ui.screens

import android.graphics.BitmapFactory
import android.net.Uri
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.ui.components.LayeredSectionCard
import com.crossscale.ui.components.ProcessingProgress
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingCheckpointSummary
import com.crossscale.viewmodel.ProcessingQueueItemStatus
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingUiState

private enum class QueueViewMode {
    SIMPLE,
    DETAILED
}

@Composable
fun HomeScreen(
    state: MainUiState,
    processingUiState: ProcessingUiState,
    onSelectMultipleImages: () -> Unit,
    onSelectImageFolder: () -> Unit,
    onClearInputSources: () -> Unit,
    onRemoveInputSource: (String) -> Unit,
    onMoveInputSourceUp: (String) -> Unit,
    onMoveInputSourceDown: (String) -> Unit,
    onSelectOutputFolder: () -> Unit,
    onUseDefaultOutputFolder: () -> Unit,
    onToggleOverwriteOriginal: (Boolean) -> Unit,
    onToggleBackupOriginal: (Boolean) -> Unit,
    onStartProcessing: () -> Unit,
    onStartBatchQueue: () -> Unit,
    onPauseBatchQueue: () -> Unit,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit,
    onClearBatchQueue: () -> Unit,
    onResumeProcessing: () -> Unit,
    onRetryFailedItems: () -> Unit,
    onResumePendingItem: (String) -> Unit,
    onRetryFailedItem: (String) -> Unit,
    onDiscardCheckpoint: () -> Unit,
    onCancelProcessing: () -> Unit
) {
    val selectedInputs = state.selectedInputSources
    val selectedInputLabel = if (selectedInputs.isEmpty()) "未选择" else "${selectedInputs.size} 项"
    val selectedModel = state.selectedModel?.name ?: "未选择"
    val selectedOutput = state.selectedOutputDirectoryUri ?: "Picture/CrossScale（默认）"
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = queueItems.count { item -> item.status == ProcessingQueueItemStatus.PENDING }
    val failedQueueCount = queueItems.count { item -> item.status == ProcessingQueueItemStatus.FAILED }
    val queueRunningLabel = if (processingUiState.isQueueRunning) "运行中" else "已暂停"
    val checkpointSummary = processingUiState.checkpointSummary

    var inputExpanded by rememberSaveable { mutableStateOf(false) }
    var outputExpanded by rememberSaveable { mutableStateOf(false) }
    var queueExpanded by rememberSaveable { mutableStateOf(false) }
    var checkpointExpanded by rememberSaveable { mutableStateOf(false) }
    var queueViewMode by rememberSaveable { mutableStateOf(QueueViewMode.SIMPLE) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        LayeredSectionCard(
            title = "输入图片",
            summary = "当前：$selectedInputLabel",
            expanded = inputExpanded,
            onToggleExpanded = { inputExpanded = !inputExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onSelectMultipleImages, modifier = Modifier.fillMaxWidth()) {
                        Text("添加图片")
                    }
                    OutlinedButton(onClick = onSelectImageFolder, modifier = Modifier.fillMaxWidth()) {
                        Text("添加文件夹")
                    }
                    OutlinedButton(
                        onClick = onClearInputSources,
                        enabled = selectedInputs.isNotEmpty(),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("清空")
                    }
                }
            },
            details = {
                if (selectedInputs.isEmpty()) {
                    Text("尚未添加输入。", style = MaterialTheme.typography.bodySmall)
                } else {
                    selectedInputs.forEachIndexed { index, source ->
                        InputSourceItem(
                            index = index,
                            source = source,
                            isFirst = index == 0,
                            isLast = index == selectedInputs.lastIndex,
                            onMoveUp = { onMoveInputSourceUp(source) },
                            onMoveDown = { onMoveInputSourceDown(source) },
                            onRemove = { onRemoveInputSource(source) }
                        )
                    }
                }
            }
        )

        LayeredSectionCard(
            title = "模型与输出",
            summary = "模型：$selectedModel；输出：$selectedOutput",
            expanded = outputExpanded,
            onToggleExpanded = { outputExpanded = !outputExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onSelectOutputFolder, modifier = Modifier.fillMaxWidth()) {
                        Text("选择输出文件夹")
                    }
                    OutlinedButton(onClick = onUseDefaultOutputFolder, modifier = Modifier.fillMaxWidth()) {
                        Text("使用默认输出目录")
                    }
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text("覆盖原图片")
                        Switch(
                            checked = state.overwriteOriginalImage,
                            onCheckedChange = onToggleOverwriteOriginal
                        )
                    }
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text("备份原图片（默认）")
                        Switch(
                            checked = state.backupOriginalImage,
                            onCheckedChange = onToggleBackupOriginal,
                            enabled = state.overwriteOriginalImage
                        )
                    }
                }
            },
            details = {
                Text("开始超分会先加入队列，再自动启动队列。", style = MaterialTheme.typography.bodySmall)
            }
        )

        LayeredSectionCard(
            title = "执行",
            summary = if (processingUiState.isProcessing) "正在处理" else "待执行",
            expanded = false,
            onToggleExpanded = {},
            showDetailsToggle = false,
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onStartProcessing, modifier = Modifier.fillMaxWidth()) {
                        Text("开始超分")
                    }
                    if (processingUiState.isProcessing) {
                        OutlinedButton(onClick = onCancelProcessing, modifier = Modifier.fillMaxWidth()) {
                            Text("取消当前任务")
                        }
                    }
                }
            },
            details = {}
        )

        LayeredSectionCard(
            title = "队列",
            summary = "状态：$queueRunningLabel；待处理 $pendingQueueCount；失败 $failedQueueCount",
            expanded = queueExpanded,
            onToggleExpanded = { queueExpanded = !queueExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = { queueViewMode = QueueViewMode.SIMPLE }) {
                            Text("简单视图")
                        }
                        OutlinedButton(onClick = { queueViewMode = QueueViewMode.DETAILED }) {
                            Text("详细视图")
                        }
                    }
                    OutlinedButton(
                        onClick = onStartBatchQueue,
                        enabled = !processingUiState.isProcessing && pendingQueueCount > 0,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("启动队列")
                    }
                    OutlinedButton(
                        onClick = onPauseBatchQueue,
                        enabled = processingUiState.isQueueRunning,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("暂停队列")
                    }
                    OutlinedButton(
                        onClick = onClearBatchQueue,
                        enabled = queueItems.isNotEmpty(),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("清空队列")
                    }
                }
            },
            details = {
                if (queueItems.isEmpty()) {
                    Text("队列为空。", style = MaterialTheme.typography.bodySmall)
                } else {
                    queueItems.forEachIndexed { index, queueItem ->
                        QueueItemRow(
                            index = index,
                            queueItem = queueItem,
                            isProcessing = processingUiState.isProcessing,
                            viewMode = queueViewMode,
                            onRetryQueuedBatch = onRetryQueuedBatch,
                            onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                            onRemoveQueuedBatch = onRemoveQueuedBatch
                        )
                    }
                }
            }
        )

        if (!processingUiState.isProcessing && checkpointSummary != null) {
            LayeredSectionCard(
                title = "断点续传",
                summary = "${checkpointSummary.statusLabel}；待处理 ${checkpointSummary.pendingItems}；失败 ${checkpointSummary.failedItems}",
                expanded = checkpointExpanded,
                onToggleExpanded = { checkpointExpanded = !checkpointExpanded },
                actions = {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = onResumeProcessing) { Text("继续任务") }
                        OutlinedButton(onClick = onRetryFailedItems, enabled = processingUiState.canRetryFailedOnly) {
                            Text("重试失败")
                        }
                        OutlinedButton(onClick = onDiscardCheckpoint) { Text("丢弃") }
                    }
                },
                details = {
                    checkpointSummary.pendingItemDetails.take(12).forEach { item ->
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = item.displayName,
                                modifier = Modifier.weight(1f),
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = MaterialTheme.typography.bodySmall
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                OutlinedButton(onClick = { onResumePendingItem(item.uri) }) {
                                    Text("继续")
                                }
                                if (item.isFailed) {
                                    OutlinedButton(onClick = { onRetryFailedItem(item.uri) }) {
                                        Text("重试")
                                    }
                                }
                            }
                        }
                    }
                }
            )
        }

        ProcessingProgress(uiState = processingUiState)
    }
}

@Composable
private fun InputSourceItem(
    index: Int,
    source: String,
    isFirst: Boolean,
    isLast: Boolean,
    onMoveUp: () -> Unit,
    onMoveDown: () -> Unit,
    onRemove: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(text = "${index + 1}. ${inputSourceLabel(source)}", style = MaterialTheme.typography.bodyMedium)
        Text(
            text = source,
            style = MaterialTheme.typography.bodySmall,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onMoveUp, enabled = !isFirst) { Text("上移") }
            OutlinedButton(onClick = onMoveDown, enabled = !isLast) { Text("下移") }
            OutlinedButton(onClick = onRemove) { Text("移除") }
        }
    }
}

@Composable
private fun QueueItemRow(
    index: Int,
    queueItem: ProcessingQueueItemSummary,
    isProcessing: Boolean,
    viewMode: QueueViewMode,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit
) {
    val queueItemStatus = when (queueItem.status) {
        ProcessingQueueItemStatus.PENDING -> "待处理"
        ProcessingQueueItemStatus.RUNNING -> "处理中"
        ProcessingQueueItemStatus.FAILED -> "失败"
    }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        if (viewMode == QueueViewMode.DETAILED) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                QueueItemThumbnail(source = queueItem.source)
                Column(modifier = Modifier.weight(1f)) {
                    Text(text = "#${index + 1} ${inputSourceLabel(queueItem.source)}", style = MaterialTheme.typography.bodyMedium)
                    Text(
                        text = "分配：${allocationSummary(queueItem)}",
                        style = MaterialTheme.typography.bodySmall,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        } else {
            Text(text = "#${index + 1}", style = MaterialTheme.typography.bodyMedium)
            Text(text = "分配：${allocationSummary(queueItem)}", style = MaterialTheme.typography.bodySmall)
        }

        Text(text = "状态：$queueItemStatus", style = MaterialTheme.typography.bodySmall)

        if (queueItem.status == ProcessingQueueItemStatus.FAILED) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { onRetryQueuedBatch(queueItem.id) }, enabled = !isProcessing) {
                    Text("重试")
                }
                OutlinedButton(onClick = { onRetryQueuedBatchFailedOnly(queueItem.id) }, enabled = !isProcessing) {
                    Text("仅失败项")
                }
            }
        }
        OutlinedButton(
            onClick = { onRemoveQueuedBatch(queueItem.id) },
            enabled = queueItem.status != ProcessingQueueItemStatus.RUNNING
        ) {
            Text("移除")
        }
    }
}

@Composable
private fun QueueItemThumbnail(source: String) {
    val context = LocalContext.current
    val bitmapState by produceState<ImageBitmap?>(initialValue = null, source) {
        value = runCatching {
            val decoded = Uri.parse(source)
            context.contentResolver.openInputStream(decoded)?.use { stream ->
                BitmapFactory.decodeStream(stream)?.asImageBitmap()
            }
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
            Text(text = "无图", style = MaterialTheme.typography.bodySmall)
        }
    }
}

private fun allocationSummary(item: ProcessingQueueItemSummary): String {
    val mode = if (item.distributedMode) "分布式" else "单机"
    val expected = item.expectedItems?.let { count -> "$count 张" } ?: "待统计"
    val overwrite = if (item.overwriteOriginal) "覆盖原图" else "写入输出目录"
    val backup = if (item.overwriteOriginal && item.backupOriginal) "（备份）" else ""
    return "$mode / 预计 $expected / $overwrite$backup"
}

private fun inputSourceLabel(source: String): String {
    if (source.isBlank()) {
        return "未命名输入"
    }
    val parsedUri = runCatching { Uri.parse(source) }.getOrNull()
    val lastSegment = parsedUri?.lastPathSegment
    if (!lastSegment.isNullOrBlank()) {
        return lastSegment
    }
    val fallback = source.substringAfterLast('/', source)
    return if (fallback.isBlank()) source else fallback
}
