package io.upix.data.model

import androidx.compose.runtime.Immutable
import io.upix.data.repository.ProcessingFailureDetail

enum class ProcessingStatus {
    IDLE,
    RUNNING,
    COMPLETED,
    FAILED,
    CANCELED
}

/** 远程（手机→PC）当前图的阶段；null = 本地模式或不适用。 */
enum class RemoteItemPhase {
    /** 上传原图到 PC。 */
    UPLOADING,
    /** PC 推理中（[ProcessingTask.remoteItemProgress] / remoteItemEtaSeconds 有效）。 */
    INFERENCE,
    /** 推理完成，PC 正在编码结果。 */
    ENCODING
}

// List fields would otherwise be inferred unstable by the Compose compiler;
// by convention the lists are never mutated after construction.
@Immutable
data class ProcessingTask(
    val id: String,
    val imagePath: String,
    val modelName: String,
    val distributedMode: Boolean,
    val outputDirectory: String? = null,
    val outputDirectoryUri: String? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true,
    val processNestedFolders: Boolean = true,
    val skipSrMarkedInputs: Boolean = true,
    val currentItemName: String? = null,
    val completedItems: Int = 0,
    val pendingItems: Int = 0,
    val failedItems: Int = 0,
    /** Already-super-resolved inputs skipped by the marker guard. */
    val skippedItems: Int = 0,
    val failureDetails: List<ProcessingFailureDetail> = emptyList(),
    val totalItems: Int = 1,
    val lastErrorMessage: String? = null,
    /** 远程当前图阶段（PC 流式 TASK_PROGRESS 透传）。 */
    val remotePhase: RemoteItemPhase? = null,
    /** 远程当前图推理进度 0..1（仅 [RemoteItemPhase.INFERENCE] 阶段有意义）。 */
    val remoteItemProgress: Float? = null,
    /** 远程当前图预计剩余秒数（PC 线性外推，0=未知）。 */
    val remoteItemEtaSeconds: Int? = null,
    val progress: Float,
    val status: ProcessingStatus
)
