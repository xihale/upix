package com.crossscale.data.repository

import android.content.Context
import java.io.File
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

@Serializable
data class ProcessingPendingItem(
    val uri: String,
    val displayName: String,
    val attempts: Int = 0
)

@Serializable
data class ProcessingFailureDetail(
    val uri: String,
    val displayName: String,
    val errorMessage: String,
    val attempts: Int = 1
)

@Serializable
data class ProcessingCheckpoint(
    val taskId: String,
    val source: String,
    val modelName: String,
    val distributedMode: Boolean,
    val totalItems: Int,
    val completedItems: Int,
    val outputDirectory: String,
    val outputDirectoryUri: String? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true,
    val processNestedFolders: Boolean = true,
    val createdAtEpochMs: Long,
    val updatedAtEpochMs: Long,
    val lastErrorMessage: String? = null,
    val pendingItems: List<ProcessingPendingItem> = emptyList(),
    val failureDetails: List<ProcessingFailureDetail> = emptyList()
)

class ProcessingCheckpointRepository(
    context: Context,
    private val logRepository: LogRepository,
    // 可注入 IO 调度器（单测注入 TestDispatcher）
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
) : ProcessingCheckpointStore {
    private val checkpointFile = File(context.filesDir, CHECKPOINT_FILE_NAME)
    private val _checkpoint = MutableStateFlow<ProcessingCheckpoint?>(null)
    override val checkpoint: StateFlow<ProcessingCheckpoint?> = _checkpoint.asStateFlow()

    // Tolerant config: unknown/missing keys decode to defaults so older cache
    // files stay readable; nullables are omitted on write like the old format.
    private val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
    }

    override suspend fun persistCheckpoint(checkpoint: ProcessingCheckpoint) {
        withContext(Dispatchers.IO) {
            runCatching {
                val payload = json.encodeToString(ProcessingCheckpoint.serializer(), checkpoint)
                val tempFile = File(checkpointFile.parentFile, "${checkpointFile.name}.tmp")
                tempFile.writeText(payload)
                if (checkpointFile.exists()) {
                    checkpointFile.delete()
                }
                val renameSuccess = tempFile.renameTo(checkpointFile)
                if (!renameSuccess) {
                    throw IllegalStateException("Failed to replace checkpoint file")
                }
                _checkpoint.value = checkpoint
            }.onFailure { throwable ->
                logRepository.error(
                    "ProcessingCheckpointRepository",
                    "Failed to save checkpoint: ${throwable.message}"
                )
            }
        }
    }

    // 读/删都走 IO 调度器：文件 IO 不允许落在主线程。
    override suspend fun loadCheckpoint(): ProcessingCheckpoint? = withContext(ioDispatcher) {
        val loadedCheckpoint = loadCheckpointFromDisk()
        _checkpoint.value = loadedCheckpoint
        loadedCheckpoint
    }

    override suspend fun clearCheckpoint() {
        withContext(ioDispatcher) {
            runCatching {
                if (checkpointFile.exists()) {
                    checkpointFile.delete()
                }
            }.onFailure { throwable ->
                logRepository.error(
                    "ProcessingCheckpointRepository",
                    "Failed to clear checkpoint: ${throwable.message}"
                )
            }
            _checkpoint.value = null
        }
    }

    private fun loadCheckpointFromDisk(): ProcessingCheckpoint? {
        if (!checkpointFile.exists()) {
            return null
        }
        return runCatching {
            json.decodeFromString(ProcessingCheckpoint.serializer(), checkpointFile.readText())
        }.onFailure { throwable ->
            logRepository.error(
                "ProcessingCheckpointRepository",
                "Failed to load checkpoint: ${throwable.message}"
            )
        }.getOrNull()
    }

    companion object {
        private const val CHECKPOINT_FILE_NAME = "processing_checkpoint.json"
    }
}
