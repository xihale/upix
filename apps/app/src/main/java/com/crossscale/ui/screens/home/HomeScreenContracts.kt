package com.crossscale.ui.screens.home

import androidx.compose.runtime.Immutable
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.RemoteEndpoint

/**
 * Home 屏的连接相关状态：当前连接态、已连接端点，以及远程回退
 * 一次性消息（展示 snackbar 后由 [onClearRemoteFallbackMessage] 清除）。
 *
 * 参数对象按约定构造后不可变；lambda 属性在 strong skipping 下由
 * Compose 自动 memoize，data class equals 可正常命中跳过。
 */
@Immutable
data class HomeConnection(
    val connectionState: ConnectionState,
    val connectedEndpoint: RemoteEndpoint? = null,
    val remoteFallbackMessage: String? = null,
    val onClearRemoteFallbackMessage: () -> Unit = {}
)

/** 模型 / 输入源 / 输出目录 / 参数开关等任务配置类事件回调。 */
@Immutable
data class HomeSetupActions(
    val onSelectModel: (ModelInfo) -> Unit,
    val onSelectImages: () -> Unit,
    val onSelectImageFolder: () -> Unit,
    val onClearInputSources: () -> Unit,
    val onRemoveInputSource: (String) -> Unit,
    val onMoveInputSourceUp: (String) -> Unit,
    val onMoveInputSourceDown: (String) -> Unit,
    val onSelectOutputFolder: () -> Unit,
    val onUseDefaultOutputFolder: () -> Unit,
    val onToggleOverwriteOriginal: (Boolean) -> Unit,
    val onToggleBackupOriginal: (Boolean) -> Unit,
    val onToggleProcessNestedFolders: (Boolean) -> Unit
)

/** 运行与批队列事件回调：立即超分、队列启停 / 重试 / 打开 / 移除 / 清空、取消。 */
@Immutable
data class HomeQueueActions(
    val onStartProcessing: () -> Unit,
    val onStartBatchQueue: () -> Unit,
    val onPauseBatchQueue: () -> Unit,
    val onRetryQueuedBatch: (String) -> Unit,
    val onRetryQueuedBatchFailedOnly: (String) -> Unit,
    val onOpenQueuedBatchOutput: (String) -> Unit,
    val onRemoveQueuedBatch: (String) -> Unit,
    val onClearBatchQueue: () -> Unit,
    val onCancelProcessing: () -> Unit
)

/** 断点续传事件回调：继续、重试失败、逐项继续 / 重试、丢弃。 */
@Immutable
data class HomeCheckpointActions(
    val onResumeProcessing: () -> Unit,
    val onRetryFailedItems: () -> Unit,
    val onResumePendingItem: (String) -> Unit,
    val onRetryFailedItem: (String) -> Unit,
    val onDiscardCheckpoint: () -> Unit
)
