package com.crossscale

import android.content.Context
import android.widget.Toast
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.util.openImageOutput
import com.crossscale.viewmodel.MainUiState
import com.crossscale.viewmodel.MainViewModel
import com.crossscale.viewmodel.ProcessingUiState
import com.crossscale.viewmodel.ProcessingViewModel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * 导航接线中的复合编排回调（跨 MainViewModel / ProcessingViewModel 的组合动作），
 * 从 CrossScaleApp 原样迁出，按 queue / checkpoint 区块分组。逻辑与原先
 * 内联在 CrossScaleNavigation 调用处的事件 lambda 逐字等价，仅在调用时
 * 以参数显式传入所需依赖。
 */

// ---- queue 区块 ----

internal fun startProcessing(
    context: Context,
    mainUiState: MainUiState,
    processingUiState: ProcessingUiState,
    processingViewModel: ProcessingViewModel,
    logRepository: LogRepository,
    startQueueWithWritePermissionCheck: (String?) -> Unit
) {
    processingViewModel.syncPendingQueueDefaults(
        distributedMode = mainUiState.useRemote,
        outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
        overwriteOriginal = mainUiState.overwriteOriginalImage,
        backupOriginal = mainUiState.backupOriginalImage,
        processNestedFolders = mainUiState.processNestedFolders
    )
    var hasQueueItems = processingUiState.queueItems.isNotEmpty()
    if (processingUiState.queueItems.isEmpty()) {
        val selectedModelName = mainUiState.selectedModel?.name
        val imagePath = mainUiState.selectedImagePath
        if (selectedModelName == null || imagePath == null) {
            logRepository.warning("CrossScaleApp", "Select image and model before processing")
            // 前置条件不满足必须给用户可见反馈，不能只写日志。
            Toast.makeText(
                context,
                context.getString(R.string.start_need_image_and_model),
                Toast.LENGTH_SHORT
            ).show()
        } else {
            processingViewModel.enqueueBatch(
                imagePath = imagePath,
                modelName = selectedModelName,
                distributedMode = mainUiState.useRemote,
                outputDirectoryUri = mainUiState.selectedOutputDirectoryUri,
                overwriteOriginal = mainUiState.overwriteOriginalImage,
                backupOriginal = mainUiState.backupOriginalImage,
                processNestedFolders = mainUiState.processNestedFolders
            )
            hasQueueItems = true
        }
    }
    if (hasQueueItems) {
        // 队列里只剩已完成/失败记录时无可启动项：可见反馈，而不是无声空转。
        val hasPending = processingUiState.queueItems.any { item ->
            item.status == PersistedQueueItemStatus.PENDING
        }
        if (hasPending) {
            startQueueWithWritePermissionCheck(null)
        } else {
            Toast.makeText(
                context,
                context.getString(R.string.queue_no_pending_items),
                Toast.LENGTH_SHORT
            ).show()
        }
    }
}

internal fun openQueuedBatchOutput(
    context: Context,
    batchId: String,
    processingViewModel: ProcessingViewModel,
    logRepository: LogRepository
) {
    val outputUri = processingViewModel.outputUriForQueuedBatch(batchId)
    if (outputUri.isNullOrBlank()) {
        logRepository.warning("CrossScaleApp", "No completed output found for batch=$batchId")
        notifyOpenOutputFailed(context)
    } else {
        val opened = openImageOutput(context = context, rawOutput = outputUri, logRepository = logRepository)
        if (!opened) {
            // 文件被删/移动或无查看器：静默失败等于坏按钮，必须可见反馈。
            notifyOpenOutputFailed(context)
        }
    }
}

private fun notifyOpenOutputFailed(context: Context) {
    Toast.makeText(
        context,
        context.getString(R.string.queue_open_output_failed),
        Toast.LENGTH_SHORT
    ).show()
}

internal fun removeQueuedBatch(
    batchId: String,
    mainViewModel: MainViewModel,
    processingViewModel: ProcessingViewModel
) {
    val removedSource = processingViewModel.sourceForQueuedBatch(batchId)
    processingViewModel.removeQueuedBatch(batchId)
    if (removedSource != null && !processingViewModel.hasQueuedBatchForSource(removedSource)) {
        mainViewModel.removeInputSource(removedSource)
    }
}

internal fun clearBatchQueue(
    mainUiState: MainUiState,
    mainViewModel: MainViewModel,
    processingViewModel: ProcessingViewModel
) {
    processingViewModel.clearBatchQueue()
    val remainingSources = processingViewModel.queuedSources()
    mainUiState.selectedInputSources
        .filter { source -> source !in remainingSources }
        .forEach { source ->
            mainViewModel.removeInputSource(source)
        }
}

// ---- checkpoint 区块 ----

internal fun restoreBackup(
    context: Context,
    mainUiState: MainUiState,
    processingViewModel: ProcessingViewModel,
    scope: CoroutineScope,
    onBackupAvailabilityChanged: (Boolean) -> Unit
) {
    val path = mainUiState.selectedImagePath
    if (path != null) {
        processingViewModel.restoreBackup(path, mainUiState.processNestedFolders) { restored, failed ->
            scope.launch {
                val msg = if (failed > 0) {
                    context.getString(R.string.home_restore_backup_result_partial, restored, failed)
                } else {
                    context.getString(R.string.home_restore_backup_result_success, restored)
                }
                Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
                // Refresh backup availability state after restore
                onBackupAvailabilityChanged(processingViewModel.hasBackupFor(path, mainUiState.processNestedFolders))
            }
        }
    }
}
