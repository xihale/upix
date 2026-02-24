package com.crossscale.ui.screens

import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.ui.components.LayeredSectionCard
import com.crossscale.ui.components.ProcessingProgress
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingCheckpointSummary
import com.crossscale.viewmodel.ProcessingQueueItemStatus
import com.crossscale.viewmodel.ProcessingQueueItemSummary
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
    val selectedOutput = state.selectedOutputDirectoryUri ?: "应用内部目录（默认）"
    val modeLabel = if (state.isDistributedMode) "分布式模式" else "单机模式"

    val checkpointSummary = processingUiState.checkpointSummary
    val checkpointPendingCount = checkpointSummary?.pendingItems ?: 0
    val checkpointFailedCount = checkpointSummary?.failedItems ?: 0
    val checkpointDeferredCount = checkpointSummary?.deferredItems ?: 0
    val continueTaskLabel = if (checkpointSummary != null) "继续任务 ($checkpointPendingCount)" else "继续任务"
    val retryFailedLabel = if (checkpointSummary != null) "仅重试失败项 ($checkpointFailedCount)" else "仅重试失败项"

    val queueItems = processingUiState.queueItems
    val pendingQueueCount = queueItems.count { item -> item.status == ProcessingQueueItemStatus.PENDING }
    val queueCheckpointReadyCount = queueItems.count { item ->
        item.status == ProcessingQueueItemStatus.PENDING && isQueueItemResumeCandidate(item, checkpointSummary)
    }
    val failedQueueCount = queueItems.count { item -> item.status == ProcessingQueueItemStatus.FAILED }
    val queueRunningLabel = if (processingUiState.isQueueRunning) "运行中" else "已暂停"

    var inputDetailsExpanded by rememberSaveable { mutableStateOf(false) }
    var outputDetailsExpanded by rememberSaveable { mutableStateOf(false) }
    var runDetailsExpanded by rememberSaveable { mutableStateOf(false) }
    var queueDetailsExpanded by rememberSaveable { mutableStateOf(false) }
    var checkpointDetailsExpanded by rememberSaveable { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(text = "工作流主页", style = MaterialTheme.typography.headlineSmall)
        Text(
            text = "按 1-4 步完成任务：输入 → 模型与输出 → 执行 → 队列。详细策略都在展开层里。",
            style = MaterialTheme.typography.bodyMedium
        )

        LayeredSectionCard(
            title = "步骤 1 / 选择输入",
            summary = "当前：$selectedInputLabel；预计：$expectedTotalLabel",
            guide = "建议先添加 1 张图片快速验证流程，再批量加入文件夹。输入顺序决定处理顺序。",
            expanded = inputDetailsExpanded,
            onToggleExpanded = { inputDetailsExpanded = !inputDetailsExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onSelectSingleImage, modifier = Modifier.fillMaxWidth()) {
                        Text("添加单图")
                    }
                    OutlinedButton(onClick = onSelectMultipleImages, modifier = Modifier.fillMaxWidth()) {
                        Text("添加多图")
                    }
                    OutlinedButton(onClick = onSelectImageFolder, modifier = Modifier.fillMaxWidth()) {
                        Text("添加文件夹")
                    }
                    OutlinedButton(
                        onClick = onClearInputSources,
                        enabled = selectedInputs.isNotEmpty(),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("清空输入")
                    }
                }
            },
            details = {
                if (selectedInputs.isEmpty()) {
                    Text("尚未选择输入源。", style = MaterialTheme.typography.bodySmall)
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
            title = "步骤 2 / 模型与输出",
            summary = "模型：$selectedModel；输出：$selectedOutput",
            guide = "模型在“模型”页切换。建议首轮使用默认输出目录，确认效果后再切换到外部目录。",
            expanded = outputDetailsExpanded,
            onToggleExpanded = { outputDetailsExpanded = !outputDetailsExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(text = "当前模式：$modeLabel", style = MaterialTheme.typography.bodyMedium)
                    Text(text = "已连接设备：${state.connectedDevices.size}", style = MaterialTheme.typography.bodySmall)
                    OutlinedButton(onClick = onSelectOutputFolder, modifier = Modifier.fillMaxWidth()) {
                        Text("选择输出文件夹")
                    }
                    OutlinedButton(onClick = onUseInternalOutputFolder, modifier = Modifier.fillMaxWidth()) {
                        Text("使用应用内部目录")
                    }
                }
            },
            details = {
                Text(
                    text = "当连接设备数 > 1 时自动进入分布式模式。首次体验建议单机跑通后再启用多设备。",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        )

        LayeredSectionCard(
            title = "步骤 3 / 执行处理",
            summary = if (processingUiState.isProcessing) "任务运行中" else "可立即执行或加入队列",
            guide = "“开始超分”立即执行；“加入队列”适合批量任务。中断后可继续或只重试失败项。",
            expanded = runDetailsExpanded,
            onToggleExpanded = { runDetailsExpanded = !runDetailsExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onStartProcessing, modifier = Modifier.fillMaxWidth()) {
                        Text("开始超分")
                    }
                    OutlinedButton(onClick = onEnqueueBatch, modifier = Modifier.fillMaxWidth()) {
                        Text("加入队列")
                    }
                    if (processingUiState.isProcessing) {
                        OutlinedButton(onClick = onCancelProcessing, modifier = Modifier.fillMaxWidth()) {
                            Text("取消当前任务")
                        }
                    }
                }
            },
            details = {
                if (!processingUiState.isProcessing && processingUiState.canResume) {
                    OutlinedButton(onClick = onResumeProcessing, modifier = Modifier.fillMaxWidth()) {
                        Text(continueTaskLabel)
                    }
                }
                if (!processingUiState.isProcessing && processingUiState.canRetryFailedOnly) {
                    OutlinedButton(onClick = onRetryFailedItems, modifier = Modifier.fillMaxWidth()) {
                        Text(retryFailedLabel)
                    }
                }
                if (!processingUiState.isProcessing && processingUiState.canResume) {
                    OutlinedButton(onClick = onDiscardCheckpoint, modifier = Modifier.fillMaxWidth()) {
                        Text("丢弃续传数据")
                    }
                }
            }
        )

        LayeredSectionCard(
            title = "步骤 4 / 批量队列",
            summary = "状态：$queueRunningLabel；待处理 $pendingQueueCount，失败 $failedQueueCount",
            guide = "队列适合长任务。先将任务加入队列，再启动队列；失败批次支持单独重试。",
            expanded = queueDetailsExpanded,
            onToggleExpanded = { queueDetailsExpanded = !queueDetailsExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(
                        onClick = onStartBatchQueue,
                        enabled = !processingUiState.isProcessing && pendingQueueCount > 0,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("启动队列 ($pendingQueueCount)")
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
                Text(text = "可续传队列项：$queueCheckpointReadyCount", style = MaterialTheme.typography.bodySmall)
                if (queueItems.isEmpty()) {
                    Text(text = "队列为空。", style = MaterialTheme.typography.bodySmall)
                } else {
                    queueItems.forEachIndexed { index, queueItem ->
                        QueueItemBlock(
                            index = index,
                            queueItem = queueItem,
                            checkpointSummary = checkpointSummary,
                            isProcessing = processingUiState.isProcessing,
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
                summary = "状态：${checkpointSummary.statusLabel}；待处理 ${checkpointSummary.pendingItems}；失败 ${checkpointSummary.failedItems}",
                guide = "任务中断后可从这里恢复。默认“继续任务”会处理全部待处理项。",
                expanded = checkpointDetailsExpanded,
                onToggleExpanded = { checkpointDetailsExpanded = !checkpointDetailsExpanded },
                actions = {
                    Text(
                        text = "进度：${checkpointSummary.completedItems}/${checkpointSummary.totalItems}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = "更新时间：${formatCheckpointUpdatedAt(checkpointSummary.updatedAtEpochMs)}",
                        style = MaterialTheme.typography.bodySmall
                    )
                },
                details = {
                    CheckpointDetailBlock(
                        checkpointSummary = checkpointSummary,
                        checkpointPendingCount = checkpointPendingCount,
                        checkpointFailedCount = checkpointFailedCount,
                        checkpointDeferredCount = checkpointDeferredCount,
                        canRetryFailedOnly = processingUiState.canRetryFailedOnly,
                        onResumePendingItem = onResumePendingItem,
                        onRetryFailedItem = onRetryFailedItem
                    )
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
            maxLines = 2,
            overflow = TextOverflow.Ellipsis
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onMoveUp, enabled = !isFirst) {
                Text("上移")
            }
            OutlinedButton(onClick = onMoveDown, enabled = !isLast) {
                Text("下移")
            }
            OutlinedButton(onClick = onRemove) {
                Text("移除")
            }
        }
    }
}

@Composable
private fun QueueItemBlock(
    index: Int,
    queueItem: ProcessingQueueItemSummary,
    checkpointSummary: ProcessingCheckpointSummary?,
    isProcessing: Boolean,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit
) {
    val queueItemStatus = when (queueItem.status) {
        ProcessingQueueItemStatus.PENDING -> "待处理"
        ProcessingQueueItemStatus.RUNNING -> "处理中"
        ProcessingQueueItemStatus.FAILED -> "失败"
    }
    val resumeCandidate = queueItem.status == ProcessingQueueItemStatus.PENDING &&
        isQueueItemResumeCandidate(queueItem, checkpointSummary)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(text = "${index + 1}. ${inputSourceSummaryLabel(queueItem.source)}", style = MaterialTheme.typography.bodyMedium)
        Text(text = "模型: ${queueItem.modelName}；状态: $queueItemStatus", style = MaterialTheme.typography.bodySmall)

        queueItem.expectedItems?.let { expectedItems ->
            Text(text = "预计图片: $expectedItems 张；尝试次数: ${queueItem.attempts}", style = MaterialTheme.typography.bodySmall)
        } ?: Text(text = "预计图片: 统计中；尝试次数: ${queueItem.attempts}", style = MaterialTheme.typography.bodySmall)

        if (queueItem.retryFailedOnly) {
            Text(text = "执行策略: 仅重试失败项", style = MaterialTheme.typography.bodySmall)
        }

        if (resumeCandidate && checkpointSummary != null) {
            val checkpointId = checkpointSummary.taskId.take(8)
            val failedInfo = if (checkpointSummary.failedItems > 0) "，失败 ${checkpointSummary.failedItems} 项" else ""
            Text(
                text = "命中断点任务($checkpointId)，将从 ${checkpointSummary.pendingItems} 项继续$failedInfo",
                style = MaterialTheme.typography.bodySmall
            )
        }

        queueItem.lastErrorMessage?.takeIf { message -> message.isNotBlank() }?.let { message ->
            Text(text = "错误: $message", style = MaterialTheme.typography.bodySmall)
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (queueItem.status == ProcessingQueueItemStatus.FAILED) {
                OutlinedButton(onClick = { onRetryQueuedBatch(queueItem.id) }, enabled = !isProcessing) {
                    Text("重试批次")
                }
                val canRetryFailedOnly = checkpointSummary != null &&
                    checkpointSummary.failedItems > 0 &&
                    isQueueItemResumeCandidate(queueItem, checkpointSummary)
                if (canRetryFailedOnly) {
                    OutlinedButton(onClick = { onRetryQueuedBatchFailedOnly(queueItem.id) }, enabled = !isProcessing) {
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

@Composable
private fun CheckpointDetailBlock(
    checkpointSummary: ProcessingCheckpointSummary,
    checkpointPendingCount: Int,
    checkpointFailedCount: Int,
    checkpointDeferredCount: Int,
    canRetryFailedOnly: Boolean,
    onResumePendingItem: (String) -> Unit,
    onRetryFailedItem: (String) -> Unit
) {
    val checkpointPendingDetails = checkpointSummary.pendingItemDetails
    val checkpointPreviewLimit = 20
    val checkpointPreviewItems = checkpointPendingDetails.take(checkpointPreviewLimit)
    val checkpointHiddenCount = (checkpointPendingDetails.size - checkpointPreviewItems.size).coerceAtLeast(0)

    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(text = "模型: ${checkpointSummary.modelName}", style = MaterialTheme.typography.bodySmall)
        Text(text = "输入: ${inputSourceLabel(checkpointSummary.source)}", style = MaterialTheme.typography.bodySmall)
        Text(text = "输出: ${checkpointSummary.outputDirectory}", style = MaterialTheme.typography.bodySmall)
        Text(text = "继续任务将处理: $checkpointPendingCount 项", style = MaterialTheme.typography.bodySmall)
        if (canRetryFailedOnly) {
            Text(
                text = "仅重试失败项将处理: $checkpointFailedCount 项（保留 $checkpointDeferredCount 项待续）",
                style = MaterialTheme.typography.bodySmall
            )
        }
        checkpointSummary.lastErrorMessage?.takeIf { message -> message.isNotBlank() }?.let { message ->
            Text(text = "最近错误: $message", style = MaterialTheme.typography.bodySmall)
        }

        if (checkpointPreviewItems.isNotEmpty()) {
            Text(text = "待续文件（失败项优先）", style = MaterialTheme.typography.titleSmall)
            checkpointPreviewItems.forEach { pendingItem ->
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 6.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    val itemStatus = if (pendingItem.isFailed) "失败待重试" else "待继续"
                    Text(text = pendingItem.displayName, style = MaterialTheme.typography.bodyMedium)
                    Text(
                        text = "状态: $itemStatus；尝试次数: ${pendingItem.attempts}",
                        style = MaterialTheme.typography.bodySmall
                    )
                    pendingItem.lastErrorMessage?.takeIf { message -> message.isNotBlank() }?.let { message ->
                        Text(text = "错误: $message", style = MaterialTheme.typography.bodySmall)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
            if (checkpointHiddenCount > 0) {
                Text(text = "其余 $checkpointHiddenCount 项未展示，可继续任务统一处理", style = MaterialTheme.typography.bodySmall)
            }
        }
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
