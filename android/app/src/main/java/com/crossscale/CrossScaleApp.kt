package com.crossscale

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.rememberNavController
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import com.crossscale.data.repository.LogRepository
import com.crossscale.ui.components.BottomNavigationBar
import com.crossscale.ui.navigation.AppScreen
import com.crossscale.ui.navigation.CrossScaleNavigation
import com.crossscale.viewmodel.DeviceViewModel
import com.crossscale.viewmodel.MainViewModel
import com.crossscale.viewmodel.ProcessingViewModel
import java.io.File

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
    val navBackStackEntry by navController.currentBackStackEntryAsState()
    val currentRoute = navBackStackEntry?.destination?.route
    val currentScreen = AppScreen.allScreens.firstOrNull { screen ->
        screen.route == currentRoute
    } ?: AppScreen.Home

    LaunchedEffect(mainUiState.selectedImagePath) {
        processingViewModel.refreshExpectedTotal(mainUiState.selectedImagePath)
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
        val selectedModelName = mainUiState.selectedModel?.name
        val inputSources = uris.map { uri -> uri.toString() }
        mainViewModel.selectImages(inputSources)
        if (selectedModelName == null) {
            logRepository.warning("CrossScaleApp", "No model selected, skip auto enqueue for newly added images")
        } else {
            inputSources.forEach { source ->
                processingViewModel.enqueueBatch(
                    imagePath = source,
                    modelName = selectedModelName,
                    distributedMode = mainUiState.isDistributedMode,
                    outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
                    overwriteOriginal = mainUiState.overwriteOriginalImage,
                    backupOriginal = mainUiState.backupOriginalImage
                )
            }
        }
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
        val selectedModelName = mainUiState.selectedModel?.name
        val inputSource = uri.toString()
        mainViewModel.selectImage(inputSource)
        if (selectedModelName == null) {
            logRepository.warning("CrossScaleApp", "No model selected, skip auto enqueue for newly added folder")
        } else {
            processingViewModel.enqueueBatch(
                imagePath = inputSource,
                modelName = selectedModelName,
                distributedMode = mainUiState.isDistributedMode,
                outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
                overwriteOriginal = mainUiState.overwriteOriginalImage,
                backupOriginal = mainUiState.backupOriginalImage
            )
        }
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

    var pendingQueueStartBatchId by rememberSaveable { mutableStateOf<String?>(null) }

    val requestWritePermissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            logRepository.info("CrossScaleApp", "Storage write permission granted")
            processingViewModel.startBatchQueue(pendingQueueStartBatchId)
        } else {
            logRepository.warning("CrossScaleApp", "Storage write permission denied, queue start canceled")
        }
        pendingQueueStartBatchId = null
    }

    val startQueueWithWritePermissionCheck: (String?) -> Unit = { preferredBatchId ->
        if (needsLegacyWritePermission().not()) {
            processingViewModel.startBatchQueue(preferredBatchId)
        } else {
            val hasPermission = ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            ) == PackageManager.PERMISSION_GRANTED
            if (hasPermission) {
                processingViewModel.startBatchQueue(preferredBatchId)
            } else {
                pendingQueueStartBatchId = preferredBatchId
                requestWritePermissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        text = currentScreen.title,
                        style = MaterialTheme.typography.titleMedium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
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
            onUseDefaultOutputFolder = mainViewModel::useDefaultOutputDirectory,
            onToggleOverwriteOriginal = mainViewModel::setOverwriteOriginalImage,
            onToggleBackupOriginal = mainViewModel::setBackupOriginalImage,
            onStartProcessing = {
                var hasQueueItems = processingUiState.queueItems.isNotEmpty()
                if (processingUiState.queueItems.isEmpty()) {
                    val selectedModelName = mainUiState.selectedModel?.name
                    val imagePath = mainUiState.selectedImagePath
                    if (selectedModelName == null || imagePath == null) {
                        logRepository.warning("CrossScaleApp", "Select image and model before processing")
                    } else {
                        processingViewModel.enqueueBatch(
                            imagePath = imagePath,
                            modelName = selectedModelName,
                            distributedMode = mainUiState.isDistributedMode,
                            outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
                            overwriteOriginal = mainUiState.overwriteOriginalImage,
                            backupOriginal = mainUiState.backupOriginalImage
                        )
                        hasQueueItems = true
                    }
                }
                if (hasQueueItems) {
                    startQueueWithWritePermissionCheck(null)
                }
            },
            onStartBatchQueue = { startQueueWithWritePermissionCheck(null) },
            onPauseBatchQueue = processingViewModel::pauseBatchQueue,
            onRetryQueuedBatch = processingViewModel::retryQueuedBatch,
            onRetryQueuedBatchFailedOnly = processingViewModel::retryQueuedBatchFailedOnly,
            onOpenQueuedBatchOutput = { batchId ->
                val outputUri = processingViewModel.outputUriForQueuedBatch(batchId)
                if (outputUri.isNullOrBlank()) {
                    logRepository.warning("CrossScaleApp", "No completed output found for batch=$batchId")
                } else {
                    openImageOutput(context = context, rawOutput = outputUri, logRepository = logRepository)
                }
            },
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
            onOpenModelRelease = {
                val releaseUri = Uri.parse(MODEL_RELEASE_URL)
                runCatching {
                    context.startActivity(
                        Intent(Intent.ACTION_VIEW, releaseUri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    )
                }
            },
            modifier = Modifier.padding(innerPadding)
        )
    }
}

private const val MODEL_RELEASE_URL = "https://github.com/xihale/crossscale/releases"

private fun openImageOutput(
    context: Context,
    rawOutput: String,
    logRepository: LogRepository
): Boolean {
    val normalizedOutput = rawOutput.trim()
    if (normalizedOutput.isBlank()) {
        return false
    }

    val viewUri = resolveOutputViewUri(context = context, rawOutput = normalizedOutput)
    if (viewUri == null) {
        logRepository.warning("CrossScaleApp", "Cannot resolve output URI: $normalizedOutput")
        return false
    }

    val viewIntent = Intent(Intent.ACTION_VIEW).apply {
        setDataAndType(viewUri, "image/*")
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }
    val canHandle = viewIntent.resolveActivity(context.packageManager) != null
    if (!canHandle) {
        logRepository.warning("CrossScaleApp", "No app can open output image: $viewUri")
        return false
    }

    return runCatching {
        context.startActivity(viewIntent)
        true
    }.onFailure { throwable ->
        logRepository.warning("CrossScaleApp", "Failed to open output image: ${throwable.message}")
    }.getOrDefault(false)
}

private fun resolveOutputViewUri(
    context: Context,
    rawOutput: String
): Uri? {
    if (rawOutput.startsWith("/")) {
        val file = File(rawOutput)
        if (!file.exists()) {
            return null
        }
        return FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
    }

    val parsedUri = runCatching { Uri.parse(rawOutput) }.getOrNull() ?: return null
    return when (parsedUri.scheme?.lowercase()) {
        null -> {
            val file = File(rawOutput)
            if (!file.exists()) {
                null
            } else {
                FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
            }
        }
        "content" -> parsedUri
        "file" -> {
            val file = File(parsedUri.path.orEmpty())
            if (!file.exists()) {
                null
            } else {
                FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
            }
        }
        else -> null
    }
}

private fun needsLegacyWritePermission(): Boolean {
    return Build.VERSION.SDK_INT <= Build.VERSION_CODES.P
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
