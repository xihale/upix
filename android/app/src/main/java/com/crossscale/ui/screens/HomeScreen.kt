package com.crossscale.ui.screens

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
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
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.ui.components.LayeredSectionCard
import com.crossscale.ui.components.ProcessingProgress
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingCheckpointSummary
import com.crossscale.viewmodel.ProcessingQueueItemStatus
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingUiState
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

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
    onOpenQueuedBatchOutput: (String) -> Unit,
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
    val selectedOutput = state.selectedOutputDirectoryUri ?: "/sdcard/Pictures/CrossScale"
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = queueItems.count { item -> item.status == ProcessingQueueItemStatus.PENDING }
    val failedQueueCount = queueItems.count { item -> item.status == ProcessingQueueItemStatus.FAILED }
    val queueRunningLabel = if (processingUiState.isQueueRunning) "运行中" else "已暂停"
    val checkpointSummary = processingUiState.checkpointSummary

    var inputExpanded by rememberSaveable { mutableStateOf(false) }
    var outputExpanded by rememberSaveable { mutableStateOf(false) }
    var checkpointExpanded by rememberSaveable { mutableStateOf(false) }

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
                Text("默认输出至 /sdcard/Pictures/CrossScale。", style = MaterialTheme.typography.bodySmall)
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
            summary = "状态：$queueRunningLabel；未开始 $pendingQueueCount；失败 $failedQueueCount",
            expanded = true,
            onToggleExpanded = {},
            showDetailsToggle = false,
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        OutlinedButton(
                            onClick = onStartBatchQueue,
                            enabled = !processingUiState.isProcessing && pendingQueueCount > 0,
                            modifier = Modifier.weight(1f)
                        ) {
                            Text("启动队列")
                        }
                        OutlinedButton(
                            onClick = onPauseBatchQueue,
                            enabled = processingUiState.isQueueRunning,
                            modifier = Modifier.weight(1f)
                        ) {
                            Text("暂停队列")
                        }
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
                    LazyColumn(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(max = 260.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
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
                        }
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
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = onResumeProcessing, modifier = Modifier.fillMaxWidth()) {
                            Text("继续任务")
                        }
                        OutlinedButton(
                            onClick = onRetryFailedItems,
                            enabled = processingUiState.canRetryFailedOnly,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("重试失败")
                        }
                        OutlinedButton(onClick = onDiscardCheckpoint, modifier = Modifier.fillMaxWidth()) {
                            Text("丢弃")
                        }
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
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit
) {
    val queueItemStatus = when (queueItem.status) {
        ProcessingQueueItemStatus.PENDING -> "未开始"
        ProcessingQueueItemStatus.RUNNING -> "处理中"
        ProcessingQueueItemStatus.FAILED -> "失败"
        ProcessingQueueItemStatus.COMPLETED -> "已完成"
    }
    val canOpenOutput = queueItem.status == ProcessingQueueItemStatus.COMPLETED &&
        !queueItem.lastOutputUri.isNullOrBlank()
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = canOpenOutput) {
                onOpenQueuedBatchOutput(queueItem.id)
            }
            .padding(vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
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
        if (canOpenOutput) {
            OutlinedButton(onClick = { onOpenQueuedBatchOutput(queueItem.id) }) {
                Text("打开结果")
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(
                onClick = { onRemoveQueuedBatch(queueItem.id) },
                enabled = queueItem.status != ProcessingQueueItemStatus.RUNNING
            ) {
                Text("移除")
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
        value = withContext(Dispatchers.IO) {
            runCatching {
                decodeQueueThumbnail(context = context, source = source)
            }.getOrNull()
        }
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

private fun decodeQueueThumbnail(
    context: Context,
    source: String
): ImageBitmap? {
    val thumbnailSources = resolveThumbnailCandidateSources(context = context, source = source)
    for (thumbnailSource in thumbnailSources) {
        val boundsOptions = BitmapFactory.Options().apply {
            inJustDecodeBounds = true
        }
        val hasBounds = openThumbnailInputStream(context = context, source = thumbnailSource)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, boundsOptions)
            true
        } == true
        if (!hasBounds) {
            continue
        }

        val sourceWidth = boundsOptions.outWidth
        val sourceHeight = boundsOptions.outHeight
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            continue
        }

        val decodeOptions = BitmapFactory.Options().apply {
            inSampleSize = calculateInSampleSize(sourceWidth, sourceHeight, THUMBNAIL_TARGET_SIZE_PX)
            inPreferredConfig = Bitmap.Config.RGB_565
        }

        val thumbnail = openThumbnailInputStream(context = context, source = thumbnailSource)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, decodeOptions)
        } ?: continue

        return thumbnail.asImageBitmap()
    }
    return null
}

