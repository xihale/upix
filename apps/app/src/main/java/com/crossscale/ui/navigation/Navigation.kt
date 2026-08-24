package com.crossscale.ui.navigation

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import com.crossscale.data.model.ProcessingStatus
import androidx.lifecycle.compose.collectAsStateWithLifecycle
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
import com.crossscale.openQueuedBatchOutput
import com.crossscale.removeQueuedBatch
import com.crossscale.restoreBackup
import com.crossscale.startProcessing
import com.crossscale.ui.input.rememberInputSelection
import com.crossscale.ui.components.SecondaryScreenScaffold
import com.crossscale.ui.screens.ConnectionScreen
import com.crossscale.ui.screens.HelpScreen
import com.crossscale.ui.screens.HomeCallbacks
import com.crossscale.ui.screens.HomeScreen
import com.crossscale.ui.screens.LogsScreen
import com.crossscale.ui.screens.ModelsScreen
import com.crossscale.ui.screens.SettingsDetail
import com.crossscale.ui.screens.SettingsScreen
import com.crossscale.ui.theme.NavEnterTransition
import com.crossscale.ui.theme.NavExitTransition
import com.crossscale.ui.theme.NavPopEnterTransition
import com.crossscale.ui.theme.NavPopExitTransition
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

    // 仅 mainUiState（低频：选择/配置变化）在导航层收集；高频流（处理进度、
    // 下载进度、连接状态）下沉到各自目的地收集，避免任一更新重跑整个 NavHost。
    val mainUiState by mainViewModel.uiState.collectAsStateWithLifecycle()
    val logRepository = container.logRepository
    val appSettings = container.appSettingsRepository

    val scope = rememberCoroutineScope()

    // 加密数据流开关(SharedPreferences 持久化, 默认关闭=吞吐优先)。
    val streamEncryptedState = rememberSaveable {
        mutableStateOf(appSettings.isStreamEncrypted())
    }
    val isStreamEncrypted = streamEncryptedState.value

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
        popEnterTransition = { NavPopEnterTransition },
        popExitTransition = { NavPopExitTransition }
    ) {
        composable(AppScreen.Home.route) {
            // 高频流在目的地内收集：处理进度/下载进度/连接状态只失效本目的地。
            val processingUiState by processingViewModel.uiState.collectAsStateWithLifecycle()
            val modelsUiState by modelsViewModel.uiState.collectAsStateWithLifecycle()
            val connectionState by deviceViewModel.connectionState.collectAsStateWithLifecycle()
            val connectedEndpoint by deviceViewModel.connectedEndpoint.collectAsStateWithLifecycle()
            val remoteFallbackMessage by processingViewModel.remoteFallbackMessage.collectAsStateWithLifecycle()
            var hasBackupAvailable by rememberSaveable { mutableStateOf(false) }

            LaunchedEffect(mainUiState.selectedImagePath, mainUiState.processNestedFolders) {
                val path = mainUiState.selectedImagePath
                hasBackupAvailable = if (path != null) {
                    processingViewModel.hasBackupFor(path, mainUiState.processNestedFolders)
                } else {
                    false
                }
            }

            // 处理完成后清空主页 hero（已选图片）：按 taskId 消费一次，
            // 旋转/返回重放不再清掉用户新选的图。
            var lastClearedTaskId by rememberSaveable { mutableStateOf<String?>(null) }
            LaunchedEffect(processingUiState.task?.id, processingUiState.task?.status) {
                val task = processingUiState.task
                if (task?.status == ProcessingStatus.COMPLETED && lastClearedTaskId != task.id) {
                    lastClearedTaskId = task.id
                    mainViewModel.clearSelectedImage()
                }
            }

            DestinationContainer {
                HomeScreen(
                    state = mainUiState,
                    processingUiState = processingUiState,
                    connectionState = connectionState,
                    connectedEndpoint = connectedEndpoint,
                    remoteFallbackMessage = remoteFallbackMessage,
                    remoteUiState = modelsUiState,
                    hasBackupAvailable = hasBackupAvailable,
                    callbacks = HomeCallbacks(
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
                        onRefreshRemoteModels = modelsViewModel::refreshRemoteModels,
                        onDownloadRemoteModel = modelsViewModel::downloadRemoteModel,
                        onDismissDownloadError = modelsViewModel::dismissDownloadError,
                        onStartProcessing = {
                            startProcessing(
                                context = context,
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
                        onRestoreBackup = {
                            restoreBackup(
                                context = context,
                                mainUiState = mainUiState,
                                processingViewModel = processingViewModel,
                                scope = scope,
                                onBackupAvailabilityChanged = { hasBackupAvailable = it }
                            )
                        },
                        onCancelDownload = modelsViewModel::cancelDownload,
                        onResolveQueuedBatchOutputUri = processingViewModel::outputUriForQueuedBatch
                    )
                )
            }
        }
        composable(AppScreen.Devices.route) {
            val deviceUiState by deviceViewModel.uiState.collectAsStateWithLifecycle()
            val connectionState by deviceViewModel.connectionState.collectAsStateWithLifecycle()
            val connectedEndpoint by deviceViewModel.connectedEndpoint.collectAsStateWithLifecycle()
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
            // medium+ 双栏 detail pane 直接嵌入子页：在此收集所需状态。
            val modelsUiState by modelsViewModel.uiState.collectAsStateWithLifecycle()
            val logs by logRepository.logs.collectAsStateWithLifecycle()
            DestinationContainer {
                SettingsScreen(
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
                    },
                    detailPaneContent = { detail ->
                        when (detail) {
                            SettingsDetail.MODELS -> ModelsScreen(
                                library = mainUiState.library,
                                phoneModelNames = mainUiState.models.map { it.name }.toSet(),
                                pcConnected = mainUiState.useRemote,
                                onRefreshModels = mainViewModel::refreshModels,
                                onImportModels = inputSelection::importModels,
                                onDeleteModel = mainViewModel::deleteModel,
                                modelDirectoryPath = mainUiState.modelDirectoryPath,
                                uploadingToPc = mainUiState.uploadingToPc,
                                downloadingFromPc = mainUiState.downloadingFromPc,
                                onUploadToPc = mainViewModel::uploadModelToPc,
                                onDownloadFromPc = mainViewModel::downloadModelFromPc,
                                remoteUiState = modelsUiState,
                                onRefreshRemoteModels = modelsViewModel::refreshRemoteModels,
                                onDownloadRemoteModel = modelsViewModel::downloadRemoteModel,
                                onDismissDownloadError = modelsViewModel::dismissDownloadError,
                                onCancelDownload = modelsViewModel::cancelDownload
                            )
                            SettingsDetail.LOGS -> LogsScreen(
                                logs = logs,
                                onClearLogs = logRepository::clear
                            )
                            SettingsDetail.HELP -> HelpScreen()
                        }
                    }
                )
            }
        }
        composable(AppScreen.SettingsModels.route) {
            val modelsUiState by modelsViewModel.uiState.collectAsStateWithLifecycle()
            SecondaryScreenScaffold(
                titleRes = AppScreen.SettingsModels.titleRes,
                onBack = { navController.popBackStack() }
            ) {
                ModelsScreen(
                    library = mainUiState.library,
                    phoneModelNames = mainUiState.models.map { it.name }.toSet(),
                    pcConnected = mainUiState.useRemote,
                    onRefreshModels = mainViewModel::refreshModels,
                    onImportModels = inputSelection::importModels,
                    onDeleteModel = mainViewModel::deleteModel,
                    modelDirectoryPath = mainUiState.modelDirectoryPath,
                    uploadingToPc = mainUiState.uploadingToPc,
                    downloadingFromPc = mainUiState.downloadingFromPc,
                    onUploadToPc = mainViewModel::uploadModelToPc,
                    onDownloadFromPc = mainViewModel::downloadModelFromPc,
                    remoteUiState = modelsUiState,
                    onRefreshRemoteModels = modelsViewModel::refreshRemoteModels,
                    onDownloadRemoteModel = modelsViewModel::downloadRemoteModel,
                    onDismissDownloadError = modelsViewModel::dismissDownloadError,
                    onCancelDownload = modelsViewModel::cancelDownload
                )
            }
        }
        composable(AppScreen.SettingsLogs.route) {
            val logs by logRepository.logs.collectAsStateWithLifecycle()
            SecondaryScreenScaffold(
                titleRes = AppScreen.SettingsLogs.titleRes,
                onBack = { navController.popBackStack() }
            ) {
                LogsScreen(
                    logs = logs,
                    onClearLogs = logRepository::clear
                )
            }
        }
        composable(AppScreen.Help.route) {
            SecondaryScreenScaffold(
                titleRes = AppScreen.Help.titleRes,
                onBack = { navController.popBackStack() }
            ) {
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
