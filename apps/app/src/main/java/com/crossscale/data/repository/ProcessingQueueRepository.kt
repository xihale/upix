package com.crossscale.data.repository

import android.content.Context
import java.io.File
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

enum class PersistedQueueItemStatus {
    PENDING,
    RUNNING,
    FAILED,
    COMPLETED
}

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
    val submittedAt: Long = System.currentTimeMillis(),
    val createdAtEpochMs: Long,
    val expectedItems: Int? = null,
    val status: PersistedQueueItemStatus = PersistedQueueItemStatus.PENDING,
    val attempts: Int = 0,
    val lastErrorMessage: String? = null,
    val lastOutputUri: String? = null
)

data class ProcessingQueueSnapshot(
    val items: List<PersistedQueueItem> = emptyList(),
    val isQueueRunning: Boolean = false
)

class ProcessingQueueRepository(
    context: Context,
    private val logRepository: LogRepository
) : ProcessingQueueStore {
    private val queueFile = File(context.filesDir, QUEUE_FILE_NAME)
    private val _queue = MutableStateFlow(ProcessingQueueSnapshot())
    override val queue: StateFlow<ProcessingQueueSnapshot> = _queue.asStateFlow()

    override fun saveQueue(snapshot: ProcessingQueueSnapshot) {
        val normalizedSnapshot = snapshot.normalizedForSave()
        if (normalizedSnapshot.items.isEmpty() && !normalizedSnapshot.isQueueRunning) {
            runCatching {
                if (queueFile.exists()) {
                    queueFile.delete()
                }
                _queue.value = normalizedSnapshot
            }.onFailure { throwable ->
                logRepository.error(
                    "ProcessingQueueRepository",
                    "Failed to clear queue: ${throwable.message}"
                )
            }
            return
        }

        runCatching {
            val payload = Json.encodeToString(
                JsonObject.serializer(),
                normalizedSnapshot.toJsonObject()
            )
            val tempFile = File(queueFile.parentFile, "${queueFile.name}.tmp")
            tempFile.writeText(payload)
            if (queueFile.exists()) {
                queueFile.delete()
            }
            val renameSuccess = tempFile.renameTo(queueFile)
            if (!renameSuccess) {
                throw IllegalStateException("Failed to replace queue file")
            }
            _queue.value = normalizedSnapshot
        }.onFailure { throwable ->
            logRepository.error(
                "ProcessingQueueRepository",
                "Failed to save queue: ${throwable.message}"
            )
        }
    }

    override fun loadQueue(): ProcessingQueueSnapshot {
        val loadedQueue = loadQueueFromDisk()
        _queue.value = loadedQueue
        return loadedQueue
    }

    override fun clearQueue() {
        runCatching {
            if (queueFile.exists()) {
                queueFile.delete()
            }
        }.onFailure { throwable ->
            logRepository.error(
                "ProcessingQueueRepository",
                "Failed to clear queue: ${throwable.message}"
            )
        }
        _queue.value = ProcessingQueueSnapshot()
    }

    private fun loadQueueFromDisk(): ProcessingQueueSnapshot {
        if (!queueFile.exists()) {
            return ProcessingQueueSnapshot()
        }

        return runCatching {
            val payload = queueFile.readText()
            val json = Json.parseToJsonElement(payload).jsonObject
            json.toQueueSnapshot().normalizedForRestore()
        }.onFailure { throwable ->
            logRepository.error(
                "ProcessingQueueRepository",
                "Failed to load queue: ${throwable.message}"
            )
        }.getOrElse {
            ProcessingQueueSnapshot()
        }
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
                    submittedAt = item.submittedAt,
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

    private fun ProcessingQueueSnapshot.toJsonObject(): JsonObject {
        return buildJsonObject {
            put("isQueueRunning", isQueueRunning)
            put(
                "items",
                buildJsonArray {
                    items.forEach { item ->
                        add(
                            buildJsonObject {
                                put("id", item.id)
                                put("source", item.source)
                                put("modelName", item.modelName)
                                put("distributedMode", item.distributedMode)
                                put("retryFailedOnly", item.retryFailedOnly)
                                item.outputDirectoryUri?.let { uri ->
                                    put("outputDirectoryUri", uri)
                                }
                                put("overwriteOriginal", item.overwriteOriginal)
                                put("backupOriginal", item.backupOriginal)
                                put("processNestedFolders", item.processNestedFolders)
                                put("submittedAt", item.submittedAt)
                                put("createdAtEpochMs", item.createdAtEpochMs)
                                item.expectedItems?.let { expectedItems ->
                                    put("expectedItems", expectedItems)
                                }
                                put("status", item.status.name)
                                put("attempts", item.attempts)
                                item.lastErrorMessage?.let { message ->
                                    put("lastErrorMessage", message)
                                }
                                item.lastOutputUri?.let { outputUri ->
                                    put("lastOutputUri", outputUri)
                                }
                            }
                        )
                    }
                }
            )
        }
    }

    private fun JsonObject.toQueueSnapshot(): ProcessingQueueSnapshot {
        val restoredItems = this["items"]
            ?.jsonArrayOrNull()
            ?.mapNotNull { element ->
                val item = element.jsonObjectOrNull() ?: return@mapNotNull null
                val id = item["id"]?.jsonPrimitive?.contentOrNull.orEmpty().trim()
                val source = item["source"]?.jsonPrimitive?.contentOrNull.orEmpty().trim()
                val modelName = item["modelName"]?.jsonPrimitive?.contentOrNull.orEmpty().trim()
                if (id.isBlank() || source.isBlank() || modelName.isBlank()) {
                    return@mapNotNull null
                }
                val parsedStatus = item["status"]?.jsonPrimitive?.contentOrNull
                    ?.let { statusName ->
                        runCatching { PersistedQueueItemStatus.valueOf(statusName.trim()) }.getOrNull()
                    } ?: PersistedQueueItemStatus.PENDING
                PersistedQueueItem(
                    id = id,
                    source = source,
                    modelName = modelName,
                    distributedMode = item["distributedMode"]?.jsonPrimitive?.booleanOrNull ?: false,
                    retryFailedOnly = item["retryFailedOnly"]?.jsonPrimitive?.booleanOrNull ?: false,
                    outputDirectoryUri = item["outputDirectoryUri"]?.jsonPrimitive?.contentOrNull,
                    overwriteOriginal = item["overwriteOriginal"]?.jsonPrimitive?.booleanOrNull ?: false,
                    backupOriginal = item["backupOriginal"]?.jsonPrimitive?.booleanOrNull ?: true,
                    processNestedFolders = item["processNestedFolders"]?.jsonPrimitive?.booleanOrNull ?: true,
                    submittedAt = item["submittedAt"]?.jsonPrimitive?.longOrNull ?: 0L,
                    createdAtEpochMs = item["createdAtEpochMs"]?.jsonPrimitive?.longOrNull ?: 0L,
                    expectedItems = item["expectedItems"]?.jsonPrimitive?.intOrNull,
                    status = parsedStatus,
                    attempts = item["attempts"]?.jsonPrimitive?.intOrNull ?: 0,
                    lastErrorMessage = item["lastErrorMessage"]?.jsonPrimitive?.contentOrNull,
                    lastOutputUri = item["lastOutputUri"]?.jsonPrimitive?.contentOrNull
                )
            } ?: emptyList()

        return ProcessingQueueSnapshot(
            items = restoredItems,
            isQueueRunning = this["isQueueRunning"]?.jsonPrimitive?.booleanOrNull ?: false
        )
    }

    private fun kotlinx.serialization.json.JsonElement.jsonArrayOrNull(): kotlinx.serialization.json.JsonArray? {
        return runCatching { jsonArray }.getOrNull()
    }

    private fun kotlinx.serialization.json.JsonElement.jsonObjectOrNull(): JsonObject? {
        return runCatching { jsonObject }.getOrNull()
    }

    companion object {
        private const val QUEUE_FILE_NAME = "processing_queue.json"
        private const val RUN_INTERRUPTED_MESSAGE = "Application restarted before batch completed"
    }
}
