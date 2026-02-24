package com.crossscale

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.rememberNavController
import com.crossscale.ui.components.BottomNavigationBar
import com.crossscale.ui.navigation.CrossScaleNavigation
import com.crossscale.viewmodel.DeviceViewModel
import com.crossscale.viewmodel.MainViewModel
import com.crossscale.viewmodel.ProcessingViewModel

@OptIn(ExperimentalMaterial3Api::class)
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
    val logs by container.logRepository.logs.collectAsState()
    val logRepository = container.logRepository

    LaunchedEffect(mainUiState.selectedImagePath) {
        processingViewModel.refreshExpectedTotal(mainUiState.selectedImagePath)
    }

    val pickSingleImageLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) {
            logRepository.warning("CrossScaleApp", "Image picker canceled")
            return@rememberLauncherForActivityResult
        }
        persistUriPermission(context = context, uri = uri, read = true, write = false)
        mainViewModel.selectImage(uri.toString())
        logRepository.info("CrossScaleApp", "Added input image: $uri")
    }

    val pickMultipleImagesLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenMultipleDocuments()
    ) { uris ->
        if (uris.isNullOrEmpty()) {
            logRepository.warning("CrossScaleApp", "Multiple image picker canceled")
            return@rememberLauncherForActivityResult
        }
        uris.forEach { uri ->
            persistUriPermission(context = context, uri = uri, read = true, write = false)
        }
        mainViewModel.selectImages(uris.map { uri -> uri.toString() })
        logRepository.info("CrossScaleApp", "Added ${uris.size} input images from picker")
    }

    val pickImageFolderLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri == null) {
            logRepository.warning("CrossScaleApp", "Folder picker canceled")
            return@rememberLauncherForActivityResult
        }
        persistUriPermission(context = context, uri = uri, read = true, write = false)
        mainViewModel.selectImage(uri.toString())
        logRepository.info("CrossScaleApp", "Added input folder: $uri")
    }

    val pickOutputFolderLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri == null) {
            logRepository.warning("CrossScaleApp", "Output folder picker canceled")
            return@rememberLauncherForActivityResult
        }
        persistUriPermission(context = context, uri = uri, read = true, write = true)
        mainViewModel.selectOutputDirectory(uri.toString())
        logRepository.info("CrossScaleApp", "Selected output folder: $uri")
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("CrossScale") }
            )
        },
        bottomBar = { BottomNavigationBar(navController = navController) }
    ) { innerPadding ->
        CrossScaleNavigation(
            navController = navController,
            mainUiState = mainUiState,
            deviceUiState = deviceUiState,
            processingUiState = processingUiState,
            logs = logs,
            onSelectModel = mainViewModel::selectModel,
            onSelectSingleImage = {
                pickSingleImageLauncher.launch(arrayOf("image/*"))
            },
            onSelectMultipleImages = {
                pickMultipleImagesLauncher.launch(arrayOf("image/*"))
            },
            onSelectImageFolder = {
                pickImageFolderLauncher.launch(null)
            },
            onClearInputSources = mainViewModel::clearSelectedImage,
            onRemoveInputSource = mainViewModel::removeInputSource,
            onMoveInputSourceUp = mainViewModel::moveInputSourceUp,
            onMoveInputSourceDown = mainViewModel::moveInputSourceDown,
            onSelectOutputFolder = {
                pickOutputFolderLauncher.launch(null)
            },
            onUseInternalOutputFolder = {
                mainViewModel.selectOutputDirectory(null)
            },
            onStartProcessing = {
                val selectedModelName = mainUiState.selectedModel?.name
                val imagePath = mainUiState.selectedImagePath
                if (selectedModelName == null || imagePath == null) {
                    logRepository.warning("CrossScaleApp", "Select image and model before processing")
                } else {
                    processingViewModel.startProcessing(
                        imagePath = imagePath,
                        modelName = selectedModelName,
                        distributedMode = mainUiState.isDistributedMode,
                        outputDirectoryUri = mainUiState.selectedOutputDirectoryUri
                    )
                }
            },
            onEnqueueBatch = {
                val selectedModelName = mainUiState.selectedModel?.name
                val imagePath = mainUiState.selectedImagePath
                if (selectedModelName == null || imagePath == null) {
                    logRepository.warning("CrossScaleApp", "Select image and model before queueing")
                } else {
                    processingViewModel.enqueueBatch(
                        imagePath = imagePath,
                        modelName = selectedModelName,
                        distributedMode = mainUiState.isDistributedMode,
                        outputDirectoryUri = mainUiState.selectedOutputDirectoryUri
                    )
                }
            },
            onStartBatchQueue = processingViewModel::startBatchQueue,
            onPauseBatchQueue = processingViewModel::pauseBatchQueue,
            onRetryQueuedBatch = processingViewModel::retryQueuedBatch,
            onRetryQueuedBatchFailedOnly = processingViewModel::retryQueuedBatchFailedOnly,
            onRemoveQueuedBatch = processingViewModel::removeQueuedBatch,
            onClearBatchQueue = processingViewModel::clearBatchQueue,
            onResumeProcessing = processingViewModel::resumeLastProcessing,
            onRetryFailedItems = processingViewModel::retryFailedItems,
            onResumePendingItem = processingViewModel::resumePendingItem,
            onRetryFailedItem = processingViewModel::retryFailedItem,
            onDiscardCheckpoint = processingViewModel::discardCheckpoint,
            onCancelProcessing = processingViewModel::cancelProcessing,
            onRefreshModels = mainViewModel::refreshModels,
            onDeleteModel = mainViewModel::deleteModel,
            onStartDiscovery = deviceViewModel::startDiscovery,
            onStopDiscovery = deviceViewModel::stopDiscovery,
            onConnectDevice = deviceViewModel::connectDevice,
            onDisconnectDevice = deviceViewModel::disconnectDevice,
            onShowManualConnectDialog = deviceViewModel::showManualConnectDialog,
            onManualConnect = deviceViewModel::connectManual,
            onConnectByQr = deviceViewModel::connectByQrPayload,
            onClearLogs = logRepository::clear,
            modifier = Modifier.padding(innerPadding)
        )
    }
}

private fun persistUriPermission(
    context: Context,
    uri: Uri,
    read: Boolean,
    write: Boolean
) {
    val flags = (if (read) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
        (if (write) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
    if (flags == 0) {
        return
    }
    try {
        context.contentResolver.takePersistableUriPermission(
            uri,
            flags
        )
    } catch (_: SecurityException) {
        // Some providers do not grant persistable permissions.
    } catch (_: IllegalArgumentException) {
        // URI does not support persistable permissions.
    }
}
