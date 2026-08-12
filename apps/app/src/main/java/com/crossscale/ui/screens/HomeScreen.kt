package com.crossscale.ui.screens

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.ui.components.ProcessingProgress
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.data.model.ConnectionState
import com.crossscale.viewmodel.MainUiState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.viewmodel.ProcessingQueueItemSummary
import com.crossscale.viewmodel.ProcessingUiState
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    state: MainUiState,
    processingUiState: ProcessingUiState,
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?,
    remoteFallbackMessage: String?,
    onClearRemoteFallbackMessage: () -> Unit,
    onSelectModel: (ModelInfo) -> Unit,
    onSelectImages: () -> Unit,
    onSelectImageFolder: () -> Unit,
    onClearInputSources: () -> Unit,
    onRemoveInputSource: (String) -> Unit,
    onMoveInputSourceUp: (String) -> Unit,
    onMoveInputSourceDown: (String) -> Unit,
    onSelectOutputFolder: () -> Unit,
    onUseDefaultOutputFolder: () -> Unit,
    onToggleOverwriteOriginal: (Boolean) -> Unit,
    onToggleBackupOriginal: (Boolean) -> Unit,
    onToggleProcessNestedFolders: (Boolean) -> Unit,
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
    onCancelProcessing: () -> Unit,
    hasBackupAvailable: Boolean = false,
    onRestoreBackup: () -> Unit = {}
) {
    val selectedInputs = state.selectedInputSources
    val corruptedSources = state.corruptedInputSources
    val selectedModel = state.selectedModel?.name ?: "未选择"
    val selectedOutput = abbreviatePath(state.selectedOutputDirectoryUri ?: "/sdcard/Pictures/CrossScale")
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = queueItems.count { item -> item.status == PersistedQueueItemStatus.PENDING }
    val failedQueueCount = queueItems.count { item -> item.status == PersistedQueueItemStatus.FAILED }

    var showModelSheet by rememberSaveable { mutableStateOf(false) }
    var queueSheetVisible by rememberSaveable { mutableStateOf(false) }
    var previewSource by rememberSaveable { mutableStateOf<String?>(null) }
    val queueSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    val snackbarHostState = remember { SnackbarHostState() }
    val prevConnectionState = remember { mutableStateOf(connectionState) }
    val isProcessing = processingUiState.isProcessing
    LaunchedEffect(connectionState) {
        val previous = prevConnectionState.value
        if (previous != connectionState) {
            val message = when (connectionState) {
                ConnectionState.CONNECTING -> "正在连接上次的 PC…"
                ConnectionState.CONNECTED -> "已连接 PC，使用远程处理"
                ConnectionState.DISCONNECTED -> {
                    if (previous == ConnectionState.CONNECTED && isProcessing) {
                        "PC 不可达，已切换本地处理"
                    } else {
                        "PC 不可达，使用本地处理"
                    }
                }
            }
            snackbarHostState.showSnackbar(message)
            prevConnectionState.value = connectionState
        }
    }

    // PC 掉线回退（处理中）：ProcessingViewModel 在远程不可达、已切换本地处理时
    // 发出一次性信号；展示 snackbar 后清除，避免重复弹出。
    LaunchedEffect(remoteFallbackMessage) {
        val message = remoteFallbackMessage
        if (!message.isNullOrBlank()) {
            snackbarHostState.showSnackbar(message)
            onClearRemoteFallbackMessage()
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = BorderlessTokens.ScreenPaddingHorizontal)
                .padding(bottom = 180.dp),
            verticalArrangement = Arrangement.spacedBy(18.dp)
        ) {
            Spacer(modifier = Modifier.height(6.dp))

            HeroArea(
                selectedInputs = selectedInputs,
                corruptedSources = corruptedSources,
                onAddImage = onSelectImages,
                onAddFolder = onSelectImageFolder,
                onClearInput = onClearInputSources,
                onRemoveInput = onRemoveInputSource,
                onPreviewInput = { source -> previewSource = source }
            )

            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    text = "参数",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = BorderlessTokens.SectionTitleWeight
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha)
                )

                SettingsRow(
                    title = "超分模型",
                    value = selectedModel,
                    onClick = { showModelSheet = true }
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SettingsRow(
                    title = "输出路径",
                    value = selectedOutput,
                    onClick = onSelectOutputFolder,
                    trailing = {
                        TextButton(onClick = onUseDefaultOutputFolder) {
                            Text("默认")
                        }
                    }
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SwitchRow(
                    title = "覆盖原图",
                    checked = state.overwriteOriginalImage,
                    onCheckedChange = onToggleOverwriteOriginal
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SwitchRow(
                    title = "备份原图",
                    checked = state.backupOriginalImage,
                    enabled = state.overwriteOriginalImage,
                    onCheckedChange = onToggleBackupOriginal
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SwitchRow(
                    title = "包含子目录",
                    checked = state.processNestedFolders,
                    onCheckedChange = onToggleProcessNestedFolders
                )
            }

            if (!processingUiState.isProcessing && processingUiState.checkpointSummary != null) {
                CheckpointSection(
                    processingUiState = processingUiState,
                    onResumeProcessing = onResumeProcessing,
                    onRetryFailedItems = onRetryFailedItems,
                    onResumePendingItem = onResumePendingItem,
                    onRetryFailedItem = onRetryFailedItem,
                    onDiscardCheckpoint = onDiscardCheckpoint
                )
            }

            ProcessingProgress(uiState = processingUiState)
        }

        BottomActionBar(
            modifier = Modifier.align(Alignment.BottomCenter),
            isProcessing = processingUiState.isProcessing,
            pendingQueueCount = pendingQueueCount,
            failedQueueCount = failedQueueCount,
            hasBackupAvailable = hasBackupAvailable && !processingUiState.isProcessing && queueItems.isEmpty(),
            connectionState = connectionState,
            connectedEndpoint = connectedEndpoint,
            onShowQueue = {
                queueSheetVisible = true
            },
            onStart = onStartProcessing,
            onRestore = onRestoreBackup,
            onCancel = onCancelProcessing
        )

        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 150.dp)
        )
    }

    if (showModelSheet) {
        ModalBottomSheet(onDismissRequest = { showModelSheet = false }) {
            Text(
                text = "选择模型",
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp)
            )
            state.models.forEach { model ->
                SheetActionItem(
                    label = model.name,
                    subtitle = "x${model.scaleFactor} · tile ${model.tileSize}"
                ) {
                    showModelSheet = false
                    onSelectModel(model)
                }
            }
            Spacer(modifier = Modifier.height(20.dp))
        }
    }

    if (queueSheetVisible) {
        ModalBottomSheet(
            sheetState = queueSheetState,
            scrimColor = MaterialTheme.colorScheme.scrim.copy(alpha = 0.18f),
            onDismissRequest = {
                queueSheetVisible = false
            }
        ) {
            QueueBottomSheetContent(
                processingUiState = processingUiState,
                onStartBatchQueue = onStartBatchQueue,
                onPauseBatchQueue = onPauseBatchQueue,
                onRetryQueuedBatch = onRetryQueuedBatch,
                onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                onRemoveQueuedBatch = onRemoveQueuedBatch,
                onClearBatchQueue = onClearBatchQueue
            )
        }
    }

    previewSource?.let { source ->
        PreviewBottomSheet(
            source = source,
            isCorrupted = source in corruptedSources,
            onDismiss = { previewSource = null }
        )
    }
}

