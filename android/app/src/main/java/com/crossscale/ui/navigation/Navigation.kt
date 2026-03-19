package com.crossscale.ui.navigation

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.camera.core.ExperimentalGetImage
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import kotlinx.coroutines.flow.StateFlow
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.screens.DevicesScreen
import com.crossscale.ui.screens.HelpScreen
import com.crossscale.ui.screens.HomeScreen
import com.crossscale.ui.screens.LogsScreen
import com.crossscale.ui.screens.ModelsScreen
import com.crossscale.ui.screens.SettingsScreen
import com.crossscale.viewmodel.DeviceUiState
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingUiState

@ExperimentalGetImage
@Composable
fun CrossScaleNavigation(
    navController: NavHostController,
    mainUiState: MainUiState,
    deviceUiState: DeviceUiState,
    processingUiState: ProcessingUiState,
    logsFlow: StateFlow<List<LogEntry>>,
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
    onRestoreBackup: () -> Unit = {},
    onRefreshModels: () -> Unit,
    onImportModels: () -> Unit,
    onDeleteModel: (ModelInfo) -> Unit,
    onStartDiscovery: () -> Unit,
    onStopDiscovery: () -> Unit,
    onConnectDevice: (String) -> Unit,
    onDisconnectDevice: (String) -> Unit,
    onAdjustDeviceWeight: (String, Float) -> Unit,
    onShowManualConnectDialog: (Boolean) -> Unit,
    onManualConnect: (String, String, String) -> Unit,
    onConnectByQr: (String) -> Unit,
    onClearLogs: () -> Unit,
    onOpenModelRelease: () -> Unit,
    onOpenHelp: () -> Unit,
    modifier: Modifier = Modifier
) {
    NavHost(
        navController = navController,
        startDestination = AppScreen.Home.route,
        modifier = modifier
    ) {
        composable(AppScreen.Home.route) {
            DestinationContainer {
                HomeScreen(
                    state = mainUiState,
                    processingUiState = processingUiState,
                    onSelectModel = onSelectModel,
                    onSelectImages = onSelectImages,
                    onSelectImageFolder = onSelectImageFolder,
                    onClearInputSources = onClearInputSources,
                    onRemoveInputSource = onRemoveInputSource,
                    onMoveInputSourceUp = onMoveInputSourceUp,
                    onMoveInputSourceDown = onMoveInputSourceDown,
                    onSelectOutputFolder = onSelectOutputFolder,
                    onUseDefaultOutputFolder = onUseDefaultOutputFolder,
                    onToggleOverwriteOriginal = onToggleOverwriteOriginal,
                    onToggleBackupOriginal = onToggleBackupOriginal,
                    onToggleProcessNestedFolders = onToggleProcessNestedFolders,
                    onStartProcessing = onStartProcessing,
                    onStartBatchQueue = onStartBatchQueue,
                    onPauseBatchQueue = onPauseBatchQueue,
                    onRetryQueuedBatch = onRetryQueuedBatch,
                    onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                    onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                    onRemoveQueuedBatch = onRemoveQueuedBatch,
                    onClearBatchQueue = onClearBatchQueue,
                    onResumeProcessing = onResumeProcessing,
                    onRetryFailedItems = onRetryFailedItems,
                    onResumePendingItem = onResumePendingItem,
                    onRetryFailedItem = onRetryFailedItem,
                    onDiscardCheckpoint = onDiscardCheckpoint,
                    onCancelProcessing = onCancelProcessing,
                    hasBackupAvailable = hasBackupAvailable,
                    onRestoreBackup = onRestoreBackup
                )
            }
        }
        composable(AppScreen.Devices.route) {
            DestinationContainer {
                DevicesScreen(
                    state = deviceUiState,
                    onStartDiscovery = onStartDiscovery,
                    onStopDiscovery = onStopDiscovery,
                    onConnectDevice = onConnectDevice,
                    onDisconnectDevice = onDisconnectDevice,
                    onAdjustDeviceWeight = onAdjustDeviceWeight,
                    onShowManualConnectDialog = onShowManualConnectDialog,
                    onManualConnect = onManualConnect,
                    onConnectByQr = onConnectByQr
                )
            }
        }
        composable(AppScreen.Settings.route) {
            DestinationContainer {
                SettingsScreen(
                    modelDirectoryPath = mainUiState.modelDirectoryPath,
                    selectedModelName = mainUiState.selectedModel?.name,
                    onOpenModels = {
                        navController.navigate(AppScreen.SettingsModels.route)
                    },
                    onOpenLogs = {
                        navController.navigate(AppScreen.SettingsLogs.route)
                    },
                    onOpenHelp = onOpenHelp
                )
            }
        }
        composable(AppScreen.SettingsModels.route) {
            DestinationContainer {
                ModelsScreen(
                    models = mainUiState.models,
                    selectedModel = mainUiState.selectedModel,
                    onSelectModel = onSelectModel,
                    onRefreshModels = onRefreshModels,
                    onImportModels = onImportModels,
                    onDeleteModel = onDeleteModel,
                    modelDirectoryPath = mainUiState.modelDirectoryPath,
                    onOpenModelRelease = onOpenModelRelease
                )
            }
        }
        composable(AppScreen.SettingsLogs.route) {
            val logs by logsFlow.collectAsState()
            DestinationContainer {
                LogsScreen(
                    logs = logs,
                    onClearLogs = onClearLogs
                )
            }
        }
        composable(AppScreen.Help.route) {
            DestinationContainer {
                HelpScreen()
            }
        }
    }
}

@Composable
private fun DestinationContainer(content: @Composable () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
    ) {
        content()
    }
}
