package io.upix.data.repository

import android.content.Context
import java.io.File
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
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
    // 断点续跑跟随创建 run 时的跳过开关（旧 checkpoint 缺键时默认 true）。
    val skipSrMarkedInputs: Boolean = true,
    val createdAtEpochMs: Long,
    val updatedAtEpochMs: Long,
    val lastErrorMessage: String? = null,
    val pendingItems: List<ProcessingPendingItem> = emptyList(),
    val failureDetails: List<ProcessingFailureDetail> = emptyList(),
    val queueItemId: String? = null
)

class ProcessingCheckpointRepository(
    context: Context,
    private val logRepository: LogRepository,
    // 可注入 IO 调度器（单测注入 TestDispatcher）
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
) : ProcessingCheckpointStore {
    private val checkpointFile = AtomicSnapshotFile(File(context.filesDir, CHECKPOINT_FILE_NAME))
    private val persistenceLock = Any()
    private val _checkpoint = MutableStateFlow<ProcessingCheckpoint?>(null)
    override val checkpoint: StateFlow<ProcessingCheckpoint?> = _checkpoint.asStateFlow()

    // Tolerant config: unknown/missing keys decode to defaults so older cache
    // files stay readable; nullables are omitted on write like the old format.
    private val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
    }

    override suspend fun persistCheckpoint(checkpoint: ProcessingCheckpoint) {
        withContext(ioDispatcher) {
            val coroutineContext = currentCoroutineContext()
            synchronized(persistenceLock) {
                coroutineContext.ensureActive()
                try {
                    checkpointFile.writeText(json.encodeToString(ProcessingCheckpoint.serializer(), checkpoint))
                    _checkpoint.value = checkpoint
                } catch (exception: Exception) {
                    logRepository.error("ProcessingCheckpointRepository", "Failed to save checkpoint: ${exception.message}")
                    throw exception
                }
            }
        }
    }

    override suspend fun loadCheckpoint(): ProcessingCheckpoint? = withContext(ioDispatcher) {
        val coroutineContext = currentCoroutineContext()
        synchronized(persistenceLock) {
            coroutineContext.ensureActive()
            val loadedCheckpoint = try {
                loadCheckpointFromDisk()
            } catch (exception: Exception) {
                logRepository.error(
                    "ProcessingCheckpointRepository",
                    "Failed to load checkpoint: ${exception.message}"
                )
                return@synchronized _checkpoint.value
            }
            _checkpoint.value = loadedCheckpoint
            loadedCheckpoint
        }
    }

    override suspend fun clearCheckpoint() {
        withContext(ioDispatcher) {
            val coroutineContext = currentCoroutineContext()
            synchronized(persistenceLock) {
                coroutineContext.ensureActive()
                try {
                    checkpointFile.delete()
                    _checkpoint.value = null
                } catch (exception: Exception) {
                    logRepository.error("ProcessingCheckpointRepository", "Failed to clear checkpoint: ${exception.message}")
                    throw exception
                }
            }
        }
    }

    private fun loadCheckpointFromDisk(): ProcessingCheckpoint? {
        val payload = checkpointFile.readText() ?: return null
        return json.decodeFromString(ProcessingCheckpoint.serializer(), payload)
    }

    companion object {
        private const val CHECKPOINT_FILE_NAME = "processing_checkpoint.json"
    }
}
