package io.upix.ui.screens

import android.widget.Toast
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
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
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.SheetValue
import androidx.compose.material3.rememberBottomSheetState
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
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.data.model.ConnectionState
import io.upix.data.model.ModelInfo
import io.upix.data.model.RemoteEndpoint
import io.upix.data.model.RemoteModel
import io.upix.data.repository.OutputImageFormat
import io.upix.data.repository.PersistedQueueItemStatus
import io.upix.ui.components.UpixTopAppBar
import io.upix.ui.components.RemoteModelsSheet
import io.upix.ui.components.StageProgressOverlay
import io.upix.ui.screens.home.BottomActionDock
import io.upix.ui.screens.home.CheckpointSection
import io.upix.ui.screens.home.DockCtaHeight
import io.upix.ui.screens.home.ImageStage
import io.upix.ui.screens.home.ModelPickerSheet
import io.upix.ui.screens.home.ParameterCard
import io.upix.ui.screens.home.PreviewBottomSheet
import io.upix.ui.screens.home.QueueBottomSheetContent
import io.upix.ui.theme.EnterMotionSpec
import io.upix.ui.theme.EnterSizeSpec
import io.upix.ui.theme.EnterTween
import io.upix.ui.theme.LocalScreenMargin
import io.upix.ui.theme.MaxContentWidth
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.ValueMono
import io.upix.viewmodel.MainUiState
import io.upix.viewmodel.ModelsUiState
import io.upix.viewmodel.ProcessingUiState

/**
 * Home「聚焦工作台」：图片舞台是唯一视觉焦点，UI 退后。
 * 顶部标识条 → 图片舞台 → 平铺参数区 → 断点续传通知条 → 底部悬浮 Dock。
 */
