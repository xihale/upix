package com.crossscale

import android.content.Intent
import android.net.Uri
import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.rememberNavController
import com.crossscale.ui.components.BottomNavigationBar
import com.crossscale.ui.input.rememberInputSelection
import com.crossscale.ui.navigation.AppScreen
import com.crossscale.ui.navigation.CrossScaleNavigation
import com.crossscale.viewmodel.DeviceViewModel
import com.crossscale.viewmodel.MainViewModel
import com.crossscale.viewmodel.ProcessingViewModel

@OptIn(ExperimentalMaterial3Api::class)
@ExperimentalGetImage
@Composable
fun CrossScaleApp(container: AppContainer) {
    val context = LocalContext.current
    val navController = rememberNavController()
    val mainViewModel: MainViewModel = viewModel(factory = MainViewModel.factory(container))
    val deviceViewModel: DeviceViewModel = viewModel(factory = DeviceViewModel.factory(container))
    val processingViewModel: ProcessingViewModel = viewModel(factory = ProcessingViewModel.factory(container))

    val mainUiState by mainViewModel.uiState.collectAsState()
    val deviceUiState by deviceViewModel.uiState.collectAsState()
    val processingUiState by processingViewModel.uiState.collectAsState()
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
        if (path != null) {
            hasBackupAvailable = processingViewModel.hasBackupFor(path, mainUiState.processNestedFolders)
        } else {
            hasBackupAvailable = false
        }
        processingViewModel.refreshExpectedTotal(path)
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

    Scaffold(
        bottomBar = { BottomNavigationBar(navController = navController) }
    ) { innerPadding ->
        CrossScaleNavigation(
            navController = navController,
            mainUiState = mainUiState,
            deviceUiState = deviceUiState,
            processingUiState = processingUiState,
            logsFlow = container.logRepository.logs,
            onSelectModel = mainViewModel::selectModel,
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
            onMoveInputSourceUp = mainViewModel::moveInputSourceUp,
            onMoveInputSourceDown = mainViewModel::moveInputSourceDown,
            onSelectOutputFolder = inputSelection::pickOutputFolder,
            onUseDefaultOutputFolder = mainViewModel::useDefaultOutputDirectory,
            onToggleOverwriteOriginal = mainViewModel::setOverwriteOriginalImage,
            onToggleBackupOriginal = mainViewModel::setBackupOriginalImage,
            onToggleProcessNestedFolders = mainViewModel::setProcessNestedFolders,
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
            onResumeProcessing = processingViewModel::resumeLastProcessing,
            onRetryFailedItems = processingViewModel::retryFailedItems,
            onResumePendingItem = processingViewModel::resumePendingItem,
            onRetryFailedItem = processingViewModel::retryFailedItem,
            onDiscardCheckpoint = processingViewModel::discardCheckpoint,
            onCancelProcessing = processingViewModel::cancelProcessing,
            hasBackupAvailable = hasBackupAvailable,
            onRestoreBackup = {
                restoreBackup(
                    context = context,
                    mainUiState = mainUiState,
                    processingViewModel = processingViewModel,
                    scope = scope,
                    onBackupAvailabilityChanged = { hasBackupAvailable = it }
                )
            },
            onRefreshModels = mainViewModel::refreshModels,
            onImportModels = inputSelection::importModels,
            onDeleteModel = mainViewModel::deleteModel,
            onShowManualConnectDialog = deviceViewModel::showManualConnectDialog,
            onManualConnect = deviceViewModel::connectManual,
            onConnectByQr = deviceViewModel::connectByQrPayload,
            onClearConnectionError = deviceViewModel::clearConnectionError,
            onConfirmFingerprint = deviceViewModel::confirmFingerprintAndConnect,
            onCancelFingerprint = deviceViewModel::cancelFingerprintConfirm,
            isStreamEncrypted = isStreamEncrypted,
            onStreamEncryptedChange = { enabled ->
                appSettings.setStreamEncrypted(enabled)
                streamEncryptedState.value = enabled
            },
            onClearLogs = logRepository::clear,
            onOpenModelRelease = {
                val releaseUri = Uri.parse(MODEL_RELEASE_URL)
                runCatching {
                    context.startActivity(
                        Intent(Intent.ACTION_VIEW, releaseUri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    )
                }
            },
            onOpenHelp = {
                navController.navigate(AppScreen.Help.route)
            },
            modifier = Modifier.padding(innerPadding)
        )
    }
}

private const val MODEL_RELEASE_URL = "https://github.com/xihale/crossscale/releases"
