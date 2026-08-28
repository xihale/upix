package io.upix.ui.input

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.ChecksSdkIntAtLeast
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import io.upix.R
import io.upix.data.model.ModelImportResult
import io.upix.data.repository.InputSourceSpec
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
    internal lateinit var requestNotificationPermissionLauncher: ActivityResultLauncher<String>

    var mainUiState: MainUiState = MainUiState()
        internal set

    // API 33+ 通知权限只在每个会话问一次（拒绝也照常启动队列：前台服务与
    // WakeLock 不依赖该权限，仅通知可见性受影响）。
    private var notificationPermissionAsked = false
    private var pendingNotificationQueueStartBatchId: String? = null

    // 「无模型 → 有模型」跃迁检测状态：首次观察不触发（控制器随重组重建时
    // 直接对齐当前值，避免旋转/恢复时对已入队批次重复补入队）。
    private var modelAvailabilityObserved = false
    private var lastSeenModelName: String? = null

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
            } else if (queueEligibleSources.isNotEmpty()) {
                // 多选合并为一个批次：InputSourceSpec.encode 把 N 个 URI 编码成
                // upix://multi: 单源（1 个时原样返回），一次规划/一份 checkpoint/
                // 一行队列；解析、输出目录、断点续传语义与文件夹批次一致
                // （InputSourceRepository/BatchOutputRepository/QueueSheet 均已
                // 内建 multi 解码）。
                val mergedBatchSource = InputSourceSpec.encode(queueEligibleSources)
                if (mergedBatchSource == null) {
                    logRepository.warning("UpixApp", "No queue eligible sources to enqueue after merge")
                } else {
                    processingViewModel.enqueueBatch(
                        imagePath = mergedBatchSource,
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

    /**
     * 选图/选文件夹时还没有任何模型会跳过自动入队（只记 warning）。这里在
     * 「无模型 → 有模型」跃迁的一刻把已选输入补进队列，否则列表会一直为空。
     * 每次模型名变化由 [rememberInputSelection] 的 LaunchedEffect 调用；
     * 模型 A→B 切换不补——沿用「切模型不重排已入队批次」的既有语义。
     */
    fun onSelectedModelNameChanged() {
        val currentModelName = mainUiState.selectedModel?.name
        val becameAvailable = modelAvailabilityObserved &&
            lastSeenModelName == null &&
            currentModelName != null
        modelAvailabilityObserved = true
        lastSeenModelName = currentModelName
        if (!becameAvailable) {
            return
        }
        val eligibleSources = pendingEnqueueSources(
            selectedSources = mainUiState.selectedInputSources,
            corruptedSources = mainUiState.corruptedInputSources,
            queuedSources = processingViewModel.queuedSources()
        )
        if (eligibleSources.isEmpty()) {
            return
        }
        // 与选图路径同一合并语义：N 源编码成 upix://multi: 单批次
        // （文件夹树 URI 子源与单图子源混排同样受 InputSourceRepository 支持）。
        val mergedBatchSource = InputSourceSpec.encode(eligibleSources)
        if (mergedBatchSource == null) {
            logRepository.warning("UpixApp", "No queue eligible sources to enqueue after model became available")
            return
        }
        processingViewModel.enqueueBatch(
            imagePath = mergedBatchSource,
            modelName = currentModelName,
            distributedMode = mainUiState.useRemote,
            outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
            overwriteOriginal = mainUiState.overwriteOriginalImage,
            backupOriginal = mainUiState.backupOriginalImage,
            processNestedFolders = mainUiState.processNestedFolders,
            skipSrMarkedInputs = mainUiState.skipSrMarkedInputs
        )
        logRepository.info(
            "UpixApp",
            "Enqueued ${eligibleSources.size} input source(s) selected before a model was available: $currentModelName"
        )
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
        // 前置：API 33+ 先补一次通知权限（拒绝不阻断，服务照常保活）。
        if (needsNotificationPermissionRequest()) {
            notificationPermissionAsked = true
            pendingNotificationQueueStartBatchId = preferredBatchId
            requestNotificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            return
        }
        startQueueWithWritePermissionCheckInternal(preferredBatchId)
    }

    // 蕴含 SDK >= 33：POST_NOTIFICATIONS 常量仅在真值分支触达，非 InlinedApi。
    @ChecksSdkIntAtLeast(api = Build.VERSION_CODES.TIRAMISU)
    private fun needsNotificationPermissionRequest(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU || notificationPermissionAsked) {
            return false
        }
        return ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.POST_NOTIFICATIONS
        ) != PackageManager.PERMISSION_GRANTED
    }

    internal fun onNotificationPermissionResult(granted: Boolean) {
        logRepository.info(
            "UpixApp",
            if (granted) {
                "Notification permission granted"
            } else {
                "Notification permission denied; foreground service continues without visible notifications"
            }
        )
        val pendingBatchId = pendingNotificationQueueStartBatchId
        pendingNotificationQueueStartBatchId = null
        startQueueWithWritePermissionCheckInternal(pendingBatchId)
    }

    private fun startQueueWithWritePermissionCheckInternal(preferredBatchId: String?) {
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

/**
 * 模型就绪补入队的候选源：剔除损坏源与已在队列中的源
 * （文件夹树 URI 等单源批次可精确去重；多图合并批次由 enqueueBatch 的
 * matchesBatchConfiguration 兜底去重）。
 */
internal fun pendingEnqueueSources(
    selectedSources: List<String>,
    corruptedSources: Set<String>,
    queuedSources: Set<String>
): List<String> = selectedSources.filterNot { source ->
    source in corruptedSources || source in queuedSources
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

    // 模型就绪的一刻把「无模型时选的输入」补入队（见 onSelectedModelNameChanged）。
    LaunchedEffect(mainUiState.selectedModel?.name) {
        controller.onSelectedModelNameChanged()
    }

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

    controller.requestNotificationPermissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission()
    ) { granted ->
        controller.onNotificationPermissionResult(granted)
    }

    return controller
}