@Composable
private fun HeroArea(
    selectedInputs: List<String>,
    corruptedSources: Set<String>,
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit,
    onClearInput: () -> Unit,
    onRemoveInput: (String) -> Unit,
    onPreviewInput: (String) -> Unit
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .height(280.dp)
            .clip(RoundedCornerShape(26.dp))
            .combinedClickable(
                onClick = onAddImage,
                onLongClick = onAddFolder
            ),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.26f)
    ) {
        if (selectedInputs.isEmpty()) {
            EmptyHero()
        } else {
            SelectedHero(
                selectedInputs = selectedInputs,
                corruptedSources = corruptedSources,
                onAddImage = onAddImage,
                onAddFolder = onAddFolder,
                onClearInput = onClearInput,
                onRemoveInput = onRemoveInput,
                onPreviewInput = onPreviewInput
            )
        }
    }
}

@Composable
private fun EmptyHero() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(18.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Box(
            modifier = Modifier
                .size(84.dp)
                .clip(CircleShape)
                .background(
                    brush = Brush.linearGradient(
                        colors = listOf(
                            MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.95f),
                            MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.85f)
                        )
                    )
                ),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                painter = painterResource(id = android.R.drawable.ic_input_add),
                contentDescription = null,
                tint = Color.White,
                modifier = Modifier.size(36.dp)
            )
        }
        Spacer(modifier = Modifier.height(14.dp))
        Text(
            text = "添加图片或文件夹",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun SelectedHero(
    selectedInputs: List<String>,
    corruptedSources: Set<String>,
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit,
    onClearInput: () -> Unit,
    onRemoveInput: (String) -> Unit,
    onPreviewInput: (String) -> Unit
) {
    val context = LocalContext.current
    val firstSource = selectedInputs.firstOrNull()
    val bgBitmap by produceState<ImageBitmap?>(initialValue = null, firstSource) {
        value = firstSource?.let { source ->
            decodeQueueThumbnailWithLimit(context = context, source = source)
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        if (bgBitmap != null) {
            Image(
                bitmap = bgBitmap!!,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .fillMaxSize()
                    .blur(22.dp)
            )
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.62f))
            )
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(12.dp),
            verticalArrangement = Arrangement.SpaceBetween
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "已选择 ${selectedInputs.size} 项",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = BorderlessTokens.SectionTitleWeight
                )
                Text(
                    text = "继续添加",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.combinedClickable(onClick = onAddImage, onLongClick = onAddFolder)
                )
            }

            LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                items(items = selectedInputs, key = { item -> item }) { source ->
                    HeroThumbnail(
                        source = source,
                        isCorrupted = source in corruptedSources,
                        onDelete = { onRemoveInput(source) },
                        onPreview = { onPreviewInput(source) }
                    )
                }
            }
        }

        Surface(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(12.dp)
                .size(34.dp)
                .clickable(onClick = onClearInput),
            shape = CircleShape,
            color = MaterialTheme.colorScheme.surface
        ) {
            Box(contentAlignment = Alignment.Center) {
                Icon(
                    painter = painterResource(id = android.R.drawable.ic_menu_close_clear_cancel),
                    contentDescription = null,
                    modifier = Modifier.size(16.dp),
                    tint = MaterialTheme.colorScheme.onSurface
                )
            }
        }
    }
}

