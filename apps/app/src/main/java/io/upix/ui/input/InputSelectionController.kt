package io.upix.ui.input

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import io.upix.R
import io.upix.data.model.ModelImportResult
import io.upix.data.repository.LogRepository
import io.upix.util.isImageSourceDecodable
import io.upix.util.needsLegacyWritePermission
import io.upix.util.persistUriPermission
import io.upix.viewmodel.MainUiState
import io.upix.viewmodel.MainViewModel
import io.upix.viewmodel.ProcessingViewModel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * 输入获取关注点的状态持有器（从 UpixApp 原样迁出）：
 * 选图 / 选文件夹 / 模型导入的 SAF 流程，以及与队列启动耦合的
 * 存储写权限流程（pendingQueueStartBatchId 与 startBatchQueue 保持原子）。
 *
 * [mainUiState] 由 [rememberInputSelection] 在每次重组时刷新，
 * 语义等价于原先每次重组重建的事件 lambda 所捕获的最新状态。
 */
internal class InputSelectionController(
    private val context: Context,
    private val logRepository: LogRepository,
    private val mainViewModel: MainViewModel,
    private val processingViewModel: ProcessingViewModel,
    private val pendingQueueStartBatchId: MutableState<String?>,
    // 由 rememberInputSelection 用 rememberCoroutineScope() 构造（主线程域），
    // 用于把选图回调里的授权持久化 + 解码探测移出主线程。
    private val scope: CoroutineScope
) {
    internal lateinit var pickMultipleImagesLauncher: ActivityResultLauncher<PickVisualMediaRequest>
    internal lateinit var pickImageFolderLauncher: ActivityResultLauncher<Uri?>
    internal lateinit var pickOutputFolderLauncher: ActivityResultLauncher<Uri?>
    internal lateinit var pickModelFilesLauncher: ActivityResultLauncher<Array<String>>
    internal lateinit var requestWritePermissionLauncher: ActivityResultLauncher<String>

    var mainUiState: MainUiState = MainUiState()
        internal set

    fun pickImages() {
        pickMultipleImagesLauncher.launch(
            PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
        )
    }

    fun pickImageFolder() {
        pickImageFolderLauncher.launch(null)
    }

    fun pickOutputFolder() {
        pickOutputFolderLauncher.launch(null)
    }

    fun importModels() {
        pickModelFilesLauncher.launch(arrayOf("*/*"))
    }

    fun handlePickedImageUris(uris: List<Uri>, pickerName: String) {
        if (uris.isEmpty()) {
            logRepository.warning("UpixApp", "$pickerName returned no images")
            return
        }
        // 先在主线程快照 UI 状态，避免协程里读到过期值。
        val selectedModelName = mainUiState.selectedModel?.name
        val useRemote = mainUiState.useRemote
        val outputDirectoryUri = mainUiState.selectedOutputDirectoryUri
        val overwriteOriginal = mainUiState.overwriteOriginalImage
        val backupOriginal = mainUiState.backupOriginalImage
        val processNestedFolders = mainUiState.processNestedFolders
        val skipSrMarkedInputs = mainUiState.skipSrMarkedInputs

        scope.launch {
            val inputSources = uris.map { uri -> uri.toString() }
            // 授权持久化 ×N 与逐图解码探测都是磁盘/Provider IO：搬到 IO 线程，
            // 不再阻塞 picker 回调所在的主线程。
            val corruptedSources = withContext(Dispatchers.IO) {
                uris.forEach { uri ->
                    persistUriPermission(context = context, uri = uri, read = true, write = true)
                }
                inputSources.filterTo(mutableSetOf()) { source ->
                    !isImageSourceDecodable(context = context, rawSource = source)
                }
            }

            // 回到主线程（scope 是主线程域）更新选中状态并入队。
            mainViewModel.selectImages(
                imagePaths = inputSources,
                corruptedSources = corruptedSources
            )

            val queueEligibleSources = inputSources.filterNot { source -> source in corruptedSources }
            if (corruptedSources.isNotEmpty()) {
                logRepository.warning(
                    "UpixApp",
                    "Skipped ${corruptedSources.size} corrupted image(s) from queue"
                )
            }
            if (selectedModelName == null) {
                logRepository.warning("UpixApp", "No model selected, skip auto enqueue for newly added images")
            } else {
                queueEligibleSources.forEach { source ->
                    processingViewModel.enqueueBatch(
                        imagePath = source,
                        modelName = selectedModelName,
                        distributedMode = useRemote,
                        outputDirectoryUri = outputDirectoryUri,
                        overwriteOriginal = overwriteOriginal,
                        backupOriginal = backupOriginal,
                        processNestedFolders = processNestedFolders,
                        skipSrMarkedInputs = skipSrMarkedInputs
                    )
                }
            }
            logRepository.info(
                "UpixApp",
                "Added ${uris.size} input images via $pickerName, queue eligible=${queueEligibleSources.size}"
            )
        }
    }

    fun handlePickedInputFolderUri(uri: Uri?) {
        if (uri == null) {
            logRepository.warning("UpixApp", "Folder picker canceled")
            return
        }
        // 先在主线程快照 UI 状态，避免协程里读到过期值（同 handlePickedImageUris）。
        val selectedModelName = mainUiState.selectedModel?.name
        val useRemote = mainUiState.useRemote
        val outputDirectoryUri = mainUiState.selectedOutputDirectoryUri
        val overwriteOriginal = mainUiState.overwriteOriginalImage
        val backupOriginal = mainUiState.backupOriginalImage
        val processNestedFolders = mainUiState.processNestedFolders
        val skipSrMarkedInputs = mainUiState.skipSrMarkedInputs
        val inputSource = uri.toString()
        scope.launch {
            // 授权持久化是 takePersistableUriPermission 的 binder IPC：
            // 搬到 IO 线程，不再阻塞 picker 回调所在的主线程。
            withContext(Dispatchers.IO) {
                persistUriPermission(context = context, uri = uri, read = true, write = true)
            }
            // 回到主线程（scope 是主线程域）更新选中状态并入队。
            mainViewModel.selectImage(inputSource)
            if (selectedModelName == null) {
                logRepository.warning("UpixApp", "No model selected, skip auto enqueue for newly added folder")
            } else {
                processingViewModel.enqueueBatch(
                    imagePath = inputSource,
                    modelName = selectedModelName,
                    distributedMode = useRemote,
                    outputDirectoryUri = outputDirectoryUri,
                    overwriteOriginal = overwriteOriginal,
                    backupOriginal = backupOriginal,
                    processNestedFolders = processNestedFolders,
                    skipSrMarkedInputs = skipSrMarkedInputs
                )
            }
            logRepository.info("UpixApp", "Added input folder: $uri")
        }
    }

    fun handlePickedOutputFolderUri(uri: Uri?) {
        if (uri == null) {
            logRepository.warning("UpixApp", "Output folder picker canceled")
        } else {
            scope.launch {
                // 授权持久化是 binder IPC：搬到 IO 线程执行。
                withContext(Dispatchers.IO) {
                    persistUriPermission(context = context, uri = uri, read = true, write = true)
                }
                mainViewModel.selectOutputDirectory(uri.toString())
                logRepository.info("UpixApp", "Selected output folder: $uri")
            }
        }
    }

    fun handleImportedModelUris(uris: List<Uri>, pickerName: String) {
        if (uris.isEmpty()) {
            logRepository.warning("UpixApp", "$pickerName did not return model files")
        } else {
            scope.launch {
                // 授权持久化 ×N 是逐条 binder IPC：搬到 IO 线程批量执行。
                withContext(Dispatchers.IO) {
                    uris.forEach { uri ->
                        persistUriPermission(context = context, uri = uri, read = true, write = false)
                    }
                }
                mainViewModel.importModels(uris) { result ->
                    val message = buildModelImportFeedbackMessage(result, context)
                    Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    fun onWritePermissionResult(granted: Boolean) {
        if (granted) {
            logRepository.info("UpixApp", "Storage write permission granted")
            processingViewModel.startBatchQueue(pendingQueueStartBatchId.value)
        } else {
            logRepository.warning("UpixApp", "Storage write permission denied, queue start canceled")
            // 拒绝权限导致队列未启动：必须给用户可见反馈。
            Toast.makeText(
                context,
                context.getString(R.string.storage_write_permission_denied),
                Toast.LENGTH_SHORT
            ).show()
        }
        pendingQueueStartBatchId.value = null
    }

    fun startQueueWithWritePermissionCheck(preferredBatchId: String?) {
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
                pendingQueueStartBatchId.value = preferredBatchId
                requestWritePermissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            }
        }
    }
}

private fun buildModelImportFeedbackMessage(result: ModelImportResult, context: Context): String {
    if (result.importedCount <= 0) {
        return context.getString(R.string.models_import_error_invalid_pair_generic)
    }
    return if (result.invalidCount > 0) {
        context.getString(R.string.models_import_result_partial, result.importedCount, result.invalidCount)
    } else {
        context.getString(R.string.models_import_result_success, result.importedCount)
    }
}

/** 创建 [InputSelectionController] 并注册其全部 ActivityResult launcher。 */
@Composable
internal fun rememberInputSelection(
    mainViewModel: MainViewModel,
    processingViewModel: ProcessingViewModel,
    mainUiState: MainUiState,
    logRepository: LogRepository
): InputSelectionController {
    val context = LocalContext.current
    val pendingQueueStartBatchId = rememberSaveable { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    val controller = remember(context, mainViewModel, processingViewModel, logRepository) {
        InputSelectionController(
            context = context,
            logRepository = logRepository,
            mainViewModel = mainViewModel,
            processingViewModel = processingViewModel,
            pendingQueueStartBatchId = pendingQueueStartBatchId,
            scope = scope
        )
    }
    controller.mainUiState = mainUiState

    controller.pickMultipleImagesLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.PickMultipleVisualMedia()
    ) { uris ->
        if (uris.isNullOrEmpty()) {
            logRepository.warning("UpixApp", "Multiple image picker canceled")
            return@rememberLauncherForActivityResult
        }
        controller.handlePickedImageUris(uris, "system photo picker")
    }

    controller.pickImageFolderLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        controller.handlePickedInputFolderUri(uri)
    }

    controller.pickOutputFolderLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        controller.handlePickedOutputFolderUri(uri)
    }

    controller.pickModelFilesLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenMultipleDocuments()
    ) { uris ->
        if (uris.isNullOrEmpty()) {
            logRepository.warning("UpixApp", "Model import canceled")
            return@rememberLauncherForActivityResult
        }
        controller.handleImportedModelUris(uris, "system picker")
    }

    controller.requestWritePermissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission()
    ) { granted ->
        controller.onWritePermissionResult(granted)
    }

    return controller
}
