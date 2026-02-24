package com.crossscale.ui.navigation

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.screens.DevicesScreen
import com.crossscale.ui.screens.HomeScreen
import com.crossscale.ui.screens.LogsScreen
import com.crossscale.ui.screens.ModelsScreen
import com.crossscale.viewmodel.DeviceUiState
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingUiState

@Composable
fun CrossScaleNavigation(
    navController: NavHostController,
    mainUiState: MainUiState,
    deviceUiState: DeviceUiState,
    processingUiState: ProcessingUiState,
    logs: List<LogEntry>,
    onSelectModel: (ModelInfo) -> Unit,
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
    onCancelProcessing: () -> Unit,
    onRefreshModels: () -> Unit,
    onDeleteModel: (ModelInfo) -> Unit,
    onStartDiscovery: () -> Unit,
    onStopDiscovery: () -> Unit,
    onConnectDevice: (String) -> Unit,
    onDisconnectDevice: (String) -> Unit,
    onShowManualConnectDialog: (Boolean) -> Unit,
    onManualConnect: (String, String, String) -> Unit,
    onConnectByQr: (String) -> Unit,
    onClearLogs: () -> Unit,
    modifier: Modifier = Modifier
) {
    NavHost(
        navController = navController,
        startDestination = AppScreen.Home.route,
        modifier = modifier
    ) {
        composable(AppScreen.Home.route) {
            HomeScreen(
                state = mainUiState,
                processingUiState = processingUiState,
                onSelectSingleImage = onSelectSingleImage,
                onSelectMultipleImages = onSelectMultipleImages,
                onSelectImageFolder = onSelectImageFolder,
                onClearInputSources = onClearInputSources,
                onRemoveInputSource = onRemoveInputSource,
                onMoveInputSourceUp = onMoveInputSourceUp,
                onMoveInputSourceDown = onMoveInputSourceDown,
                onSelectOutputFolder = onSelectOutputFolder,
                onUseInternalOutputFolder = onUseInternalOutputFolder,
                onStartProcessing = onStartProcessing,
                onEnqueueBatch = onEnqueueBatch,
                onStartBatchQueue = onStartBatchQueue,
                onPauseBatchQueue = onPauseBatchQueue,
                onRetryQueuedBatch = onRetryQueuedBatch,
                onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                onRemoveQueuedBatch = onRemoveQueuedBatch,
                onClearBatchQueue = onClearBatchQueue,
                onResumeProcessing = onResumeProcessing,
                onRetryFailedItems = onRetryFailedItems,
                onResumePendingItem = onResumePendingItem,
                onRetryFailedItem = onRetryFailedItem,
                onDiscardCheckpoint = onDiscardCheckpoint,
                onCancelProcessing = onCancelProcessing
            )
        }
        composable(AppScreen.Devices.route) {
            DevicesScreen(
                state = deviceUiState,
                onStartDiscovery = onStartDiscovery,
                onStopDiscovery = onStopDiscovery,
                onConnectDevice = onConnectDevice,
                onDisconnectDevice = onDisconnectDevice,
                onShowManualConnectDialog = onShowManualConnectDialog,
                onManualConnect = onManualConnect,
                onConnectByQr = onConnectByQr
            )
        }
        composable(AppScreen.Models.route) {
            ModelsScreen(
                models = mainUiState.models,
                selectedModel = mainUiState.selectedModel,
                onSelectModel = onSelectModel,
                onRefreshModels = onRefreshModels,
                onDeleteModel = onDeleteModel
            )
        }
        composable(AppScreen.Logs.route) {
            LogsScreen(
                logs = logs,
                onClearLogs = onClearLogs
            )
        }
    }
}