@Composable
private fun HeroThumbnail(
    source: String,
    isCorrupted: Boolean,
    onDelete: () -> Unit,
    onPreview: () -> Unit
) {
    val context = LocalContext.current
    val bitmapState by produceState<ImageBitmap?>(initialValue = null, source) {
        value = decodeQueueThumbnailWithLimit(context = context, source = source)
    }

    Box(
        modifier = Modifier
            .size(width = 96.dp, height = 116.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(MaterialTheme.colorScheme.surface)
            .combinedClickable(onClick = onDelete, onLongClick = onPreview),
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
            Text(
                text = if (isCorrupted) "损坏" else "预览",
                style = MaterialTheme.typography.bodySmall,
                color = if (isCorrupted) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
            )
        }
    }
}

@Composable
@OptIn(ExperimentalMaterial3Api::class)
private fun PreviewBottomSheet(
    source: String,
    isCorrupted: Boolean,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    val previewBitmap by produceState<ImageBitmap?>(initialValue = null, source) {
        value = decodeQueueThumbnailWithLimit(context = context, source = source)
    }
    val previewTitle = remember(source) {
        source.substringAfterLast('/').ifBlank {
            source.takeLast(28).ifBlank { "预览" }
        }
    }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = BorderlessTokens.ScreenPaddingHorizontal),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = "预览 · $previewTitle",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = BorderlessTokens.SectionTitleWeight
            )
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 220.dp, max = 520.dp)
                    .clip(RoundedCornerShape(18.dp))
                    .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)),
                contentAlignment = Alignment.Center
            ) {
                val bitmap = previewBitmap
                if (bitmap == null) {
                    Text(
                        text = if (isCorrupted) "图片损坏" else "暂无可预览图像",
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (isCorrupted) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant
                    )
                } else {
                    Image(
                        bitmap = bitmap,
                        contentDescription = null,
                        modifier = Modifier.fillMaxSize(),
                        contentScale = ContentScale.Fit
                    )
                }
            }
            Text(
                text = "长按缩略图可预览，单击缩略图可删除",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.height(14.dp))
        }
    }
}

