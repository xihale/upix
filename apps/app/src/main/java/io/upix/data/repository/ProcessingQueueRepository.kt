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
enum class PersistedQueueItemStatus {
    PENDING,
    RUNNING,
    FAILED,
    COMPLETED
}

@Serializable
data class PersistedQueueItem(
    val id: String,
    val source: String,
    val modelName: String,
    val distributedMode: Boolean,
    val retryFailedOnly: Boolean = false,
    val outputDirectoryUri: String? = null,
    val overwriteOriginal: Boolean = false,
    val backupOriginal: Boolean = true,
    val processNestedFolders: Boolean = true,
    // 旧队列文件缺该键时反序列化为默认 true（与 UI 默认一致）。
    val skipSrMarkedInputs: Boolean = true,
    val createdAtEpochMs: Long,
    val expectedItems: Int? = null,
    val status: PersistedQueueItemStatus = PersistedQueueItemStatus.PENDING,
    val attempts: Int = 0,
    val lastErrorMessage: String? = null,
    val lastOutputUri: String? = null
)

@Serializable
data class ProcessingQueueSnapshot(
    val items: List<PersistedQueueItem> = emptyList(),
    val isQueueRunning: Boolean = false
)

class ProcessingQueueRepository(
    context: Context,
    private val logRepository: LogRepository,
    // 可注入 IO 调度器（与 ProcessingCheckpointRepository 同一模式，单测可注入 TestDispatcher）
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
) : ProcessingQueueStore {
    private val queueFile = AtomicSnapshotFile(File(context.filesDir, QUEUE_FILE_NAME))
    private val persistenceLock = Any()
    private val _queue = MutableStateFlow(ProcessingQueueSnapshot())
    override val queue: StateFlow<ProcessingQueueSnapshot> = _queue.asStateFlow()

    // Tolerant config: unknown/missing keys decode to defaults so older cache
    // files stay readable; nullables are omitted on write like the old format.
    private val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
    }

    override suspend fun persistQueue(snapshot: ProcessingQueueSnapshot) {
        withContext(ioDispatcher) {
            val coroutineContext = currentCoroutineContext()
            synchronized(persistenceLock) {
                coroutineContext.ensureActive()
                val normalizedSnapshot = snapshot.normalizedForSave()
                try {
                    if (normalizedSnapshot.items.isEmpty() && !normalizedSnapshot.isQueueRunning) {
                        queueFile.delete()
                    } else {
                        queueFile.writeText(json.encodeToString(ProcessingQueueSnapshot.serializer(), normalizedSnapshot))
                    }
                    _queue.value = normalizedSnapshot
                } catch (exception: Exception) {
                    logRepository.error("ProcessingQueueRepository", "Failed to save queue: ${exception.message}")
                    throw exception
                }
            }
        }
    }

    override fun loadQueue(): ProcessingQueueSnapshot = synchronized(persistenceLock) {
        val loadedQueue = try {
            loadQueueFromDisk()
        } catch (exception: Exception) {
            logRepository.error(
                "ProcessingQueueRepository",
                "Failed to load queue: ${exception.message}"
            )
            return@synchronized _queue.value
        }
        _queue.value = loadedQueue
        loadedQueue
    }

    private fun loadQueueFromDisk(): ProcessingQueueSnapshot {
        val payload = queueFile.readText() ?: return ProcessingQueueSnapshot()
        return json.decodeFromString(ProcessingQueueSnapshot.serializer(), payload).normalizedForRestore()
    }

    private fun ProcessingQueueSnapshot.normalizedForSave(): ProcessingQueueSnapshot {
        val normalizedItems = items.mapNotNull { item ->
            val normalizedId = item.id.trim()
            val normalizedSource = item.source.trim()
            val normalizedModelName = item.modelName.trim()
            if (normalizedId.isBlank() || normalizedSource.isBlank() || normalizedModelName.isBlank()) {
                null
            } else {
                PersistedQueueItem(
                    id = normalizedId,
                    source = normalizedSource,
                    modelName = normalizedModelName,
                    distributedMode = item.distributedMode,
                    retryFailedOnly = item.retryFailedOnly,
                    outputDirectoryUri = item.outputDirectoryUri?.trim()?.ifBlank { null },
                    overwriteOriginal = item.overwriteOriginal,
                    backupOriginal = item.backupOriginal,
                    processNestedFolders = item.processNestedFolders,
                    skipSrMarkedInputs = item.skipSrMarkedInputs,
                    createdAtEpochMs = item.createdAtEpochMs,
                    expectedItems = item.expectedItems?.coerceAtLeast(0),
                    status = item.status,
                    attempts = item.attempts.coerceAtLeast(0),
                    lastErrorMessage = item.lastErrorMessage?.trim()?.ifBlank { null },
                    lastOutputUri = item.lastOutputUri?.trim()?.ifBlank { null }
                )
            }
        }
        val normalizedRunning = isQueueRunning && normalizedItems.any { item ->
            item.status == PersistedQueueItemStatus.PENDING || item.status == PersistedQueueItemStatus.RUNNING
        }
        return ProcessingQueueSnapshot(
            items = normalizedItems,
            isQueueRunning = normalizedRunning
        )
    }

    private fun ProcessingQueueSnapshot.normalizedForRestore(): ProcessingQueueSnapshot {
        val normalized = normalizedForSave()
        val restoredItems = normalized.items.map { item ->
            if (item.status == PersistedQueueItemStatus.RUNNING) {
                item.copy(
                    status = PersistedQueueItemStatus.FAILED,
                    retryFailedOnly = false,
                    lastErrorMessage = item.lastErrorMessage ?: RUN_INTERRUPTED_MESSAGE
                )
            } else {
                item
            }
        }
        return ProcessingQueueSnapshot(
            items = restoredItems,
            isQueueRunning = false
        )
    }

    companion object {
        private const val QUEUE_FILE_NAME = "processing_queue.json"
        private const val RUN_INTERRUPTED_MESSAGE = "Application restarted before batch completed"
    }
}
