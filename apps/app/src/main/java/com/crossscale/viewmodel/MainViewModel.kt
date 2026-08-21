package com.crossscale.viewmodel

import android.net.Uri
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.Stable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.crossscale.AppContainer
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.ModelImportResult
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.ModelLibraryEntry
import com.crossscale.data.model.PcModel
import com.crossscale.data.repository.BatchOutputRepository
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.repository.OutputImageFormat
import com.crossscale.network.CoordinatorClient
import com.crossscale.util.StringProvider
import java.io.File
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

// List/Set fields are treated as immutable by convention (never mutated
// after construction), which keeps the state skippable for Compose.
@Immutable
data class MainUiState(
    val models: List<ModelInfo> = emptyList(),
    val selectedModel: ModelInfo? = null,
    val selectedInputSources: List<String> = emptyList(),
    val corruptedInputSources: Set<String> = emptySet(),
    val selectedImagePath: String? = null,
    val selectedOutputDirectoryUri: String? = null,
    val overwriteOriginalImage: Boolean = false,
    val backupOriginalImage: Boolean = true,
    val processNestedFolders: Boolean = true,
    val outputImageFormat: OutputImageFormat = OutputImageFormat.FOLLOW,
    val modelDirectoryPath: String = "",
    val useRemote: Boolean = false,
    /** 合并后的双端模型库（按名称排序，含双端存在标记）。 */
    val library: List<ModelLibraryEntry> = emptyList(),
    /** 正在向 PC 上传的模型名集合。 */
    val uploadingToPc: Set<String> = emptySet(),
    /** 正在从 PC 下载的模型名集合。 */
    val downloadingFromPc: Set<String> = emptySet()
)

// Fused snapshot of the task-level toggle flows, combined ahead of the main
// uiState combine so it stays within the 5-flow overload.
private data class MainTaskSettings(
    val corruptedInputSources: Set<String>,
    val overwriteOriginalImage: Boolean,
    val backupOriginalImage: Boolean,
    val processNestedFolders: Boolean,
    val outputImageFormat: OutputImageFormat
)

