package com.crossscale.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.ui.components.SheetActionItem
import com.crossscale.ui.components.StageProgressOverlay
import com.crossscale.ui.screens.home.BottomActionDock
import com.crossscale.ui.screens.home.CheckpointSection
import com.crossscale.ui.screens.home.HomeCheckpointActions
import com.crossscale.ui.screens.home.HomeConnection
import com.crossscale.ui.screens.home.HomeQueueActions
import com.crossscale.ui.screens.home.HomeSetupActions
import com.crossscale.ui.screens.home.ImageStage
import com.crossscale.ui.screens.home.ParameterCard
import com.crossscale.ui.screens.home.PreviewBottomSheet
import com.crossscale.ui.screens.home.QueueBottomSheetContent
import com.crossscale.ui.screens.home.countFailedQueueItems
import com.crossscale.ui.screens.home.countPendingQueueItems
import com.crossscale.ui.theme.EnterMotionSpec
import com.crossscale.ui.theme.EnterSizeSpec
import com.crossscale.ui.theme.Spacing
import com.crossscale.viewmodel.MainUiState
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
    connection: HomeConnection,
    setupActions: HomeSetupActions,
    queueActions: HomeQueueActions,
    checkpointActions: HomeCheckpointActions,
    hasBackupAvailable: Boolean = false,
    onRestoreBackup: () -> Unit = {}
) {
    val connectionState = connection.connectionState
    val connectedEndpoint = connection.connectedEndpoint
    val remoteFallbackMessage = connection.remoteFallbackMessage
    val notSelectedLabel = stringResource(R.string.common_not_selected)
    val snackbarConnectingMessage = stringResource(R.string.home_snackbar_connecting)
    val snackbarConnectedMessage = stringResource(R.string.home_snackbar_connected)
    val snackbarPcUnreachableSwitchedMessage = stringResource(R.string.home_snackbar_pc_unreachable_switched)
    val snackbarPcUnreachableLocalMessage = stringResource(R.string.home_snackbar_pc_unreachable_local)

    val selectedInputs = state.selectedInputSources
    val corruptedSources = state.corruptedInputSources
    val selectedOutput = abbreviatePath(state.selectedOutputDirectoryUri ?: "/sdcard/Pictures/CrossScale")
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = remember(queueItems) { countPendingQueueItems(queueItems) }
    val failedQueueCount = remember(queueItems) { countFailedQueueItems(queueItems) }

    var showModelSheet by rememberSaveable { mutableStateOf(false) }
    var queueSheetVisible by rememberSaveable { mutableStateOf(false) }
    var previewSource by rememberSaveable { mutableStateOf<String?>(null) }
    val queueSheetState = androidx.compose.material3.rememberModalBottomSheetState(skipPartiallyExpanded = true)

    val snackbarHostState = remember { SnackbarHostState() }
    val prevConnectionState = remember { mutableStateOf(connectionState) }
    val isProcessing = processingUiState.isProcessing
    LaunchedEffect(connectionState) {
        val previous = prevConnectionState.value
        if (previous != connectionState) {
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
            prevConnectionState.value = connectionState
        }
    }

    // PC 掉线回退（处理中）：ProcessingViewModel 在远程不可达、已切换本地处理时
    // 发出一次性信号；展示 snackbar 后清除，避免重复弹出。
    LaunchedEffect(remoteFallbackMessage) {
        val message = remoteFallbackMessage
        if (!message.isNullOrBlank()) {
            snackbarHostState.showSnackbar(message)
            connection.onClearRemoteFallbackMessage()
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = Spacing.xl)
                .padding(bottom = 140.dp),
            verticalArrangement = Arrangement.spacedBy(Spacing.lg)
        ) {
            HomeIdentityBar(
                connectionState = connectionState,
                connectedEndpoint = connectedEndpoint
            )

            ImageStage(
                selectedInputs = selectedInputs,
                corruptedSources = corruptedSources,
                isProcessing = isProcessing,
                onAddImage = setupActions.onSelectImages,
                onAddFolder = setupActions.onSelectImageFolder,
                onClearInput = setupActions.onClearInputSources,
                onRemoveInput = setupActions.onRemoveInputSource,
                onPreviewInput = { source -> previewSource = source },
                overlay = {
                    StageProcessingOverlay(
                        visible = isProcessing,
                        uiState = processingUiState,
                        onCancel = queueActions.onCancelProcessing
                    )
                }
            )

            ParameterCard(
                connectionState = connectionState,
                connectedEndpoint = connectedEndpoint,
                selectedModel = state.selectedModel,
                notSelectedLabel = notSelectedLabel,
                outputDirectoryLabel = selectedOutput,
                overwriteOriginal = state.overwriteOriginalImage,
                backupOriginal = state.backupOriginalImage,
                processNestedFolders = state.processNestedFolders,
                onPickModel = { showModelSheet = true },
                onSelectOutputFolder = setupActions.onSelectOutputFolder,
                onUseDefaultOutputFolder = setupActions.onUseDefaultOutputFolder,
                onToggleOverwriteOriginal = setupActions.onToggleOverwriteOriginal,
                onToggleBackupOriginal = setupActions.onToggleBackupOriginal,
                onToggleProcessNestedFolders = setupActions.onToggleProcessNestedFolders
            )

            // 断点续传卡：存在/消失做 fade + 纵向展开/收起（200ms ease-out）。
            AnimatedVisibility(
                visible = !isProcessing && processingUiState.checkpointSummary != null,
                enter = fadeIn(EnterMotionSpec) + expandVertically(EnterSizeSpec),
                exit = shrinkVertically(EnterSizeSpec) + fadeOut(EnterMotionSpec)
            ) {
                CheckpointSection(
                    processingUiState = processingUiState,
                    onResumeProcessing = checkpointActions.onResumeProcessing,
                    onRetryFailedItems = checkpointActions.onRetryFailedItems,
                    onResumePendingItem = checkpointActions.onResumePendingItem,
                    onRetryFailedItem = checkpointActions.onRetryFailedItem,
                    onDiscardCheckpoint = checkpointActions.onDiscardCheckpoint
                )
            }
        }

        BottomActionDock(
            modifier = Modifier.align(Alignment.BottomCenter),
            isProcessing = isProcessing,
            pendingQueueCount = pendingQueueCount,
            failedQueueCount = failedQueueCount,
            hasBackupAvailable = hasBackupAvailable,
            hasQueueItems = queueItems.isNotEmpty(),
            onShowQueue = {
                queueSheetVisible = true
            },
            onStart = queueActions.onStartProcessing,
            onRestore = onRestoreBackup
        )

        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 120.dp)
        )
    }

    if (showModelSheet) {
        ModalBottomSheet(
            onDismissRequest = { showModelSheet = false },
            shape = MaterialTheme.shapes.extraLarge,
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Text(
                text = stringResource(R.string.home_select_model_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = Spacing.xl, vertical = Spacing.sm)
            )
            state.models.forEach { model ->
                SheetActionItem(
                    label = model.name,
                    subtitle = "x${model.scaleFactor} · tile ${model.tileSize}"
                ) {
                    showModelSheet = false
                    setupActions.onSelectModel(model)
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
                onStartBatchQueue = queueActions.onStartBatchQueue,
                onPauseBatchQueue = queueActions.onPauseBatchQueue,
                onRetryQueuedBatch = queueActions.onRetryQueuedBatch,
                onRetryQueuedBatchFailedOnly = queueActions.onRetryQueuedBatchFailedOnly,
                onOpenQueuedBatchOutput = queueActions.onOpenQueuedBatchOutput,
                onRemoveQueuedBatch = queueActions.onRemoveQueuedBatch,
                onClearBatchQueue = queueActions.onClearBatchQueue,
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
}

/** 舞台处理 scrim/覆盖层：出现/消失做 fade（200ms ease-out）。 */
@Composable
private fun BoxScope.StageProcessingOverlay(
    visible: Boolean,
    uiState: ProcessingUiState,
    onCancel: () -> Unit
) {
    androidx.compose.animation.AnimatedVisibility(
        visible = visible,
        enter = fadeIn(EnterMotionSpec),
        exit = fadeOut(EnterMotionSpec)
    ) {
        StageProgressOverlay(
            uiState = uiState,
            onCancel = onCancel
        )
    }
}

/** 顶部标识条：品牌名 + 连接状态 pill（直接消费 HomeConnection 现成数据）。 */@Composable
private fun HomeIdentityBar(
    connectionState: ConnectionState,
    connectedEndpoint: com.crossscale.data.model.RemoteEndpoint?
) {
    val (pillLabelRes, pillColor) = when (connectionState) {
        ConnectionState.CONNECTED -> R.string.config_mode_remote to MaterialTheme.colorScheme.primary
        ConnectionState.DISCONNECTED -> R.string.config_mode_local to MaterialTheme.colorScheme.onSurfaceVariant
        ConnectionState.CONNECTING -> R.string.config_mode_connecting to MaterialTheme.colorScheme.tertiary
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = Spacing.md, bottom = Spacing.xs),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = stringResource(R.string.app_name),
            style = MaterialTheme.typography.titleLarge
        )
        Surface(
            shape = CircleShape,
            color = pillColor.copy(alpha = 0.14f),
            contentColor = pillColor
        ) {
            Row(
                modifier = Modifier.padding(horizontal = Spacing.md, vertical = Spacing.xs),
                horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Box(
                    modifier = Modifier
                        .size(6.dp)
                        .background(color = pillColor, shape = CircleShape)
                )
                val pillLabel = stringResource(pillLabelRes)
                val endpoint = connectedEndpoint?.let { "${it.host}:${it.port}" }
                Text(
                    text = if (connectionState == ConnectionState.CONNECTED && endpoint != null) {
                        "$pillLabel $endpoint"
                    } else {
                        pillLabel
                    },
                    style = MaterialTheme.typography.labelMedium,
                    maxLines = 1
                )
            }
        }
    }
}

private fun abbreviatePath(path: String): String {
    val normalized = path.trim()
    if (normalized.length <= 22) {
        return normalized
    }
    return "${normalized.take(12)}...${normalized.takeLast(7)}"
}