@Composable
private fun SettingsRow(
    title: String,
    value: String,
    onClick: () -> Unit,
    trailing: (@Composable () -> Unit)? = null
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            .padding(vertical = BorderlessTokens.RowVerticalPadding),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(text = title, style = MaterialTheme.typography.bodyLarge)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = value,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.width(158.dp)
            )
            trailing?.invoke()
        }
    }
}

@Composable
private fun SwitchRow(
    title: String,
    checked: Boolean,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
            .padding(vertical = BorderlessTokens.ListItemVerticalPadding),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(text = title, style = MaterialTheme.typography.bodyLarge)
        Switch(checked = checked, onCheckedChange = onCheckedChange, enabled = enabled)
    }
}

/**
 * 醒目的处理模式指示条（始终可见，挂在底部操作区内）。
 * 连上 PC → 绿色「远程」；未连 → 蓝色「本地」；连接中 → 琥珀。
 */
@Composable
private fun ModeChip(
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?
) {
    val backgroundColor: Color
    val contentColor: Color
    val text: String
    when (connectionState) {
        ConnectionState.CONNECTED -> {
            backgroundColor = Color(0xFFD7F4DE)
            contentColor = Color(0xFF1B5E20)
            val endpoint = connectedEndpoint?.let { "${it.host}:${it.port}" }.orEmpty()
            text = "🖥️ 远程 · PC 处理 · $endpoint"
        }
        ConnectionState.DISCONNECTED -> {
            backgroundColor = Color(0xFFD6E4FF)
            contentColor = Color(0xFF0D47A1)
            text = "📱 本地 · 手机处理"
        }
        ConnectionState.CONNECTING -> {
            backgroundColor = Color(0xFFFFE8C2)
            contentColor = Color(0xFF8A5A00)
            text = "🔄 连接 PC 中…"
        }
    }

    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = backgroundColor,
        contentColor = contentColor,
        shape = RoundedCornerShape(20.dp)
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp)
        )
    }
}

@Composable
private fun BottomActionBar(
    modifier: Modifier = Modifier,
    isProcessing: Boolean,
    pendingQueueCount: Int,
    failedQueueCount: Int,
    hasBackupAvailable: Boolean = false,
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?,
    onShowQueue: () -> Unit,
    onStart: () -> Unit,
    onRestore: () -> Unit = {},
    onCancel: () -> Unit
) {
    Surface(modifier = modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.background) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            ModeChip(
                connectionState = connectionState,
                connectedEndpoint = connectedEndpoint
            )

            QueueDockEntry(
                pendingQueueCount = pendingQueueCount,
                failedQueueCount = failedQueueCount,
                onShowQueue = onShowQueue
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                if (hasBackupAvailable && !isProcessing) {
                    Button(
                        onClick = onRestore,
                        modifier = Modifier
                            .weight(1f)
                            .height(52.dp),
                        shape = CircleShape,
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                            contentColor = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                    ) {
                        Text(
                            "恢复备份",
                            fontWeight = BorderlessTokens.SectionTitleWeight
                        )
                    }
                }

                Button(
                    onClick = onStart,
                    modifier = Modifier
                        .weight(1f)
                        .height(52.dp),
                    shape = CircleShape,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.primaryContainer,
                        contentColor = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                ) {
                    Text(
                        if (isProcessing) "继续队列" else "开始超分",
                        fontWeight = BorderlessTokens.SectionTitleWeight
                    )
                }

                if (isProcessing) {
                    OutlinedButton(
                        onClick = onCancel,
                        modifier = Modifier
                            .weight(0.68f)
                            .height(52.dp),
                        shape = CircleShape
                    ) {
                        Text("停止")
                    }
                }
            }

            Spacer(modifier = Modifier.height(4.dp))
        }
    }
}

