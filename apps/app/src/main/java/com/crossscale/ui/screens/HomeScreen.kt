package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.ui.components.ProcessingProgress
import com.crossscale.ui.components.SettingsRow
import com.crossscale.ui.components.SheetActionItem
import com.crossscale.ui.components.SwitchRow
import com.crossscale.ui.screens.home.BottomActionBar
import com.crossscale.ui.screens.home.CheckpointSection
import com.crossscale.ui.screens.home.HomeCheckpointActions
import com.crossscale.ui.screens.home.HomeConnection
import com.crossscale.ui.screens.home.HomeQueueActions
import com.crossscale.ui.screens.home.HomeSetupActions
import com.crossscale.ui.screens.home.HeroArea
import com.crossscale.ui.screens.home.PreviewBottomSheet
import com.crossscale.ui.screens.home.QueueBottomSheetContent
import com.crossscale.ui.screens.home.countFailedQueueItems
import com.crossscale.ui.screens.home.countPendingQueueItems
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingUiState

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
    val selectedModel = state.selectedModel?.name ?: notSelectedLabel
    val selectedOutput = abbreviatePath(state.selectedOutputDirectoryUri ?: "/sdcard/Pictures/CrossScale")
    val queueItems = processingUiState.queueItems
    val pendingQueueCount = remember(queueItems) { countPendingQueueItems(queueItems) }
    val failedQueueCount = remember(queueItems) { countFailedQueueItems(queueItems) }

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
                .padding(horizontal = BorderlessTokens.ScreenPaddingHorizontal)
                .padding(bottom = 180.dp),
            verticalArrangement = Arrangement.spacedBy(18.dp)
        ) {
            Spacer(modifier = Modifier.height(6.dp))

            HeroArea(
                selectedInputs = selectedInputs,
                corruptedSources = corruptedSources,
                onAddImage = setupActions.onSelectImages,
                onAddFolder = setupActions.onSelectImageFolder,
                onClearInput = setupActions.onClearInputSources,
                onRemoveInput = setupActions.onRemoveInputSource,
                onPreviewInput = { source -> previewSource = source }
            )

            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    text = stringResource(R.string.home_parameters_section),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = BorderlessTokens.SectionTitleWeight
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha)
                )

                SettingsRow(
                    title = stringResource(R.string.home_model_setting_label),
                    value = selectedModel,
                    onClick = { showModelSheet = true }
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SettingsRow(
                    title = stringResource(R.string.home_output_path_label),
                    value = selectedOutput,
                    onClick = setupActions.onSelectOutputFolder,
                    trailing = {
                        TextButton(onClick = setupActions.onUseDefaultOutputFolder) {
                            Text(stringResource(R.string.home_use_default_output))
                        }
                    }
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SwitchRow(
                    title = stringResource(R.string.common_overwrite_original),
                    checked = state.overwriteOriginalImage,
                    onCheckedChange = setupActions.onToggleOverwriteOriginal
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SwitchRow(
                    title = stringResource(R.string.home_backup_original),
                    checked = state.backupOriginalImage,
                    enabled = state.overwriteOriginalImage,
                    onCheckedChange = setupActions.onToggleBackupOriginal
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha)
                )

                SwitchRow(
                    title = stringResource(R.string.home_include_subdirectories),
                    checked = state.processNestedFolders,
                    onCheckedChange = setupActions.onToggleProcessNestedFolders
                )
            }

            if (!processingUiState.isProcessing && processingUiState.checkpointSummary != null) {
                CheckpointSection(
                    processingUiState = processingUiState,
                    onResumeProcessing = checkpointActions.onResumeProcessing,
                    onRetryFailedItems = checkpointActions.onRetryFailedItems,
                    onResumePendingItem = checkpointActions.onResumePendingItem,
                    onRetryFailedItem = checkpointActions.onRetryFailedItem,
                    onDiscardCheckpoint = checkpointActions.onDiscardCheckpoint
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
            onStart = queueActions.onStartProcessing,
            onRestore = onRestoreBackup,
            onCancel = queueActions.onCancelProcessing
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
                text = stringResource(R.string.home_select_model_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp)
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
                onStartBatchQueue = queueActions.onStartBatchQueue,
                onPauseBatchQueue = queueActions.onPauseBatchQueue,
                onRetryQueuedBatch = queueActions.onRetryQueuedBatch,
                onRetryQueuedBatchFailedOnly = queueActions.onRetryQueuedBatchFailedOnly,
                onOpenQueuedBatchOutput = queueActions.onOpenQueuedBatchOutput,
                onRemoveQueuedBatch = queueActions.onRemoveQueuedBatch,
                onClearBatchQueue = queueActions.onClearBatchQueue
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

private fun abbreviatePath(path: String): String {
    val normalized = path.trim()
    if (normalized.length <= 22) {
        return normalized
    }
    return "${normalized.take(12)}...${normalized.takeLast(7)}"
}