/** Home 的全部事件回调。收拢成数据类，避免 30+ 个逐参数透传。 */
data class HomeCallbacks(
    val onClearRemoteFallbackMessage: () -> Unit,
    val onSelectModel: (ModelInfo) -> Unit,
    val onSelectScale: (Int) -> Unit,
    val onSelectImages: () -> Unit,
    val onSelectImageFolder: () -> Unit,
    val onClearInputSources: () -> Unit,
    val onRemoveInputSource: (String) -> Unit,
    val onSelectOutputFolder: () -> Unit,
    val onToggleOverwriteOriginal: (Boolean) -> Unit,
    val onToggleBackupOriginal: (Boolean) -> Unit,
    val onToggleProcessNestedFolders: (Boolean) -> Unit,
    val onToggleSkipSrMarkedInputs: (Boolean) -> Unit = {},
    val onSelectOutputFormat: (OutputImageFormat) -> Unit,
    val onRefreshRemoteModels: () -> Unit,
    val onDownloadRemoteModel: (RemoteModel) -> Unit,
    val onDismissDownloadError: (String) -> Unit,
    val onStartProcessing: () -> Unit,
    val onStartBatchQueue: () -> Unit,
    val onPauseBatchQueue: () -> Unit,
    val onRetryQueuedBatch: (String) -> Unit,
    val onRetryQueuedBatchFailedOnly: (String) -> Unit,
    val onOpenQueuedBatchOutput: (String) -> Unit,
    val onRemoveQueuedBatch: (String) -> Unit,
    val onClearBatchQueue: () -> Unit,
    val onCancelProcessing: () -> Unit,
    val onResumeProcessing: () -> Unit,
    val onRetryFailedItems: () -> Unit,
    val onResumePendingItem: (String) -> Unit,
    val onRetryFailedItem: (String) -> Unit,
    val onDiscardCheckpoint: () -> Unit,
    val onRestoreBackup: () -> Unit = {},
    val onCancelDownload: (String) -> Unit = {},
    val onResolveQueuedBatchOutputUri: (String) -> String? = { null },
    val onOpenHelp: () -> Unit = {}
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    state: MainUiState,
    processingUiState: ProcessingUiState,
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?,
    remoteFallbackMessage: String?,
    remoteUiState: ModelsUiState,
    hasBackupAvailable: Boolean = false,
    callbacks: HomeCallbacks
) {
    // 解构回原名，函数体保持不变；顺序必须与 HomeCallbacks 属性声明一致。
    val (
        onClearRemoteFallbackMessage,
        onSelectModel,
        onSelectScale,
        onSelectImages,
        onSelectImageFolder,
        onClearInputSources,
        onRemoveInputSource,
        onSelectOutputFolder,
        onToggleOverwriteOriginal,
        onToggleBackupOriginal,
        onToggleProcessNestedFolders,
        onToggleSkipSrMarkedInputs,
        onSelectOutputFormat,
        onRefreshRemoteModels,
        onDownloadRemoteModel,
        onDismissDownloadError,
        onStartProcessing,
        onStartBatchQueue,
        onPauseBatchQueue,
        onRetryQueuedBatch,
        onRetryQueuedBatchFailedOnly,
        onOpenQueuedBatchOutput,
        onRemoveQueuedBatch,
        onClearBatchQueue,
        onCancelProcessing,
        onResumeProcessing,
        onRetryFailedItems,
        onResumePendingItem,
        onRetryFailedItem,
        onDiscardCheckpoint,
        onRestoreBackup,
        onCancelDownload,
        onResolveQueuedBatchOutputUri,
        onOpenHelp
    ) = callbacks
    val selectModelLabel = stringResource(R.string.home_model_select)
    val snackbarConnectingMessage = stringResource(R.string.home_snackbar_connecting)
    val snackbarConnectedMessage = stringResource(R.string.home_snackbar_connected)
    val snackbarPcUnreachableSwitchedMessage = stringResource(R.string.processing_remote_fallback)
    val snackbarPcUnreachableLocalMessage = stringResource(R.string.home_snackbar_pc_unreachable_local)

    val selectedInputs = state.selectedInputSources
    val corruptedSources = state.corruptedInputSources
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
    var showRemoteModelsSheet by rememberSaveable { mutableStateOf(false) }
    var queueSheetVisible by rememberSaveable { mutableStateOf(false) }
    var previewSource by rememberSaveable { mutableStateOf<String?>(null) }
    val context = LocalContext.current
    val openOutputFailedMessage = stringResource(R.string.queue_open_output_failed)
    // 新版 BottomSheet API：skipPartiallyExpanded=true 等价于启用集不含 PartiallyExpanded。
    val queueSheetState = rememberBottomSheetState(
        initialValue = SheetValue.Hidden,
        enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded)
    )

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

    // 滚动时顶栏随内容收放（enterAlways）：大图舞台向上滚动让位给参数区。
    val scrollBehavior = TopAppBarDefaults.enterAlwaysScrollBehavior()

    Scaffold(
        topBar = {
            UpixTopAppBar(
                title = stringResource(R.string.app_name),
                scrollBehavior = scrollBehavior,
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
                .nestedScroll(scrollBehavior.nestedScrollConnection)
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
                // 规范：滚动屏主区块间垂直节奏统一 lg。
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

            val followFormatLabel = stringResource(R.string.config_output_format_follow)
            ParameterCard(
                selectedModel = state.selectedModel,
                selectModelLabel = selectModelLabel,
                outputDirectoryLabel = state.selectedOutputDirectoryUri?.let { prettyOutputPath(it) }
                    ?: DefaultOutputPathLabel,
                overwriteOriginal = state.overwriteOriginalImage,
                backupOriginal = state.backupOriginalImage,
                processNestedFolders = state.processNestedFolders,
                skipSrMarkedInputs = state.skipSrMarkedInputs,
                outputFormatLabel = outputFormatLabel(
                    format = state.outputImageFormat,
                    followLabel = followFormatLabel
                ),
                outputFormatFollowsOriginal = state.outputImageFormat == OutputImageFormat.FOLLOW,
                outputFormatFollowLabel = followFormatLabel,
                availableScales = availableScales,
                onPickModel = { showModelSheet = true },
                onPickScale = onSelectScale,
                onPickFormat = { showFormatSheet = true },
                onSelectOutputFolder = onSelectOutputFolder,
                onToggleOverwriteOriginal = onToggleOverwriteOriginal,
                onToggleBackupOriginal = onToggleBackupOriginal,
                onToggleProcessNestedFolders = onToggleProcessNestedFolders,
                onToggleSkipSrMarkedInputs = onToggleSkipSrMarkedInputs
            )

            // 断点续传通知条：存在/消失做 fade + 纵向展开/收起。
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
                onStop = onCancelProcessing,
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
        ModelPickerSheet(
            models = state.models,
            selectedModelName = state.selectedModel?.name,
            onDismiss = { showModelSheet = false },
            onSelectModel = onSelectModel,
            // 打开下载 sheet 前先收起选择 sheet：两个 ModalBottomSheet 不叠放；
            // 下载 sheet 关闭时回到选择 sheet（Home 里下载唯一入口在此）。
            onOpenDownload = {
                showModelSheet = false
                showRemoteModelsSheet = true
            },
            // 指南是页面导航而非 sheet：同样先收起本 sheet，避免叠放。
            onOpenHelp = {
                showModelSheet = false
                onOpenHelp()
            }
        )
    }

    if (showFormatSheet) {
        val followLabel = stringResource(R.string.config_output_format_follow)
        ModalBottomSheet(
            onDismissRequest = { showFormatSheet = false },
            sheetState = rememberBottomSheetState(
                initialValue = SheetValue.Hidden,
                enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded)
            ),
            shape = MaterialTheme.shapes.extraLarge,
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Text(
                text = stringResource(R.string.home_select_output_format_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = Spacing.xl, vertical = Spacing.sm)
            )
            // 无任何 AVIF 编码能力（本地构建未带、已连 PC 也不支持）时，副标签
            // 如实提示；选项保留，本地运行会快速失败。
            val avifHint = stringResource(R.string.home_output_format_avif_remote_only)
            OutputImageFormat.entries.forEach { format ->
                val avifRemoteOnly = format == OutputImageFormat.AVIF &&
                    !state.avifEncodeCapable
                SelectSheetItem(
                    label = outputFormatLabel(format, followLabel),
                    sublabel = if (avifRemoteOnly) avifHint else null,
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
                // 点卡片本体 = 应用内预览输出图；仅 FolderOpen 图标才外部分享/打开。
                onPreviewQueuedBatchOutput = { batchId ->
                    val uri = onResolveQueuedBatchOutputUri(batchId)
                    if (uri.isNullOrBlank()) {
                        Toast.makeText(context, openOutputFailedMessage, Toast.LENGTH_SHORT).show()
                    } else {
                        previewSource = uri
                    }
                },
                onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                onRemoveQueuedBatch = onRemoveQueuedBatch,
                onClearBatchQueue = onClearBatchQueue,
                onDismiss = { queueSheetVisible = false }
            )
        }
    }

    previewSource?.let { source ->
        // 输入图与已完成输出图共用同一预览 sheet；输出为单图 URI，非损坏态。
        PreviewBottomSheet(
            source = source,
            isCorrupted = source in corruptedSources,
            onDismiss = { previewSource = null }
        )
    }

    if (showRemoteModelsSheet) {
        RemoteModelsSheet(
            remoteModels = remoteUiState.remoteModels,
            installedModelNames = state.models.map { it.name }.toSet(),
            downloads = remoteUiState.downloads,
            isLoading = remoteUiState.isLoadingRemote,
            errorMessage = remoteUiState.remoteError,
            onRefresh = onRefreshRemoteModels,
            onDownload = onDownloadRemoteModel,
            onDismiss = {
                showRemoteModelsSheet = false
                // 下载完/取消后回到选择 sheet，选刚下好的模型不用再点开一层。
                showModelSheet = true
            },
            onDismissError = onDismissDownloadError,
            onCancelDownload = onCancelDownload
        )
    }
}

/**
 * 底部悬浮 Dock 占位推导：[DockCtaHeight]（56dp）+ [Spacing.lg] 悬浮间距 +
 * [Spacing.xxl] 滚动间隙，供滚动列底部预留与 Snackbar 避让共用。
 * 全部由 token 组成（§3 纪律），无裸 dp 算术。
 */
private val BottomDockClearance = DockCtaHeight + Spacing.lg + Spacing.xxl

/** Snackbar 在 Dock 之上再留 [Spacing.md] 间隙。 */
private val SnackbarAboveDockPadding = DockCtaHeight + Spacing.lg + Spacing.md

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
            // clickable 自带 mergeDescendants：行内文字与 RadioButton 的选中态
            // 合并为一个可聚焦项；role 让 TalkBack 以单选项语义播报。
            .clickable(role = Role.RadioButton, onClick = onClick)
    ) {
        Text(text = label, style = labelStyle ?: MaterialTheme.typography.bodyLarge)
    }
}