@Composable
private fun QueueDockEntry(
    pendingQueueCount: Int,
    failedQueueCount: Int,
    onShowQueue: () -> Unit
) {
    val interactionSource = remember { MutableInteractionSource() }

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(18.dp))
            .clickable(
                interactionSource = interactionSource,
                indication = null,
                onClick = onShowQueue
            ),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(28.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.85f)),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    painter = painterResource(id = android.R.drawable.ic_menu_sort_by_size),
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onPrimaryContainer,
                    modifier = Modifier.size(15.dp)
                )
            }

            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    text = "队列",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = BorderlessTokens.SectionTitleWeight
                )
                Text(
                    text = "待处理 $pendingQueueCount · 失败 $failedQueueCount",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            QueueStatusBadge(pendingQueueCount = pendingQueueCount, failedQueueCount = failedQueueCount)

            Icon(
                painter = painterResource(id = android.R.drawable.arrow_up_float),
                contentDescription = "打开队列",
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(16.dp)
            )
        }
    }
}

@Composable
private fun QueueStatusBadge(
    pendingQueueCount: Int,
    failedQueueCount: Int
) {
    val label = when {
        failedQueueCount > 0 -> "${failedQueueCount} 失败"
        pendingQueueCount > 0 -> "${pendingQueueCount} 待处理"
        else -> "空"
    }
    val backgroundColor = when {
        failedQueueCount > 0 -> MaterialTheme.colorScheme.errorContainer
        pendingQueueCount > 0 -> MaterialTheme.colorScheme.secondaryContainer
        else -> MaterialTheme.colorScheme.surface
    }
    val contentColor = when {
        failedQueueCount > 0 -> MaterialTheme.colorScheme.onErrorContainer
        pendingQueueCount > 0 -> MaterialTheme.colorScheme.onSecondaryContainer
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }

    Surface(shape = CircleShape, color = backgroundColor) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = contentColor,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp)
        )
    }
}

