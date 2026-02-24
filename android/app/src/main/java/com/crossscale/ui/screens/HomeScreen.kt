package com.crossscale.ui.screens

import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.ui.components.ProcessingProgress
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingCheckpointSummary
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingQueueItemStatus
import com.crossscale.viewmodel.ProcessingUiState
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

@Composable
fun HomeScreen(
    state: MainUiState,
    processingUiState: ProcessingUiState,
    onSelectSingleImage: () -> Unit,
    onSelectMultipleImages: () -> Unit,
    onSelectImageFolder: () -> Unit,
    onClearInputSources: () -> Unit,
    onRemoveInputSource: (String) -> Unit,
    onMoveInputSourceUp: (String) -> Unit,
    onMoveInputSourceDown: (String) -> Unit,
    onSelectOutputFolder: () -> Unit,
    onUseInternalOutputFolder: () -> Unit,
    onStartProcessing: () -> Unit,
    onEnqueueBatch: () -> Unit,
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
    val selectedModel = state.selectedModel?.name ?: "未选择"
    val selectedInputs = state.selectedInputSources
    val selectedInputLabel = when (selectedInputs.size) {
        0 -> "未选择"
        1 -> inputSourceLabel(selectedInputs.first())
        else -> "${selectedInputs.size} 个输入源"
    }
    val expectedTotalLabel = when {
        selectedInputs.isEmpty() -> "未选择输入"
        processingUiState.isEstimatingInputCount -> "统计中..."
        processingUiState.expectedTotalItems == null -> "未统计"
        else -> "${processingUiState.expectedTotalItems} 张"
    }
    val checkpointSummary = processingUiState.checkpointSummary
    val checkpointPendingCount = checkpointSummary?.pendingItems ?: 0
    val checkpointFailedCount = checkpointSummary?.failedItems ?: 0
    val checkpointDeferredCount = checkpointSummary?.deferredItems ?: 0
    val checkpointPendingDetails = checkpointSummary?.pendingItemDetails.orEmpty()
    val checkpointPreviewLimit = 20
    val checkpointPreviewItems = checkpointPendingDetails.take(checkpointPreviewLimit)
    val checkpointHiddenCount = (checkpointPendingDetails.size - checkpointPreviewItems.size).coerceAtLeast(0)
    val continueTaskLabel = if (checkpointSummary != null) {
        "继续任务 ($checkpointPendingCount)"
    } else {
        "继续任务"
    }
    val retryFailedLabel = if (checkpointSummary != null) {
        "仅重试失败项 ($checkpointFailedCount)"
    } else {
        "仅重试失败项"
    }
    val selectedOutput = state.selectedOutputDirectoryUri ?: "应用内部目录（默认）"
    val modeLabel = if (state.isDistributedMode) "分布式模式" else "单机模式"
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = queueItems.count { item ->
        item.status == ProcessingQueueItemStatus.PENDING
    }
    val queueCheckpointReadyCount = queueItems.count { item ->
        item.status == ProcessingQueueItemStatus.PENDING &&
            isQueueItemResumeCandidate(item, checkpointSummary)
    }
    val failedQueueCount = queueItems.count { item ->
        item.status == ProcessingQueueItemStatus.FAILED
    }
    val queueRunningLabel = if (processingUiState.isQueueRunning) "运行中" else "已暂停"
    val scrollState = rememberScrollState()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(scrollState)
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            text = "主页",
            style = MaterialTheme.typography.headlineMedium
        )

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(text = "当前模式: $modeLabel", style = MaterialTheme.typography.titleMedium)
                Text(text = "已连接设备: ${state.connectedDevices.size}", style = MaterialTheme.typography.bodyMedium)
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(text = "当前模型: $selectedModel", style = MaterialTheme.typography.titleSmall)
                Text(text = "输入源: $selectedInputLabel", style = MaterialTheme.typography.bodySmall)
                Text(text = "预计图片: $expectedTotalLabel", style = MaterialTheme.typography.bodySmall)
                Text(text = "输出目录: $selectedOutput", style = MaterialTheme.typography.bodySmall)
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onSelectSingleImage) {
                Text("添加单图")
            }
            OutlinedButton(onClick = onSelectMultipleImages) {
                Text("添加多图")
            }
            OutlinedButton(onClick = onSelectImageFolder) {
                Text("添加文件夹")
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onClearInputSources) {
                Text("清空输入")
            }
        }

        if (selectedInputs.isNotEmpty()) {
            Text(
                text = "已选输入源列表",
                style = MaterialTheme.typography.titleSmall
            )
            Column(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                selectedInputs.forEachIndexed { index, source ->
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(12.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(8.dp)
                            ) {
                                Column(modifier = Modifier.weight(1.0f)) {
                                    Text(
                                        text = "${index + 1}. ${inputSourceLabel(source)}",
                                        style = MaterialTheme.typography.bodyMedium
                                    )
                                    Text(
                                        text = source,
                                        style = MaterialTheme.typography.bodySmall,
                                        maxLines = 2,
                                        overflow = TextOverflow.Ellipsis
                                    )
                                }
                            }
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(8.dp)
                            ) {
                                OutlinedButton(
                                    onClick = { onMoveInputSourceUp(source) },
                                    enabled = index > 0
                                ) {
                                    Text("上移")
                                }
                                OutlinedButton(
                                    onClick = { onMoveInputSourceDown(source) },
                                    enabled = index < selectedInputs.lastIndex
                                ) {
                                    Text("下移")
                                }
                                OutlinedButton(onClick = { onRemoveInputSource(source) }) {
                                    Text("移除")
                                }
                            }
                        }
                    }
                }
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onSelectOutputFolder) {
                Text("选择输出文件夹")
            }
            OutlinedButton(onClick = onUseInternalOutputFolder) {
                Text("用应用内部目录")
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = onStartProcessing) {
                Text("开始超分")
            }
            OutlinedButton(onClick = onEnqueueBatch) {
                Text("加入队列")
            }
            if (!processingUiState.isProcessing && processingUiState.canResume) {
                OutlinedButton(onClick = onResumeProcessing) {
                    Text(continueTaskLabel)
                }
            }
            if (!processingUiState.isProcessing && processingUiState.canRetryFailedOnly) {
                OutlinedButton(onClick = onRetryFailedItems) {
                    Text(retryFailedLabel)
                }
            }
            if (!processingUiState.isProcessing && processingUiState.canResume) {
                OutlinedButton(onClick = onDiscardCheckpoint) {
                    Text("丢弃续传")
                }
            }
            if (processingUiState.isProcessing) {
                OutlinedButton(onClick = onCancelProcessing) {
                    Text("取消")
                }
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = onStartBatchQueue,
                enabled = !processingUiState.isProcessing && pendingQueueCount > 0
            ) {
                Text("启动队列($pendingQueueCount)")
            }
            OutlinedButton(
                onClick = onPauseBatchQueue,
                enabled = processingUiState.isQueueRunning
            ) {
                Text("暂停队列")
            }
            OutlinedButton(
                onClick = onClearBatchQueue,
                enabled = queueItems.isNotEmpty()
            ) {
                Text("清空队列")
            }
        }

        if (queueItems.isNotEmpty()) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Text(
                        text = "批量任务队列",
                        style = MaterialTheme.typography.titleSmall
                    )
                    Text(
                        text = "状态: $queueRunningLabel",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "待处理: $pendingQueueCount，可续传: $queueCheckpointReadyCount，失败: $failedQueueCount，总计: ${queueItems.size}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    queueItems.forEachIndexed { index, queueItem ->
                        Card(modifier = Modifier.fillMaxWidth()) {
                            Column(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(12.dp),
                                verticalArrangement = Arrangement.spacedBy(4.dp)
                            ) {
                                val queueItemStatus = when (queueItem.status) {
                                    ProcessingQueueItemStatus.PENDING -> "待处理"
                                    ProcessingQueueItemStatus.RUNNING -> "处理中"
                                    ProcessingQueueItemStatus.FAILED -> "失败"
                                }
                                Text(
                                    text = "${index + 1}. ${inputSourceSummaryLabel(queueItem.source)}",
                                    style = MaterialTheme.typography.bodyMedium
                                )
                                Text(
                                    text = "模型: ${queueItem.modelName}，状态: $queueItemStatus",
                                    style = MaterialTheme.typography.bodySmall
                                )
                                if (queueItem.retryFailedOnly) {
                                    Text(
                                        text = "执行策略: 仅重试失败项",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                }
                                queueItem.expectedItems?.let { expectedItems ->
                                    Text(
                                        text = "预计图片: $expectedItems 张，尝试次数: ${queueItem.attempts}",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                } ?: Text(
                                    text = "预计图片: 统计中，尝试次数: ${queueItem.attempts}",
                                    style = MaterialTheme.typography.bodySmall
                                )
                                val resumeCandidate = queueItem.status == ProcessingQueueItemStatus.PENDING &&
                                    isQueueItemResumeCandidate(queueItem, checkpointSummary)
                                if (resumeCandidate && checkpointSummary != null) {
                                    val checkpointId = checkpointSummary.taskId.take(8)
                                    val failedInfo = if (checkpointSummary.failedItems > 0) {
                                        "，失败 ${checkpointSummary.failedItems} 项"
                                    } else {
                                        ""
                                    }
                                    Text(
                                        text = "命中断点任务($checkpointId)，将从 ${checkpointSummary.pendingItems} 项继续$failedInfo",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                }
                                queueItem.lastErrorMessage?.takeIf { message -> message.isNotBlank() }?.let { message ->
                                    Text(
                                        text = "错误: $message",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                }
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                                ) {
                                    if (queueItem.status == ProcessingQueueItemStatus.FAILED) {
                                        OutlinedButton(
                                            onClick = { onRetryQueuedBatch(queueItem.id) },
                                            enabled = !processingUiState.isProcessing
                                        ) {
                                            Text("重试批次")
                                        }
                                        val canRetryFailedOnly = checkpointSummary != null &&
                                            checkpointSummary.failedItems > 0 &&
                                            isQueueItemResumeCandidate(queueItem, checkpointSummary)
                                        if (canRetryFailedOnly) {
                                            OutlinedButton(
                                                onClick = { onRetryQueuedBatchFailedOnly(queueItem.id) },
                                                enabled = !processingUiState.isProcessing
                                            ) {
                                                Text("仅重试失败项")
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
                        }
                    }
                }
            }
        }

        if (!processingUiState.isProcessing && checkpointSummary != null) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Text(
                        text = "断点续传任务",
                        style = MaterialTheme.typography.titleSmall
                    )
                    Text(
                        text = "状态: ${checkpointSummary.statusLabel}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = "进度: ${checkpointSummary.completedItems}/${checkpointSummary.totalItems}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "待处理: ${checkpointSummary.pendingItems}，失败: ${checkpointSummary.failedItems}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "继续任务将处理: $checkpointPendingCount 项",
                        style = MaterialTheme.typography.bodySmall
                    )
                    if (processingUiState.canRetryFailedOnly) {
                        Text(
                            text = "仅重试失败项将处理: $checkpointFailedCount 项（保留 $checkpointDeferredCount 项待续）",
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                    Text(
                        text = "模型: ${checkpointSummary.modelName}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "输入: ${inputSourceLabel(checkpointSummary.source)}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "输出: ${checkpointSummary.outputDirectory}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "更新时间: ${formatCheckpointUpdatedAt(checkpointSummary.updatedAtEpochMs)}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    checkpointSummary.lastErrorMessage?.takeIf { message -> message.isNotBlank() }?.let { message ->
                        Text(
                            text = "最近错误: $message",
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                    if (checkpointPreviewItems.isNotEmpty()) {
                        Text(
                            text = "待续文件（失败项优先）",
                            style = MaterialTheme.typography.titleSmall
                        )
                        checkpointPreviewItems.forEach { pendingItem ->
                            Card(modifier = Modifier.fillMaxWidth()) {
                                Column(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(12.dp),
                                    verticalArrangement = Arrangement.spacedBy(4.dp)
                                ) {
                                    val itemStatus = if (pendingItem.isFailed) "失败待重试" else "待继续"
                                    Text(
                                        text = pendingItem.displayName,
                                        style = MaterialTheme.typography.bodyMedium
                                    )
                                    Text(
                                        text = "状态: $itemStatus，尝试次数: ${pendingItem.attempts}",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                    pendingItem.lastErrorMessage?.takeIf { message -> message.isNotBlank() }?.let { message ->
                                        Text(
                                            text = "错误: $message",
                                            style = MaterialTheme.typography.bodySmall
                                        )
                                    }
                                    Row(
                                        modifier = Modifier.fillMaxWidth(),
                                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                                    ) {
                                        OutlinedButton(onClick = { onResumePendingItem(pendingItem.uri) }) {
                                            Text("继续此项")
                                        }
                                        if (pendingItem.isFailed) {
                                            OutlinedButton(onClick = { onRetryFailedItem(pendingItem.uri) }) {
                                                Text("重试此项")
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (checkpointHiddenCount > 0) {
                            Text(
                                text = "其余 $checkpointHiddenCount 项未展示，可继续任务统一处理",
                                style = MaterialTheme.typography.bodySmall
                            )
                        }
                    }
                }
            }
        }

        ProcessingProgress(uiState = processingUiState)
    }
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

private fun inputSourceSummaryLabel(source: String): String {
    val decodedSources = InputSourceSpec.decode(source)
    if (decodedSources != null) {
        return when (decodedSources.size) {
            0 -> "空输入源"
            1 -> inputSourceLabel(decodedSources.first())
            else -> "${decodedSources.size} 个输入源"
        }
    }
    return inputSourceLabel(source)
}

private fun formatCheckpointUpdatedAt(updatedAtEpochMs: Long): String {
    if (updatedAtEpochMs <= 0L) {
        return "未知"
    }
    val localDateTime = Instant.ofEpochMilli(updatedAtEpochMs)
        .atZone(ZoneId.systemDefault())
        .toLocalDateTime()
    return CHECKPOINT_TIME_FORMATTER.format(localDateTime)
}

private fun isQueueItemResumeCandidate(
    queueItem: ProcessingQueueItemSummary,
    checkpointSummary: ProcessingCheckpointSummary?
): Boolean {
    if (checkpointSummary == null || checkpointSummary.pendingItems <= 0) {
        return false
    }
    val sameSource = checkpointSummary.source.trim() == queueItem.source.trim()
    val sameModel = checkpointSummary.modelName.trim() == queueItem.modelName.trim()
    val sameMode = checkpointSummary.distributedMode == queueItem.distributedMode
    val checkpointOutputUri = checkpointSummary.outputDirectoryUri?.trim().orEmpty()
    val queueOutputUri = queueItem.outputDirectoryUri?.trim().orEmpty()
    return sameSource && sameModel && sameMode && checkpointOutputUri == queueOutputUri
}

private val CHECKPOINT_TIME_FORMATTER: DateTimeFormatter =
    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss", Locale.getDefault())