// Observable state lives in StateFlow properties; the class itself never
// mutates through public API, so it is safe to mark @Stable.
@Stable
class MainViewModel(
    private val modelRepository: ModelProvider,
    private val deviceRepository: DeviceRepository,
    private val logRepository: LogRepository,
    private val stringProvider: StringProvider,
    private val batchOutputRepository: BatchOutputRepository,
    private val coordinatorClient: CoordinatorClient,
    private val modelsDirectory: () -> File
) : ViewModel() {
    private val _selectedModelName = MutableStateFlow<String?>(null)
    private val _pcModels = MutableStateFlow<List<PcModel>>(emptyList())
    private val _uploadingToPc = MutableStateFlow<Set<String>>(emptySet())
    private val _downloadingFromPc = MutableStateFlow<Set<String>>(emptySet())
    private val _selectedInputSources = MutableStateFlow<List<String>>(emptyList())
    private val _corruptedInputSources = MutableStateFlow<Set<String>>(emptySet())
    private val _selectedOutputDirectoryUri = MutableStateFlow<String?>(null)
    private val _overwriteOriginalImage = MutableStateFlow(false)
    private val _backupOriginalImage = MutableStateFlow(true)
    private val _processNestedFolders = MutableStateFlow(true)
    private val _outputImageFormat = MutableStateFlow(OutputImageFormat.FOLLOW)

    private val baseUiState = combine(
        modelRepository.models,
        _selectedModelName,
        _selectedInputSources,
        _selectedOutputDirectoryUri,
        deviceRepository.connectionState
    ) { models, selectedModelName, selectedInputSources, selectedOutputDirectoryUri, connectionState ->
        val selectedModel = models.firstOrNull { model ->
            model.name == selectedModelName
        } ?: models.firstOrNull()
        val encodedInputSource = InputSourceSpec.encode(selectedInputSources)
        MainUiState(
            models = models,
            selectedModel = selectedModel,
            selectedInputSources = selectedInputSources,
            selectedImagePath = encodedInputSource,
            selectedOutputDirectoryUri = selectedOutputDirectoryUri,
            modelDirectoryPath = modelRepository.modelDirectoryPath(),
            useRemote = connectionState == ConnectionState.CONNECTED
        )
    }

    /** 合并后的双端模型库：手机端优先命名，按名称排序。 */
    private val libraryFlow = combine(
        modelRepository.models,
        _pcModels
    ) { models, pcModels ->
        data class Merge(var phone: ModelInfo? = null, var pc: PcModel? = null)
        val merged = linkedMapOf<String, Merge>()
        models.forEach { m -> merged.getOrPut(normalizeModelName(m.name)) { Merge() }.phone = m }
        pcModels.forEach { p -> merged.getOrPut(normalizeModelName(p.name)) { Merge() }.pc = p }
        merged.map { (_, pair) ->
            val display = pair.phone?.name ?: pair.pc?.name.orEmpty()
            ModelLibraryEntry(
                name = display,
                scaleFactor = pair.phone?.scaleFactor ?: pair.pc?.scaleFactor ?: 2,
                totalSize = pair.phone?.totalSize ?: pair.pc?.totalSize ?: 0L,
                onPhone = pair.phone != null,
                onPc = pair.pc != null,
                localModel = pair.phone
            )
        }.sortedBy { it.name.lowercase(Locale.ROOT) }
    }

    private val taskSettings = combine(
        _corruptedInputSources,
        _overwriteOriginalImage,
        _backupOriginalImage,
        _processNestedFolders,
        _outputImageFormat
    ) { corruptedInputSources, overwriteOriginalImage, backupOriginalImage, processNestedFolders, outputImageFormat ->
        MainTaskSettings(
            corruptedInputSources = corruptedInputSources,
            overwriteOriginalImage = overwriteOriginalImage,
            backupOriginalImage = backupOriginalImage,
            processNestedFolders = processNestedFolders,
            outputImageFormat = outputImageFormat
        )
    }

    val uiState: StateFlow<MainUiState> = combine(
        baseUiState,
        taskSettings,
        libraryFlow,
        _uploadingToPc,
        _downloadingFromPc
    ) { state, settings, library, uploading, downloading ->
        state.copy(
            corruptedInputSources = settings.corruptedInputSources,
            overwriteOriginalImage = settings.overwriteOriginalImage,
            backupOriginalImage = settings.backupOriginalImage,
            processNestedFolders = settings.processNestedFolders,
            outputImageFormat = settings.outputImageFormat,
            library = library,
            uploadingToPc = uploading,
            downloadingFromPc = downloading
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000L),
        initialValue = MainUiState()
    )

    init {
        viewModelScope.launch {
            modelRepository.refreshModels()
            logRepository.info("MainViewModel", "Model initialization finished")
        }

        viewModelScope.launch {
            modelRepository.models.collectLatest { models ->
                if (_selectedModelName.value == null && models.isNotEmpty()) {
                    _selectedModelName.value = models.first().name
                }
            }
        }

        // 连接建立时拉取 PC 模型库；断开时清空，避免残留过期状态。
        viewModelScope.launch {
            deviceRepository.connectionState.collectLatest { state ->
                when (state) {
                    ConnectionState.CONNECTED -> refreshPcModels()
                    else -> {
                        _pcModels.value = emptyList()
                        _uploadingToPc.value = emptySet()
                        _downloadingFromPc.value = emptySet()
                    }
                }
            }
        }
    }

    fun selectModel(model: ModelInfo) {
        _selectedModelName.value = model.name
        logRepository.info("MainViewModel", "Selected model: ${model.name}")
        // 选择即同步：PC 模型库没有该模型时自动上传，之后远程处理可直接调用。
        maybeAutoUploadToPc(model)
    }

    private fun normalizeModelName(name: String): String =
        name.lowercase(Locale.ROOT).replace('_', '-')

    private fun pcHasModel(name: String): Boolean {
        val key = normalizeModelName(name)
        return _pcModels.value.any { normalizeModelName(it.name) == key }
    }

    private fun maybeAutoUploadToPc(model: ModelInfo) {
        if (deviceRepository.connectionState.value != ConnectionState.CONNECTED) return
        if (!coordinatorClient.isConnected()) return
        if (pcHasModel(model.name)) return
        logRepository.info("MainViewModel", "PC 模型库缺少 ${model.name}，自动上传")
        uploadModelToPc(model)
    }

    /** 拉取 PC 模型库元数据（连接后/上传下载完成后调用）。 */
    fun refreshPcModels() {
        viewModelScope.launch {
            val remote = withContext(Dispatchers.IO) { coordinatorClient.listPCModels() }
            if (remote != null) {
                _pcModels.value = remote
                logRepository.info("MainViewModel", "PC 模型库：${remote.size} 个模型")
            } else {
                logRepository.warning("MainViewModel", "拉取 PC 模型库失败")
            }
        }
    }

    /** 上传手机本地模型到 PC 模型库。 */
    fun uploadModelToPc(entry: ModelLibraryEntry) {
        entry.localModel?.let(::uploadModelToPc)
    }

    fun uploadModelToPc(model: ModelInfo) {
        if (model.name in _uploadingToPc.value) return
        _uploadingToPc.update { it + model.name }
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { coordinatorClient.uploadModelToPC(model) }
            result
                .onSuccess { logRepository.info("MainViewModel", "已上传到 PC：${model.name}") }
                .onFailure { logRepository.error("MainViewModel", "上传 PC 失败（${model.name}）：${it.message}") }
            _uploadingToPc.update { it - model.name }
            refreshPcModels()
        }
    }

    /** 从 PC 模型库下载模型到本地模型目录。 */
    fun downloadModelFromPc(entry: ModelLibraryEntry) {
        if (entry.name in _downloadingFromPc.value) return
        _downloadingFromPc.update { it + entry.name }
        viewModelScope.launch {
            val directory = File(modelsDirectory(), entry.name)
            val result = withContext(Dispatchers.IO) {
                coordinatorClient.downloadModelFromPC(
                    name = entry.name,
                    destParamPath = File(directory, "${entry.name}.param").absolutePath,
                    destBinPath = File(directory, "${entry.name}.bin").absolutePath
                )
            }
            result
                .onSuccess {
                    logRepository.info("MainViewModel", "已从 PC 下载：${entry.name}")
                    modelRepository.refreshModels()
                }
                .onFailure { throwable ->
                    logRepository.error("MainViewModel", "从 PC 下载失败（${entry.name}）：${throwable.message}")
                    // 清掉半成品文件，避免扫描出残缺模型。
                    directory.deleteRecursively()
                }
            _downloadingFromPc.update { it - entry.name }
        }
    }

    /**
     * 按倍率选模型：倍率由模型固有决定，切换倍率即切换到该倍率的模型。
     * 同倍率存在多个模型时优先同系列（名称去掉 -xN 后缀后相同），
     * 避免从 Real-CUGAN-denoise-x2 切 x3 时跳到无关模型。
     */
    fun selectModelByScale(scale: Int) {
        val models = modelRepository.models.value
        val currentModel = uiState.value.selectedModel
        if (currentModel?.scaleFactor == scale) {
            return
        }
        val familyOf = { name: String ->
            name.substringBeforeLast("-x", missingDelimiterValue = name)
        }
        val currentFamily = currentModel?.let { familyOf(it.name) }
        val targetModel = models.firstOrNull { model ->
            model.scaleFactor == scale && familyOf(model.name) == currentFamily
        } ?: models.firstOrNull { model -> model.scaleFactor == scale }
        if (targetModel == null) {
            logRepository.warning("MainViewModel", "No model found for scale x$scale")
            return
        }
        selectModel(targetModel)
    }

    fun selectImage(imagePath: String) {
        selectImages(listOf(imagePath))
    }

    fun selectImages(
        imagePaths: List<String>,
        corruptedSources: Set<String> = emptySet()
    ) {
        if (imagePaths.isEmpty()) {
            return
        }
        val normalizedSources = imagePaths
            .map { source -> source.trim() }
            .filter { source -> source.isNotBlank() }
            .distinct()
        if (normalizedSources.isEmpty()) {
            return
        }

        var addedCount = 0
        _selectedInputSources.update { currentSources ->
            val mergedSources = currentSources.toMutableList()
            normalizedSources.forEach { source ->
                if (source !in mergedSources) {
                    mergedSources.add(source)
                    addedCount += 1
                }
            }
            mergedSources
        }
        if (addedCount <= 0) {
            return
        }

        _corruptedInputSources.update { currentCorrupted ->
            val updatedCorrupted = currentCorrupted.toMutableSet()
            normalizedSources.forEach { source ->
                if (source in corruptedSources) {
                    updatedCorrupted.add(source)
                } else {
                    updatedCorrupted.remove(source)
                }
            }
            updatedCorrupted
        }
        logRepository.info(
            "MainViewModel",
            "Added $addedCount input source(s), total=${_selectedInputSources.value.size}"
        )
    }

    fun clearSelectedImage() {
        _selectedInputSources.value = emptyList()
        _corruptedInputSources.value = emptySet()
        logRepository.info("MainViewModel", "Cleared all input sources")
    }

    fun removeInputSource(source: String) {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return
        }
        var removed = false
        _selectedInputSources.update { currentSources ->
            if (normalizedSource !in currentSources) {
                currentSources
            } else {
                removed = true
                currentSources.filterNot { item ->
                    item == normalizedSource
                }
            }
        }
        if (removed) {
            _corruptedInputSources.update { currentCorrupted ->
                currentCorrupted - normalizedSource
            }
            logRepository.info(
                "MainViewModel",
                "Removed input source (${_selectedInputSources.value.size} left): $normalizedSource"
            )
        }
    }

    fun selectOutputDirectory(outputDirectoryUri: String?) {
        _selectedOutputDirectoryUri.value = outputDirectoryUri?.ifBlank { null }
        val outputLabel = _selectedOutputDirectoryUri.value ?: "App internal directory"
        logRepository.info("MainViewModel", "Selected output directory: $outputLabel")
    }

    fun useDefaultOutputDirectory() {
        _selectedOutputDirectoryUri.value = null
        logRepository.info("MainViewModel", "Using default output directory")
    }

    fun setOverwriteOriginalImage(enabled: Boolean) {
        _overwriteOriginalImage.value = enabled
    }

    fun setBackupOriginalImage(enabled: Boolean) {
        _backupOriginalImage.value = enabled
    }

    fun setProcessNestedFolders(enabled: Boolean) {
        _processNestedFolders.value = enabled
    }

    /** 输出格式：同步写入 BatchOutputRepository，输出名在处理时生成时读取。 */
    fun setOutputImageFormat(format: OutputImageFormat) {
        _outputImageFormat.value = format
        batchOutputRepository.requestedOutputFormat = format
    }

    fun refreshModels() {
        viewModelScope.launch {
            modelRepository.refreshModels()
        }
    }

    fun importModels(
        modelUris: List<Uri>,
        onCompleted: (ModelImportResult) -> Unit = {}
    ) {
        val normalizedUris = modelUris.distinct()
        if (normalizedUris.isEmpty()) {
            onCompleted(
                ModelImportResult(
                    invalidItems = listOf(
                        stringProvider.getString(R.string.models_import_error_no_files)
                    )
                )
            )
            return
        }

        viewModelScope.launch {
            val result = modelRepository.importModels(normalizedUris)
            if (result.importedCount > 0) {
                logRepository.info(
                    "MainViewModel",
                    "Imported ${result.importedCount} model(s): ${result.importedModelNames.joinToString()}"
                )
            }
            if (result.invalidCount > 0) {
                result.invalidItems.forEach { invalidItem ->
                    logRepository.warning("MainViewModel", "Model import invalid: $invalidItem")
                }
            }
            onCompleted(result)
        }
    }

    fun deleteModel(model: ModelInfo) {
        viewModelScope.launch {
            modelRepository.deleteModel(model)
        }
    }

    companion object {
        fun factory(container: AppContainer): ViewModelProvider.Factory = viewModelFactory {
            initializer {
                MainViewModel(
                    modelRepository = container.modelRepository,
                    deviceRepository = container.deviceRepository,
                    logRepository = container.logRepository,
                    stringProvider = container.stringProvider,
                    batchOutputRepository = container.batchOutputRepository,
                    coordinatorClient = container.coordinatorClient,
                    modelsDirectory = container.modelRepository::modelsDirectory
                )
            }
        }
    }
}
