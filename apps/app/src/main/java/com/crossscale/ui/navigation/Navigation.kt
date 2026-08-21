package com.crossscale.ui.navigation

import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import com.crossscale.AppContainer
import com.crossscale.clearBatchQueue
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.openQueuedBatchOutput
import com.crossscale.removeQueuedBatch
import com.crossscale.restoreBackup
import com.crossscale.startProcessing
import com.crossscale.ui.input.rememberInputSelection
import com.crossscale.ui.screens.ConnectionScreen
import com.crossscale.ui.screens.HelpScreen
import com.crossscale.ui.screens.HomeScreen
import com.crossscale.ui.screens.LogsScreen
import com.crossscale.ui.screens.ModelsScreen
import com.crossscale.ui.screens.SettingsScreen
import com.crossscale.ui.theme.NavEnterTransition
import com.crossscale.ui.theme.NavExitTransition
import com.crossscale.viewmodel.DeviceViewModel
import com.crossscale.viewmodel.MainViewModel
import com.crossscale.viewmodel.ModelsViewModel
import com.crossscale.viewmodel.ProcessingViewModel

/**
 * 导航图自行解析三个 ViewModel（与 CrossScaleApp 同一 Activity 级
 * ViewModelStore，viewModel() 返回同一实例），并在各目的地的调用点
 * 直接绑定 VM 方法。跨 VM 的复合编排回调（startProcessing 等）位于
 * AppCallbacks.kt，此处仅做接线；不经过逐参数透传的回调墙。
 */