@Composable
private fun QueueBottomSheetContent(
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
    val pendingQueueCount = queueItems.count { item -> item.status == PersistedQueueItemStatus.PENDING }
    val failedQueueCount = queueItems.count { item -> item.status == PersistedQueueItemStatus.FAILED }
    val queueRunningLabel = if (processingUiState.isQueueRunning) "运行中" else "已暂停"

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 300.dp, max = 640.dp)
            .padding(horizontal = BorderlessTokens.ScreenPaddingHorizontal),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text(
            text = "队列详情",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = "状态：$queueRunningLabel · 待处理 $pendingQueueCount · 失败 $failedQueueCount",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = onStartBatchQueue,
                enabled = !processingUiState.isProcessing && pendingQueueCount > 0,
                modifier = Modifier.weight(1f)
            ) {
                Text("启动")
            }
            OutlinedButton(
                onClick = onPauseBatchQueue,
                enabled = processingUiState.isQueueRunning,
                modifier = Modifier.weight(1f)
            ) {
                Text("暂停")
            }
            OutlinedButton(
                onClick = onClearBatchQueue,
                enabled = queueItems.isNotEmpty(),
                modifier = Modifier.weight(1f)
            ) {
                Text("清空")
            }
        }

        if (queueItems.isEmpty()) {
            Text(
                text = "队列为空",
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
private fun CheckpointSection(
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

@Composable
private fun SheetActionItem(label: String, subtitle: String? = null, onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(
                horizontal = BorderlessTokens.ScreenPaddingHorizontal,
                vertical = BorderlessTokens.RowVerticalPadding
            )
    ) {
        Text(text = label, style = MaterialTheme.typography.bodyLarge)
        subtitle?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
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
        PersistedQueueItemStatus.PENDING -> "未开始"
        PersistedQueueItemStatus.RUNNING -> "处理中"
        PersistedQueueItemStatus.FAILED -> "失败"
        PersistedQueueItemStatus.COMPLETED -> "已完成"
    }
    val canOpenOutput = queueItem.status == PersistedQueueItemStatus.COMPLETED &&
        !queueItem.lastOutputUri.isNullOrBlank()

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

        if (queueItem.status == PersistedQueueItemStatus.FAILED) {
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
                enabled = queueItem.status != PersistedQueueItemStatus.RUNNING
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
            Text(text = "无图", style = MaterialTheme.typography.bodySmall)
        }
    }
}

private fun decodeQueueThumbnail(
    context: Context,
    source: String
): ImageBitmap? {
    val sourceKey = thumbnailCacheKey(source)
    ThumbnailMemoryCache.getThumbnail(sourceKey)?.let { cachedBitmap ->
        return cachedBitmap
    }

    val thumbnailSources = resolveThumbnailCandidateSources(context = context, source = source)
    for (thumbnailSource in thumbnailSources) {
        val candidateKey = thumbnailCacheKey(thumbnailSource)
        ThumbnailMemoryCache.getThumbnail(candidateKey)?.let { cachedBitmap ->
            ThumbnailMemoryCache.putThumbnail(sourceKey, cachedBitmap)
            return cachedBitmap
        }

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

        val imageBitmap = thumbnail.asImageBitmap()
        ThumbnailMemoryCache.putThumbnail(candidateKey, imageBitmap)
        ThumbnailMemoryCache.putThumbnail(sourceKey, imageBitmap)
        return imageBitmap
    }
    return null
}

private suspend fun decodeQueueThumbnailWithLimit(
    context: Context,
    source: String
): ImageBitmap? {
    return withContext(Dispatchers.IO) {
        ThumbnailDecodeLimiter.withPermit {
            decodeQueueThumbnail(context = context, source = source)
        }
    }
}

private fun resolveThumbnailCandidateSources(
    context: Context,
    source: String
): List<String> {
    val normalizedSource = source.trim()
    if (normalizedSource.isBlank()) {
        return emptyList()
    }

    ThumbnailMemoryCache.getSourceCandidates(normalizedSource)?.let { cachedCandidates ->
        return cachedCandidates
    }

    val decodedSources = InputSourceSpec.decode(normalizedSource).orEmpty()
    val directSources = if (decodedSources.isEmpty()) {
        listOf(normalizedSource)
    } else {
        decodedSources
    }

    val resolvedSources = directSources
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

    ThumbnailMemoryCache.putSourceCandidates(normalizedSource, resolvedSources)
    return resolvedSources
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

private fun thumbnailCacheKey(source: String): String {
    return source.trim()
}

private object ThumbnailMemoryCache {
    private const val THUMBNAIL_CACHE_SIZE = 96
    private const val SOURCE_CANDIDATE_CACHE_SIZE = 128
    private val thumbnailCache = LruCache<String, ImageBitmap>(THUMBNAIL_CACHE_SIZE)
    private val sourceCandidateCache = LruCache<String, List<String>>(SOURCE_CANDIDATE_CACHE_SIZE)

    fun getThumbnail(sourceKey: String): ImageBitmap? {
        if (sourceKey.isBlank()) {
            return null
        }
        return synchronized(this) {
            thumbnailCache.get(sourceKey)
        }
    }

    fun putThumbnail(sourceKey: String, bitmap: ImageBitmap) {
        if (sourceKey.isBlank()) {
            return
        }
        synchronized(this) {
            thumbnailCache.put(sourceKey, bitmap)
        }
    }

    fun getSourceCandidates(sourceKey: String): List<String>? {
        if (sourceKey.isBlank()) {
            return null
        }
        return synchronized(this) {
            sourceCandidateCache.get(sourceKey)
        }
    }

    fun putSourceCandidates(sourceKey: String, candidates: List<String>) {
        if (sourceKey.isBlank() || candidates.isEmpty()) {
            return
        }
        synchronized(this) {
            sourceCandidateCache.put(sourceKey, candidates)
        }
    }
}

private object ThumbnailDecodeLimiter {
    private val semaphore = Semaphore(THUMBNAIL_DECODE_PARALLELISM)

    suspend fun <T> withPermit(block: () -> T): T {
        return semaphore.withPermit {
            block()
        }
    }
}

private const val THUMBNAIL_TARGET_SIZE_PX = 192
private const val THUMBNAIL_DECODE_PARALLELISM = 2

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

private fun abbreviatePath(path: String): String {
    val normalized = path.trim()
    if (normalized.length <= 22) {
        return normalized
    }
    return "${normalized.take(12)}...${normalized.takeLast(7)}"
}