private fun resolveThumbnailCandidateSources(
    context: Context,
    source: String
): List<String> {
    val normalizedSource = source.trim()
    if (normalizedSource.isBlank()) {
        return emptyList()
    }

    val decodedSources = InputSourceSpec.decode(normalizedSource).orEmpty()
    val directSources = if (decodedSources.isEmpty()) {
        listOf(normalizedSource)
    } else {
        decodedSources
    }

    return directSources
        .map { candidate -> candidate.trim() }
        .filter { candidate -> candidate.isNotBlank() }
        .flatMap { candidate ->
            val treePreview = resolveTreeSourceThumbnail(context = context, source = candidate)
            if (treePreview == null) {
                listOf(candidate)
            } else {
                listOf(treePreview, candidate)
            }
        }
        .distinct()
}

private fun resolveTreeSourceThumbnail(
    context: Context,
    source: String
): String? {
    val sourceUri = runCatching { Uri.parse(source) }.getOrNull() ?: return null
    if (sourceUri.scheme?.lowercase() != "content") {
        return null
    }

    if (!DocumentsContract.isTreeUri(sourceUri)) {
        return null
    }

    val treeDocumentId = runCatching {
        DocumentsContract.getTreeDocumentId(sourceUri)
    }.getOrNull() ?: return null
    val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(sourceUri, treeDocumentId)

    return runCatching {
        context.contentResolver.query(
            childrenUri,
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE
            ),
            null,
            null,
            null
        )?.use { cursor ->
            val documentIdIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
            val mimeTypeIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
            if (documentIdIndex < 0 || mimeTypeIndex < 0) {
                return@use null
            }
            while (cursor.moveToNext()) {
                val mimeType = cursor.getString(mimeTypeIndex)?.trim().orEmpty()
                if (!mimeType.startsWith("image/")) {
                    continue
                }
                val documentId = cursor.getString(documentIdIndex)?.trim().orEmpty()
                if (documentId.isBlank()) {
                    continue
                }
                val documentUri = DocumentsContract.buildDocumentUriUsingTree(sourceUri, documentId)
                return@use documentUri.toString()
            }
            null
        }
    }.getOrNull()
}

private fun openThumbnailInputStream(
    context: Context,
    source: String
) = runCatching {
    if (source.startsWith("/")) {
        return@runCatching File(source).inputStream()
    }

    val parsedUri = Uri.parse(source)
    when (parsedUri.scheme?.lowercase()) {
        null -> File(source).inputStream()
        "file" -> {
            val path = parsedUri.path ?: return@runCatching null
            File(path).inputStream()
        }
        else -> context.contentResolver.openInputStream(parsedUri)
    }
}.getOrNull()

private fun calculateInSampleSize(
    sourceWidth: Int,
    sourceHeight: Int,
    requestedSizePx: Int
): Int {
    var sampleSize = 1
    var currentWidth = sourceWidth
    var currentHeight = sourceHeight
    while (currentWidth > requestedSizePx || currentHeight > requestedSizePx) {
        sampleSize *= 2
        currentWidth /= 2
        currentHeight /= 2
    }
    return sampleSize.coerceAtLeast(1)
}

private const val THUMBNAIL_TARGET_SIZE_PX = 192

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
    val decodedSources = InputSourceSpec.decode(source).orEmpty()
    if (decodedSources.isNotEmpty()) {
        val firstLabel = inputSourceLabel(decodedSources.first())
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
