package com.crossscale.viewmodel

import androidx.compose.runtime.Immutable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.crossscale.AppContainer
import com.crossscale.R
import com.crossscale.data.model.RemoteModel
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.repository.ModelReleaseRepository
import com.crossscale.util.StringProvider
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** 单个远程模型的下载状态。 */
@Immutable
data class RemoteModelDownloadState(
    val downloadedBytes: Long = 0L,
    val totalBytes: Long = 0L,
    val failed: Boolean = false,
    val errorMessage: String? = null
) {
    val progress: Float
        get() = if (totalBytes > 0) (downloadedBytes.toFloat() / totalBytes).coerceIn(0f, 1f) else 0f

    /** 无 Content-Length（totalBytes<=0）时总量未知，UI 应显示不确定进度条。 */
    val indeterminate: Boolean
        get() = totalBytes <= 0L
}

@Immutable
data class ModelsUiState(
    val remoteModels: List<RemoteModel> = emptyList(),
    val isLoadingRemote: Boolean = false,
    val remoteError: String? = null,
    /** 正在下载的模型名 → 进度状态；不在表中 = 未在下载。 */
    val downloads: Map<String, RemoteModelDownloadState> = emptyMap()
)

/**
 * Models 页的远程模型状态：manifest 列表 + 逐模型下载进度。
 * 本地模型列表仍由 MainViewModel/ModelProvider 持有，下载完成后触发其刷新。
 */
class ModelsViewModel(
    private val modelRepository: ModelProvider,
    private val modelReleaseRepository: ModelReleaseRepository,
    private val logRepository: LogRepository,
    private val stringProvider: StringProvider,
    private val modelsDirectoryProvider: () -> File
) : ViewModel() {

    private val _uiState = MutableStateFlow(ModelsUiState())
    val uiState: StateFlow<ModelsUiState> = _uiState.asStateFlow()

    /** 模型名 → 下载 Job，供 cancelDownload 取消；只在主线程访问（VM 方法 + Main 派发的协程体）。 */
    private val downloadJobs = mutableMapOf<String, Job>()

    fun refreshRemoteModels() {
        if (_uiState.value.isLoadingRemote) return
        _uiState.update { it.copy(isLoadingRemote = true, remoteError = null) }
        viewModelScope.launch {
            modelReleaseRepository.fetchRemoteModels()
                .onSuccess { models ->
                    _uiState.update {
                        it.copy(isLoadingRemote = false, remoteModels = models)
                    }
                }
                .onFailure { throwable ->
                    _uiState.update {
                        it.copy(
                            isLoadingRemote = false,
                            remoteError = throwable.message
                                ?: stringProvider.getString(R.string.models_remote_error)
                        )
                    }
                }
        }
    }

    fun downloadRemoteModel(model: RemoteModel) {
        if (_uiState.value.downloads.containsKey(model.name)) return
        _uiState.update {
            it.copy(downloads = it.downloads + (model.name to RemoteModelDownloadState()))
        }
        downloadJobs[model.name] = viewModelScope.launch {
            try {
                val result = modelReleaseRepository.downloadModel(
                    model = model,
                    modelsDirectory = modelsDirectoryProvider(),
                    onProgress = { downloaded, total ->
                        _uiState.update { state ->
                            state.copy(
                                downloads = state.downloads + (
                                    model.name to RemoteModelDownloadState(
                                        downloadedBytes = downloaded,
                                        totalBytes = total
                                    )
                                )
                            )
                        }
                    }
                )
                result
                    .onSuccess { name ->
                        logRepository.info("ModelsViewModel", "Model downloaded: $name")
                    }
                    .onFailure { throwable ->
                        // 取消不是失败：重新抛给外层 catch 做取消清理，不落 failed 态。
                        if (throwable is CancellationException) throw throwable
                        _uiState.update { state ->
                            state.copy(
                                downloads = state.downloads + (
                                    model.name to RemoteModelDownloadState(
                                        failed = true,
                                        errorMessage = throwable.message
                                    )
                                )
                            )
                        }
                    }
                // 无论成败都刷新本地列表（失败时清掉半成品文件后的目录状态）。
                modelRepository.refreshModels()
                // 成功后从下载表中移除，让列表回归"已安装"状态。
                if (result.isSuccess) {
                    _uiState.update { state ->
                        state.copy(downloads = state.downloads - model.name)
                    }
                }
            } catch (ce: CancellationException) {
                // 用户取消（或 VM 销毁）：不算失败——移除条目，让该行回到可下载状态。
                // 仓库内 ensureActive 抛出时已走 .part 清理路径；目录刷新用
                // NonCancellable 包裹，保证在已取消的协程里仍能执行完。
                _uiState.update { state ->
                    state.copy(downloads = state.downloads - model.name)
                }
                withContext(NonCancellable) { modelRepository.refreshModels() }
                throw ce
            } finally {
                downloadJobs.remove(model.name)
            }
        }
    }

    /** 用户主动取消某个模型的下载；该行随后回到可下载状态（不算失败）。 */
    fun cancelDownload(modelName: String) {
        downloadJobs.remove(modelName)?.cancel()
    }

    /** 下载失败后由用户手动清除失败标记，允许重试。 */
    fun dismissDownloadError(modelName: String) {
        _uiState.update { state ->
            state.copy(downloads = state.downloads - modelName)
        }
    }

    companion object {
        fun factory(container: AppContainer): ViewModelProvider.Factory = viewModelFactory {
            initializer {
                ModelsViewModel(
                    modelRepository = container.modelRepository,
                    modelReleaseRepository = container.modelReleaseRepository,
                    logRepository = container.logRepository,
                    stringProvider = container.stringProvider,
                    modelsDirectoryProvider = container.modelRepository::modelsDirectory
                )
            }
        }
    }
}
