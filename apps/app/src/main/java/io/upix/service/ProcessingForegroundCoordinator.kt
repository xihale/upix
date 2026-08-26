package io.upix.service

import android.content.Context
import android.content.Intent
import androidx.compose.runtime.Immutable
import androidx.core.content.ContextCompat
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * 前台保活协调器：批处理进度的“共享总线”。
 *
 * ProcessingViewModel 在 init 中把 ProcessingUiState 投影成
 * [ForegroundProgressSnapshot] 推进来，ProcessingKeepAliveService 收集
 * [state] 渲染前台通知、收集 [completions] 发完成通知——两者互不持有引用，
 * 依赖关系只经过 AppContainer 里的这一个单例（与现有手写 DI 风格一致）。
 *
 * 协调器还负责服务的启动触发：active 变为 true（或换批次）时调用
 * startForegroundService；服务的停止由服务自己在观察到 active=false 时
 * stopSelf 完成。
 */
class ProcessingForegroundCoordinator(
    private val context: Context
) {
    private val _state = MutableStateFlow(ForegroundProgressSnapshot())
    val state: StateFlow<ForegroundProgressSnapshot> = _state.asStateFlow()

    // 完成事件（一次一条，带单调 id）：extraBufferCapacity + DROP_OLDEST 让
    // tryEmit 永不丢协程，慢消费者（服务未启动）只丢旧事件。
    private val _completions = MutableSharedFlow<ForegroundCompletionSnapshot>(
        extraBufferCapacity = 16,
        onBufferOverflow = BufferOverflow.DROP_OLDEST
    )
    val completions: SharedFlow<ForegroundCompletionSnapshot> = _completions.asSharedFlow()

    // 已向系统请求启动服务（服务自停后由 onServiceDestroyed 复位）。
    private var serviceStartRequested = false

    /** 当前批次进度快照；由 ProcessingViewModel 的 uiState 投影调用。 */
    fun onProgress(snapshot: ForegroundProgressSnapshot) {
        val previous = _state.value
        _state.value = snapshot
        if (!snapshot.active) {
            return
        }
        // active 期间保证服务在跑：批次启动（false→true 或换 taskId）时补一次
        // startForegroundService——服务已存活时这只是一次幂等的 onStartCommand。
        val newTaskStarted = snapshot.runningTaskId != null && snapshot.runningTaskId != previous.runningTaskId
        if (!serviceStartRequested || newTaskStarted) {
            ensureServiceStarted()
        }
    }

    /** 批次到达终态（COMPLETED/FAILED/CANCELED）时由 ViewModel 侧调用一次。 */
    fun emitCompletion(completion: ForegroundCompletionSnapshot) {
        _completions.tryEmit(completion)
    }

    /** 服务自行销毁后复位启动标记，下一轮 active 能重新拉起服务。 */
    fun onServiceDestroyed() {
        serviceStartRequested = false
    }

    private fun ensureServiceStarted() {
        // minSdk 26：startForegroundService 自 API 26 起可用，无需版本分支。
        val started = runCatching {
            ContextCompat.startForegroundService(
                context,
                Intent(context, ProcessingKeepAliveService::class.java)
            )
            true
        }.isSuccess
        serviceStartRequested = started
    }
}

/** 前台通知渲染所需的进度快照（原始数值，通知文案由服务层格式化）。 */
@Immutable
data class ForegroundProgressSnapshot(
    /** 有批次在跑（或队列在推进）：前台服务应存活。 */
    val active: Boolean = false,
    val runningTaskId: String? = null,
    val modelName: String? = null,
    val completedItems: Int = 0,
    val totalItems: Int = 0,
    val currentItemName: String? = null,
    val remoteMode: Boolean = false,
    /** PC 推理百分比 0..100；仅 INFERENCE 阶段非 null。 */
    val remoteInferencePercent: Int? = null,
    val remoteEtaSeconds: Int? = null
)

/** 批次终态快照（完成/失败/取消），驱动一条完成通知。 */
@Immutable
data class ForegroundCompletionSnapshot(
    val id: Long,
    val taskId: String,
    val completed: Boolean,
    val canceled: Boolean,
    val completedItems: Int,
    val totalItems: Int,
    val failedItems: Int,
    val outputDirectory: String?,
    val errorMessage: String?
)