@ExperimentalGetImage
@Composable
fun CrossScaleNavigation(
    container: AppContainer,
    navController: NavHostController,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val mainViewModel: MainViewModel = viewModel(factory = MainViewModel.factory(container))
    val deviceViewModel: DeviceViewModel = viewModel(factory = DeviceViewModel.factory(container))
    val processingViewModel: ProcessingViewModel = viewModel(factory = ProcessingViewModel.factory(container))
    val modelsViewModel: ModelsViewModel = viewModel(factory = ModelsViewModel.factory(container))

    val mainUiState by mainViewModel.uiState.collectAsState()
    val deviceUiState by deviceViewModel.uiState.collectAsState()
    val processingUiState by processingViewModel.uiState.collectAsState()
    val modelsUiState by modelsViewModel.uiState.collectAsState()
    val connectionState: ConnectionState by deviceViewModel.connectionState.collectAsState()
    val connectedEndpoint: RemoteEndpoint? by deviceViewModel.connectedEndpoint.collectAsState()
    val remoteFallbackMessage: String? by processingViewModel.remoteFallbackMessage.collectAsState()
    val logRepository = container.logRepository
    val appSettings = container.appSettingsRepository

    var hasBackupAvailable by rememberSaveable { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    // 加密数据流开关(SharedPreferences 持久化, 默认关闭=吞吐优先)。
    val streamEncryptedState = rememberSaveable {
        mutableStateOf(appSettings.isStreamEncrypted())
    }
    val isStreamEncrypted = streamEncryptedState.value

    LaunchedEffect(mainUiState.selectedImagePath, mainUiState.processNestedFolders) {
        val path = mainUiState.selectedImagePath
        hasBackupAvailable = if (path != null) {
            processingViewModel.hasBackupFor(path, mainUiState.processNestedFolders)
        } else {
            false
        }
    }

    // App 启动即尝试自动重连上次的 PC (1:1 模型)。成功 -> Remote 模式;
    // 失败/无记录 -> Native 模式。mDNS 自动发现已移除。
    LaunchedEffect(Unit) {
        deviceViewModel.tryAutoReconnect()
    }

    val inputSelection = rememberInputSelection(
        mainViewModel = mainViewModel,
        processingViewModel = processingViewModel,
        mainUiState = mainUiState,
        logRepository = logRepository
    )

    NavHost(
        navController = navController,
        startDestination = AppScreen.Home.route,
        modifier = modifier,
        enterTransition = { NavEnterTransition },
        exitTransition = { NavExitTransition },
        popEnterTransition = { NavEnterTransition },
        popExitTransition = { NavExitTransition }
    ) {
        composable(AppScreen.Home.route) {
            DestinationContainer {
                HomeScreen(
                    state = mainUiState,
                    processingUiState = processingUiState,
                    connectionState = connectionState,
                    connectedEndpoint = connectedEndpoint,
                    remoteFallbackMessage = remoteFallbackMessage,
                    onClearRemoteFallbackMessage = processingViewModel::clearRemoteFallbackMessage,
                    onSelectModel = mainViewModel::selectModel,
                    onSelectScale = mainViewModel::selectModelByScale,
                    onSelectImages = inputSelection::pickImages,
                    onSelectImageFolder = inputSelection::pickImageFolder,
                    onClearInputSources = {
                        mainViewModel.clearSelectedImage()
                        processingViewModel.clearBatchQueue()
                    },
                    onRemoveInputSource = { source ->
                        mainViewModel.removeInputSource(source)
                        processingViewModel.removeQueuedBatchesBySource(source)
                    },
                    onSelectOutputFolder = inputSelection::pickOutputFolder,
                    onUseDefaultOutputFolder = mainViewModel::useDefaultOutputDirectory,
                    onToggleOverwriteOriginal = mainViewModel::setOverwriteOriginalImage,
                    onToggleBackupOriginal = mainViewModel::setBackupOriginalImage,
                    onToggleProcessNestedFolders = mainViewModel::setProcessNestedFolders,
                    onSelectOutputFormat = mainViewModel::setOutputImageFormat,
                    onOpenModelManagement = {
                        navController.navigate(AppScreen.SettingsModels.route)
                    },
                    onStartProcessing = {
                        startProcessing(
                            mainUiState = mainUiState,
                            processingUiState = processingUiState,
                            processingViewModel = processingViewModel,
                            logRepository = logRepository,
                            startQueueWithWritePermissionCheck = inputSelection::startQueueWithWritePermissionCheck
                        )
                    },
                    onStartBatchQueue = {
                        processingViewModel.syncPendingQueueDefaults(
                            distributedMode = mainUiState.useRemote,
                            outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
                            overwriteOriginal = mainUiState.overwriteOriginalImage,
                            backupOriginal = mainUiState.backupOriginalImage,
                            processNestedFolders = mainUiState.processNestedFolders
                        )
                        inputSelection.startQueueWithWritePermissionCheck(null)
                    },
                    onPauseBatchQueue = processingViewModel::pauseBatchQueue,
                    onRetryQueuedBatch = processingViewModel::retryQueuedBatch,
                    onRetryQueuedBatchFailedOnly = processingViewModel::retryQueuedBatchFailedOnly,
                    onOpenQueuedBatchOutput = { batchId ->
                        openQueuedBatchOutput(
                            context = context,
                            batchId = batchId,
                            processingViewModel = processingViewModel,
                            logRepository = logRepository
                        )
                    },
                    onRemoveQueuedBatch = { batchId ->
                        removeQueuedBatch(
                            batchId = batchId,
                            mainViewModel = mainViewModel,
                            processingViewModel = processingViewModel
                        )
                    },
                    onClearBatchQueue = {
                        clearBatchQueue(
                            mainUiState = mainUiState,
                            mainViewModel = mainViewModel,
                            processingViewModel = processingViewModel
                        )
                    },
                    onCancelProcessing = processingViewModel::cancelProcessing,
                    onResumeProcessing = processingViewModel::resumeLastProcessing,
                    onRetryFailedItems = processingViewModel::retryFailedItems,
                    onResumePendingItem = processingViewModel::resumePendingItem,
                    onRetryFailedItem = processingViewModel::retryFailedItem,
                    onDiscardCheckpoint = processingViewModel::discardCheckpoint,
                    hasBackupAvailable = hasBackupAvailable,
                    onRestoreBackup = {
                        restoreBackup(
                            context = context,
                            mainUiState = mainUiState,
                            processingViewModel = processingViewModel,
                            scope = scope,
                            onBackupAvailabilityChanged = { hasBackupAvailable = it }
                        )
                    }
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
                    onShowManualConnectDialog = deviceViewModel::showManualConnectDialog,
                    onManualConnect = deviceViewModel::connectManual,
                    onConnectByQr = deviceViewModel::connectByQrPayload,
                    onClearConnectionError = deviceViewModel::clearConnectionError,
                    onConfirmFingerprint = deviceViewModel::confirmFingerprintAndConnect,
                    onCancelFingerprint = deviceViewModel::cancelFingerprintConfirm
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
                    onOpenHelp = {
                        navController.navigate(AppScreen.Help.route)
                    },
                    isStreamEncrypted = isStreamEncrypted,
                    onStreamEncryptedChange = { enabled ->
                        appSettings.setStreamEncrypted(enabled)
                        streamEncryptedState.value = enabled
                    }
                )
            }
        }
        composable(AppScreen.SettingsModels.route) {
            DestinationContainer {
                ModelsScreen(
                    models = mainUiState.models,
                    selectedModel = mainUiState.selectedModel,
                    onSelectModel = mainViewModel::selectModel,
                    onRefreshModels = mainViewModel::refreshModels,
                    onImportModels = inputSelection::importModels,
                    onDeleteModel = mainViewModel::deleteModel,
                    modelDirectoryPath = mainUiState.modelDirectoryPath,
                    remoteUiState = modelsUiState,
                    onRefreshRemoteModels = modelsViewModel::refreshRemoteModels,
                    onDownloadRemoteModel = modelsViewModel::downloadRemoteModel,
                    onDismissDownloadError = modelsViewModel::dismissDownloadError
                )
            }
        }
        composable(AppScreen.SettingsLogs.route) {
            val logs by logRepository.logs.collectAsState()
            DestinationContainer {
                LogsScreen(
                    logs = logs,
                    onClearLogs = logRepository::clear
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
