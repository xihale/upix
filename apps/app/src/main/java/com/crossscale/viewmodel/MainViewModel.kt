package com.crossscale.viewmodel

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.crossscale.AppContainer
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.ModelImportResult
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.InputSourceSpec
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelProvider
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

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
    val modelDirectoryPath: String = "",
    val useRemote: Boolean = false
)

class MainViewModel(
    private val modelRepository: ModelProvider,
    private val deviceRepository: DeviceRepository,
    private val logRepository: LogRepository
) : ViewModel() {
    private val _selectedModelName = MutableStateFlow<String?>(null)
    private val _selectedInputSources = MutableStateFlow<List<String>>(emptyList())
    private val _corruptedInputSources = MutableStateFlow<Set<String>>(emptySet())
    private val _selectedOutputDirectoryUri = MutableStateFlow<String?>(null)
    private val _overwriteOriginalImage = MutableStateFlow(false)
    private val _backupOriginalImage = MutableStateFlow(true)
    private val _processNestedFolders = MutableStateFlow(true)

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

    val uiState: StateFlow<MainUiState> = combine(
        baseUiState,
        _corruptedInputSources,
        _overwriteOriginalImage,
        _backupOriginalImage,
        _processNestedFolders
    ) { state, corruptedInputSources, overwriteOriginalImage, backupOriginalImage, processNestedFolders ->
        state.copy(
            corruptedInputSources = corruptedInputSources,
            overwriteOriginalImage = overwriteOriginalImage,
            backupOriginalImage = backupOriginalImage,
            processNestedFolders = processNestedFolders
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000L),
        initialValue = MainUiState()
    )

    init {
        viewModelScope.launch {
            modelRepository.syncBundledModels()
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

    fun moveInputSourceUp(source: String) {
        moveInputSource(source = source, offset = -1)
    }

    fun moveInputSourceDown(source: String) {
        moveInputSource(source = source, offset = 1)
    }

    private fun moveInputSource(
        source: String,
        offset: Int
    ) {
        val normalizedSource = source.trim()
        if (normalizedSource.isBlank()) {
            return
        }
        var fromIndex = -1
        var toIndex = -1
        _selectedInputSources.update { currentSources ->
            val currentIndex = currentSources.indexOf(normalizedSource)
            if (currentIndex == -1) {
                return@update currentSources
            }
            val targetIndex = (currentIndex + offset).coerceIn(0, currentSources.lastIndex)
            if (targetIndex == currentIndex) {
                return@update currentSources
            }
            fromIndex = currentIndex
            toIndex = targetIndex
            val movedSources = currentSources.toMutableList()
            val movedItem = movedSources.removeAt(currentIndex)
            movedSources.add(targetIndex, movedItem)
            movedSources
        }
        if (fromIndex >= 0 && toIndex >= 0) {
            logRepository.info(
                "MainViewModel",
                "Moved input source to position ${toIndex + 1}: $normalizedSource"
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
                    invalidItems = listOf("未选择模型文件")
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
                    logRepository = container.logRepository
                )
            }
        }
    }
}
