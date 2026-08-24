package com.crossscale.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.data.model.RemoteModel
import com.crossscale.data.repository.OutputImageFormat
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.ui.components.CrossScaleTopAppBar
import com.crossscale.ui.components.RemoteModelsDialog
import com.crossscale.ui.components.StageProgressOverlay
import com.crossscale.ui.screens.home.BottomActionDock
import com.crossscale.ui.screens.home.CheckpointSection
import com.crossscale.ui.screens.home.ImageStage
import com.crossscale.ui.screens.home.ParameterCard
import com.crossscale.ui.screens.home.PreviewBottomSheet
import com.crossscale.ui.screens.home.QueueBottomSheetContent
import com.crossscale.ui.theme.EnterMotionSpec
import com.crossscale.ui.theme.EnterSizeSpec
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.MaxContentWidth
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ModelsUiState
import com.crossscale.viewmodel.ProcessingUiState

/**
 * Home「聚焦工作台」：图片舞台是唯一视觉焦点，UI 退后。
 * 顶部标识条 → 图片舞台 → 参数卡 → 断点续传卡 → 底部悬浮 Dock。
 */
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
    onSelectScale: (Int) -> Unit,
    onSelectImages: () -> Unit,
    onSelectImageFolder: () -> Unit,
    onClearInputSources: () -> Unit,
    onRemoveInputSource: (String) -> Unit,
    onSelectOutputFolder: () -> Unit,
    onUseDefaultOutputFolder: () -> Unit,
    onToggleOverwriteOriginal: (Boolean) -> Unit,
    onToggleBackupOriginal: (Boolean) -> Unit,
    onToggleProcessNestedFolders: (Boolean) -> Unit,
    onSelectOutputFormat: (OutputImageFormat) -> Unit,
    remoteUiState: ModelsUiState,
    onRefreshRemoteModels: () -> Unit,
    onDownloadRemoteModel: (RemoteModel) -> Unit,
    onDismissDownloadError: (String) -> Unit,
    onStartProcessing: () -> Unit,
    onStartBatchQueue: () -> Unit,
    onPauseBatchQueue: () -> Unit,
    onRetryQueuedBatch: (String) -> Unit,
    onRetryQueuedBatchFailedOnly: (String) -> Unit,
    onOpenQueuedBatchOutput: (String) -> Unit,
    onRemoveQueuedBatch: (String) -> Unit,
    onClearBatchQueue: () -> Unit,
    onCancelProcessing: () -> Unit,
    onResumeProcessing: () -> Unit,
    onRetryFailedItems: () -> Unit,
    onResumePendingItem: (String) -> Unit,
    onRetryFailedItem: (String) -> Unit,
    onDiscardCheckpoint: () -> Unit,
    hasBackupAvailable: Boolean = false,
    onRestoreBackup: () -> Unit = {},
    onCancelDownload: (String) -> Unit = {}
) {
    val notSelectedLabel = stringResource(R.string.common_not_selected)
    val snackbarConnectingMessage = stringResource(R.string.home_snackbar_connecting)
    val snackbarConnectedMessage = stringResource(R.string.home_snackbar_connected)
    val snackbarPcUnreachableSwitchedMessage = stringResource(R.string.processing_remote_fallback)
    val snackbarPcUnreachableLocalMessage = stringResource(R.string.home_snackbar_pc_unreachable_local)

    val selectedInputs = state.selectedInputSources
    val corruptedSources = state.corruptedInputSources
    // remember 持稳定实例：输入未变时不重建 List/String，恢复下游可跳过性。
    val selectedOutput = remember(state.selectedOutputDirectoryUri) {
        prettyOutputPath(state.selectedOutputDirectoryUri ?: "/sdcard/Pictures/CrossScale")
    }
    val queueItems = processingUiState.queueItems
    // 倍率针对当前模型：只列出同系列（名称去掉 -xN 后缀）的倍率；
    // 系列内没有其他倍率时为空，参数卡即隐藏倍率选择。
    val availableScales = remember(state.models, state.selectedModel?.name) {
        val selectedFamily = state.selectedModel?.name?.let { name ->
            name.substringBeforeLast("-x", missingDelimiterValue = name)
        }
        state.models
            .filter { model ->
                model.name.substringBeforeLast("-x", missingDelimiterValue = model.name) == selectedFamily
            }
            .map { model -> model.scaleFactor }
            .distinct()
            .sorted()
    }
    // 队列角标只统计待处理 + 运行中：完成/失败条目不代表剩余工作量。
    val queueCount = remember(queueItems) {
        queueItems.count { item ->
            item.status == PersistedQueueItemStatus.PENDING ||
                item.status == PersistedQueueItemStatus.RUNNING
        }
    }

    var showModelSheet by rememberSaveable { mutableStateOf(false) }
    var showFormatSheet by rememberSaveable { mutableStateOf(false) }
    var showRemoteModelsDialog by rememberSaveable { mutableStateOf(false) }
    var queueSheetVisible by rememberSaveable { mutableStateOf(false) }
    var previewSource by rememberSaveable { mutableStateOf<String?>(null) }
    val queueSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    val snackbarHostState = remember { SnackbarHostState() }
    val prevConnectionState = remember { mutableStateOf(connectionState) }
    val isProcessing = processingUiState.isProcessing
    LaunchedEffect(connectionState) {
        val previous = prevConnectionState.value
        if (previous != connectionState) {
            // 先落新状态再挂起等 snackbar：挂起期间状态再变不会用旧 previous 判错文案。
            prevConnectionState.value = connectionState
            val message = when (connectionState) {
                ConnectionState.CONNECTING -> snackbarConnectingMessage
                ConnectionState.CONNECTED -> snackbarConnectedMessage
                ConnectionState.DISCONNECTED -> {
                    if (previous == ConnectionState.CONNECTED && isProcessing) {
                        snackbarPcUnreachableSwitchedMessage
                    } else {
                        snackbarPcUnreachableLocalMessage
                    }
                }
            }
            snackbarHostState.showSnackbar(message)
        }
    }

    // PC 掉线回退（处理中）：ProcessingViewModel 在远程不可达、已切换本地处理时
    // 发出一次性信号；先清除再展示，避免 showSnackbar 挂起期间重建导致重放。
    LaunchedEffect(remoteFallbackMessage) {
        val message = remoteFallbackMessage
        if (!message.isNullOrBlank()) {
            onClearRemoteFallbackMessage()
            snackbarHostState.showSnackbar(message)
        }
    }

    Scaffold(
        topBar = {
            CrossScaleTopAppBar(
                title = stringResource(R.string.app_name),
                actions = {
                    HomeConnectionStatus(
                        connectionState = connectionState,
                        connectedEndpoint = connectedEndpoint
                    )
                }
            )
        },
        containerColor = MaterialTheme.colorScheme.background,
        // 顶层 app shell 已统一消费系统栏 insets（compact 由 NavigationBar 占位、
        // medium+ 由外层 navigationBarsPadding），这里不再叠加。
        contentWindowInsets = WindowInsets(0, 0, 0, 0)
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
            // 宽屏正文列限宽居中；悬浮 Dock/Snackbar 仍按全宽对齐。
            contentAlignment = Alignment.TopCenter
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .widthIn(max = MaxContentWidth)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = LocalScreenMargin.current)
                    .padding(top = Spacing.md, bottom = BottomDockClearance),
                verticalArrangement = Arrangement.spacedBy(Spacing.lg)
            ) {
            ImageStage(
                selectedInputs = selectedInputs,
                corruptedSources = corruptedSources,
                isProcessing = isProcessing,
                onAddImage = onSelectImages,
                onAddFolder = onSelectImageFolder,
                onClearInput = onClearInputSources,
                onRemoveInput = onRemoveInputSource,
                onPreviewInput = { source -> previewSource = source },
                overlay = {
                    StageProcessingOverlay(
                        visible = isProcessing,
                        task = processingUiState.task,
                        onCancel = onCancelProcessing
                    )
                }
            )

            ParameterCard(
                selectedModel = state.selectedModel,
                notSelectedLabel = notSelectedLabel,
                outputDirectoryLabel = selectedOutput,
                overwriteOriginal = state.overwriteOriginalImage,
                backupOriginal = state.backupOriginalImage,
                processNestedFolders = state.processNestedFolders,
                outputFormatLabel = outputFormatLabel(
                    format = state.outputImageFormat,
                    followLabel = stringResource(R.string.config_output_format_follow)
                ),
                availableScales = availableScales,
                hasCustomOutputDirectory = state.selectedOutputDirectoryUri != null,
                onPickModel = { showModelSheet = true },
                onPickScale = onSelectScale,
                onPickFormat = { showFormatSheet = true },
                onSelectOutputFolder = onSelectOutputFolder,
                onUseDefaultOutputFolder = onUseDefaultOutputFolder,
                onToggleOverwriteOriginal = onToggleOverwriteOriginal,
                onToggleBackupOriginal = onToggleBackupOriginal,
                onToggleProcessNestedFolders = onToggleProcessNestedFolders
            )

            // 断点续传卡：存在/消失做 fade + 纵向展开/收起（200ms ease-out）。
            AnimatedVisibility(
                visible = !isProcessing && processingUiState.checkpointSummary != null,
                enter = fadeIn(EnterMotionSpec) + expandVertically(EnterSizeSpec),
                exit = shrinkVertically(EnterSizeSpec) + fadeOut(EnterMotionSpec)
            ) {
                CheckpointSection(
                    processingUiState = processingUiState,
                    onResumeProcessing = onResumeProcessing,
                    onRetryFailedItems = onRetryFailedItems,
                    onResumePendingItem = onResumePendingItem,
                    onRetryFailedItem = onRetryFailedItem,
                    onDiscardCheckpoint = onDiscardCheckpoint
                )
            }
        }

            BottomActionDock(
                modifier = Modifier.align(Alignment.BottomCenter),
                isProcessing = isProcessing,
                hasBackupAvailable = hasBackupAvailable,
                queueCount = queueCount,
                onShowQueue = {
                    queueSheetVisible = true
                },
                onStart = onStartProcessing,
                onRestore = onRestoreBackup
            )

            SnackbarHost(
                hostState = snackbarHostState,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = SnackbarAboveDockPadding)
            )
        }
    }

    if (showModelSheet) {
        // 仅在远程列表为空时自动拉取一次；已有列表时不主动联网，靠手动刷新。
        if (remoteUiState.remoteModels.isEmpty()) {
            LaunchedEffect(Unit) { onRefreshRemoteModels() }
        }
        // 大屏取舍：官方暂无 SideSheet 组件，medium+ 上 ModalBottomSheet
        // 自动限宽居中展示（不拉伸全宽），作为侧栏方案的过渡实现。
        ModalBottomSheet(
            onDismissRequest = { showModelSheet = false },
            sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
            shape = MaterialTheme.shapes.extraLarge,
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = Spacing.xl, end = Spacing.sm),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.home_select_model_sheet_title),
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(vertical = Spacing.sm)
                )
                // 右上角下载入口：与设置页相同的远程模型下载对话框。
                IconButton(onClick = { showRemoteModelsDialog = true }) {
                    Icon(
                        imageVector = Icons.Outlined.CloudDownload,
                        contentDescription = stringResource(R.string.models_download),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
            state.models.forEach { model ->
                SelectSheetItem(
                    label = model.name,
                    sublabel = stringResource(R.string.home_model_subtitle, model.scaleFactor),
                    selected = state.selectedModel?.name == model.name
                ) {
                    showModelSheet = false
                    onSelectModel(model)
                }
            }
            Spacer(modifier = Modifier.height(Spacing.xl))
        }
    }

    if (showFormatSheet) {
        val followLabel = stringResource(R.string.config_output_format_follow)
        ModalBottomSheet(
            onDismissRequest = { showFormatSheet = false },
            sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
            shape = MaterialTheme.shapes.extraLarge,
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Text(
                text = stringResource(R.string.home_select_output_format_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = Spacing.xl, vertical = Spacing.sm)
            )
            OutputImageFormat.entries.forEach { format ->
                SelectSheetItem(
                    label = outputFormatLabel(format, followLabel),
                    selected = state.outputImageFormat == format,
                    // 等宽字体无 CJK 字形：「跟随原图」等中文走系统字体，仅拉丁格式名用 mono。
                    labelStyle = if (format == OutputImageFormat.FOLLOW) null else ValueMono
                ) {
                    showFormatSheet = false
                    onSelectOutputFormat(format)
                }
            }
            Spacer(modifier = Modifier.height(Spacing.xl))
        }
    }

    if (queueSheetVisible) {
        ModalBottomSheet(
            sheetState = queueSheetState,
            shape = MaterialTheme.shapes.extraLarge,
            containerColor = MaterialTheme.colorScheme.surface,
            scrimColor = MaterialTheme.colorScheme.scrim.copy(alpha = 0.18f),
            onDismissRequest = {
                queueSheetVisible = false
            }
        ) {
            QueueBottomSheetContent(
                processingUiState = processingUiState,
                onStartBatchQueue = onStartBatchQueue,
                onPauseBatchQueue = onPauseBatchQueue,
                onCancelProcessing = onCancelProcessing,
                onRetryQueuedBatch = onRetryQueuedBatch,
                onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                onRemoveQueuedBatch = onRemoveQueuedBatch,
                onClearBatchQueue = onClearBatchQueue,
                onDismiss = { queueSheetVisible = false }
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

    if (showRemoteModelsDialog) {
        RemoteModelsDialog(
            remoteModels = remoteUiState.remoteModels,
            installedModelNames = state.models.map { it.name }.toSet(),
            downloads = remoteUiState.downloads,
            isLoading = remoteUiState.isLoadingRemote,
            errorMessage = remoteUiState.remoteError,
            onRefresh = onRefreshRemoteModels,
            onDownload = onDownloadRemoteModel,
            onDismiss = { showRemoteModelsDialog = false },
            onDismissError = onDismissDownloadError,
            onCancelDownload = onCancelDownload
        )
    }
}

/**
 * 底部悬浮 Dock 占位推导：56dp CTA + 16dp 悬浮间距 + 24dp 滚动间隙，
 * 供滚动列底部预留与 Snackbar 避让共用，替代散落的魔法数。
 */
private val BottomDockClearance = 56.dp + 16.dp + 24.dp

/** Snackbar 在 Dock 之上再留 12dp 间隙。 */
private val SnackbarAboveDockPadding = 56.dp + 16.dp + 12.dp

/** Sheet 内单选项：M3 官方 ListItem + RadioButton 单选列表模式。 */
@Composable
private fun SelectSheetItem(
    label: String,
    sublabel: String? = null,
    selected: Boolean,
    labelStyle: TextStyle? = null,
    onClick: () -> Unit
) {
    ListItem(
        headlineContent = {
            Text(text = label, style = labelStyle ?: MaterialTheme.typography.bodyLarge)
        },
        supportingContent = if (sublabel != null) {
            {
                Text(
                    text = sublabel,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        } else {
            null
        },
        trailingContent = { RadioButton(selected = selected, onClick = null) },
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
    )
}

/** 舞台处理 scrim/覆盖层：出现/消失做 fade（200ms ease-out）。只接收 task，避免整包 UI state 参与 overlay 重组。 */
@Composable
private fun BoxScope.StageProcessingOverlay(
    visible: Boolean,
    task: com.crossscale.data.model.ProcessingTask?,
    onCancel: () -> Unit
) {
    androidx.compose.animation.AnimatedVisibility(
        visible = visible,
        enter = fadeIn(EnterMotionSpec),
        exit = fadeOut(EnterMotionSpec)
    ) {
        StageProgressOverlay(
            task = task,
            onCancel = onCancel
        )
    }
}

/** 顶栏连接状态指示：状态点 + 文本，颜色随连接状态（原 HomeIdentityBar 升级为 TopAppBar actions）。 */
@Composable
private fun HomeConnectionStatus(
    connectionState: ConnectionState,
    connectedEndpoint: com.crossscale.data.model.RemoteEndpoint?
) {
    val (statusLabelRes, statusColor) = when (connectionState) {
        ConnectionState.CONNECTED -> R.string.config_mode_remote to MaterialTheme.colorScheme.primary
        ConnectionState.DISCONNECTED -> R.string.config_mode_local to MaterialTheme.colorScheme.onSurfaceVariant
        ConnectionState.CONNECTING -> R.string.config_mode_connecting to MaterialTheme.colorScheme.primary
    }

    val statusLabel = stringResource(statusLabelRes)
    val endpoint = connectedEndpoint?.let { "${it.host}:${it.port}" }
    val text = if (connectionState == ConnectionState.CONNECTED && endpoint != null) {
        "$statusLabel $endpoint"
    } else {
        statusLabel
    }

    Row(
        modifier = Modifier.padding(end = Spacing.md),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        Box(
            modifier = Modifier
                .size(8.dp)
                .clip(CircleShape)
                .background(statusColor)
        )
        Text(
            text = text,
            style = MaterialTheme.typography.labelMedium,
            color = statusColor,
            maxLines = 1
        )
    }
}

/**
 * SAF tree URI 转可读路径：取 tree/ 后的路径并 URL 解码，
 * primary: 前缀表示内部存储根目录，替换为 /。解析失败时原样返回。
 */
private fun prettyOutputPath(uri: String): String {
    val marker = "/tree/"
    val idx = uri.indexOf(marker)
    if (!uri.startsWith("content://") || idx < 0) {
        return uri
    }
    val decoded = runCatching {
        java.net.URLDecoder.decode(uri.substring(idx + marker.length), "UTF-8")
    }.getOrDefault("")
    if (decoded.isEmpty()) {
        return uri
    }
    return when {
        decoded.startsWith("primary:") -> "/" + decoded.removePrefix("primary:")
        else -> decoded
    }
}

/** 输出格式选项文本：「跟随原图」走资源，其余用常量。 */
private fun outputFormatLabel(format: OutputImageFormat, followLabel: String): String = when (format) {
    OutputImageFormat.FOLLOW -> followLabel
    OutputImageFormat.PNG -> "PNG"
    OutputImageFormat.JPG -> "JPG"
    OutputImageFormat.WEBP -> "WebP"
    OutputImageFormat.AVIF -> "AVIF"
}