/** 舞台处理 scrim/覆盖层：出现/消失做 fade。只接收 task，避免整包 UI state 参与 overlay 重组。 */
@Composable
private fun BoxScope.StageProcessingOverlay(
    visible: Boolean,
    task: io.upix.data.model.ProcessingTask?,
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
    connectedEndpoint: io.upix.data.model.RemoteEndpoint?
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

    // 状态点颜色切换做短 fade：状态变化可感知但不闪跳。
    val dotColor by animateColorAsState(
        targetValue = statusColor,
        animationSpec = tween(EnterTween),
        label = "connectionDotColor"
    )

    Row(
        modifier = Modifier.padding(end = Spacing.md),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        Box(
            modifier = Modifier
                .size(8.dp)
                .clip(CircleShape)
                .background(dotColor)
        )
        Text(
            text = text,
            style = MaterialTheme.typography.labelMedium,
            color = statusColor,
            // 顶栏空间有限：endpoint 过长时截断省略，不撑破标题区。
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

/** 默认输出目录展示名（未自选目录时参数卡路径行显示）。 */
private const val DefaultOutputPathLabel = "Pictures/Upix"

/**
 * SAF tree URI 转可读路径：取 tree/ 后的路径并 URL 解码，primary: 前缀
 * 表示内部存储根目录，剥去前缀（与默认展示一致，不带前导 /）。
 * 解析失败时原样返回。
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
        decoded.startsWith("primary:") -> decoded.removePrefix("primary:")
        else -> decoded.trimStart('/')
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
