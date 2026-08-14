package com.crossscale.ui.navigation

import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import com.crossscale.AppContainer
import com.crossscale.CrossScaleApplication
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.ui.screens.ConnectionScreen
import com.crossscale.ui.screens.HelpScreen
import com.crossscale.ui.screens.HomeScreen
import com.crossscale.ui.screens.LogsScreen
import com.crossscale.ui.screens.ModelsScreen
import com.crossscale.ui.screens.SettingsScreen
import com.crossscale.ui.screens.home.HomeCheckpointActions
import com.crossscale.ui.screens.home.HomeConnection
import com.crossscale.ui.screens.home.HomeQueueActions
import com.crossscale.ui.screens.home.HomeSetupActions
import com.crossscale.viewmodel.DeviceUiState
import com.crossscale.viewmodel.DeviceViewModel
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.ProcessingUiState
import com.crossscale.viewmodel.ProcessingViewModel
import kotlinx.coroutines.flow.StateFlow

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
    onShowManualConnectDialog: (Boolean) -> Unit,
    onManualConnect: (String, String) -> Unit,
    onConnectByQr: (String) -> Unit,
    onClearConnectionError: () -> Unit,
    onConfirmFingerprint: () -> Unit,
    onCancelFingerprint: () -> Unit,
    onStreamEncryptedChange: (Boolean) -> Unit,
    isStreamEncrypted: Boolean,
    onClearLogs: () -> Unit,
    onOpenModelRelease: () -> Unit,
    onOpenHelp: () -> Unit,
    modifier: Modifier = Modifier
) {
    // 1:1 phone<->PC: the nav graph no longer takes connectionState /
    // connectedEndpoint / onDisconnect as threaded callback params. It pulls the
    // Activity-owned DeviceViewModel + ProcessingViewModel (same ViewModelStore as
    // CrossScaleApp, so viewModel() returns the very same instances) and consumes
    // the contract StateFlows directly. The connection screen's disconnect action
    // likewise binds straight to deviceViewModel.disconnect().
    val appContext = LocalContext.current
    val appContainer: AppContainer = remember(appContext) {
        (appContext.applicationContext as CrossScaleApplication).container
    }
    val deviceViewModel: DeviceViewModel = viewModel(factory = DeviceViewModel.factory(appContainer))
    val processingViewModel: ProcessingViewModel = viewModel(factory = ProcessingViewModel.factory(appContainer))
    val connectionState: ConnectionState by deviceViewModel.connectionState.collectAsState()
    val connectedEndpoint: RemoteEndpoint? by deviceViewModel.connectedEndpoint.collectAsState()
    val remoteFallbackMessage: String? by processingViewModel.remoteFallbackMessage.collectAsState()

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
                    connection = HomeConnection(
                        connectionState = connectionState,
                        connectedEndpoint = connectedEndpoint,
                        remoteFallbackMessage = remoteFallbackMessage,
                        onClearRemoteFallbackMessage = processingViewModel::clearRemoteFallbackMessage
                    ),
                    setupActions = HomeSetupActions(
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
                        onToggleProcessNestedFolders = onToggleProcessNestedFolders
                    ),
                    queueActions = HomeQueueActions(
                        onStartProcessing = onStartProcessing,
                        onStartBatchQueue = onStartBatchQueue,
                        onPauseBatchQueue = onPauseBatchQueue,
                        onRetryQueuedBatch = onRetryQueuedBatch,
                        onRetryQueuedBatchFailedOnly = onRetryQueuedBatchFailedOnly,
                        onOpenQueuedBatchOutput = onOpenQueuedBatchOutput,
                        onRemoveQueuedBatch = onRemoveQueuedBatch,
                        onClearBatchQueue = onClearBatchQueue,
                        onCancelProcessing = onCancelProcessing
                    ),
                    checkpointActions = HomeCheckpointActions(
                        onResumeProcessing = onResumeProcessing,
                        onRetryFailedItems = onRetryFailedItems,
                        onResumePendingItem = onResumePendingItem,
                        onRetryFailedItem = onRetryFailedItem,
                        onDiscardCheckpoint = onDiscardCheckpoint
                    ),
                    hasBackupAvailable = hasBackupAvailable,
                    onRestoreBackup = onRestoreBackup
                )
            }
        }
        composable(AppScreen.Devices.route) {
            DestinationContainer {
                ConnectionScreen(
                    state = deviceUiState,
                    connectionState = connectionState,
                    connectedEndpoint = connectedEndpoint,
                    onDisconnect = deviceViewModel::disconnect,
                    onShowManualConnectDialog = onShowManualConnectDialog,
                    onManualConnect = onManualConnect,
                    onConnectByQr = onConnectByQr,
                    onClearConnectionError = onClearConnectionError,
                    onConfirmFingerprint = onConfirmFingerprint,
                    onCancelFingerprint = onCancelFingerprint
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
                    onOpenHelp = onOpenHelp,
                    isStreamEncrypted = isStreamEncrypted,
                    onStreamEncryptedChange = onStreamEncryptedChange
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
