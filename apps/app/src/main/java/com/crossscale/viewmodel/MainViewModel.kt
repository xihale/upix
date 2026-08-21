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
import com.crossscale.data.repository.BatchOutputRepository
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.repository.OutputImageFormat
import com.crossscale.util.StringProvider
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
    val useRemote: Boolean = false
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
    private val batchOutputRepository: BatchOutputRepository
) : ViewModel() {
    private val _selectedModelName = MutableStateFlow<String?>(null)
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
        taskSettings
    ) { state, settings ->
        state.copy(
            corruptedInputSources = settings.corruptedInputSources,
            overwriteOriginalImage = settings.overwriteOriginalImage,
            backupOriginalImage = settings.backupOriginalImage,
            processNestedFolders = settings.processNestedFolders,
            outputImageFormat = settings.outputImageFormat
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
    }

    fun selectModel(model: ModelInfo) {
        _selectedModelName.value = model.name
        logRepository.info("MainViewModel", "Selected model: ${model.name}")
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
                    batchOutputRepository = container.batchOutputRepository
                )
            }
        }
    }
}
