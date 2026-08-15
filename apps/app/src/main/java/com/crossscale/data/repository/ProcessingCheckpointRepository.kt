package com.crossscale.data.repository

import android.content.Context
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
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

data class ProcessingPendingItem(
    val uri: String,
    val displayName: String,
    val attempts: Int = 0
)

data class ProcessingFailureDetail(
    val uri: String,
    val displayName: String,
    val errorMessage: String,
    val attempts: Int = 1
)

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
    private val logRepository: LogRepository
) : ProcessingCheckpointStore {
    private val checkpointFile = File(context.filesDir, CHECKPOINT_FILE_NAME)
    private val _checkpoint = MutableStateFlow<ProcessingCheckpoint?>(null)
    override val checkpoint: StateFlow<ProcessingCheckpoint?> = _checkpoint.asStateFlow()

    // Synchronous path retained for direct callers (unit tests). All production
    // flows go through [persistCheckpoint] so JSON serialization + file writes
    // stay off the main thread.
    override fun saveCheckpoint(checkpoint: ProcessingCheckpoint) {
        runBlocking { persistCheckpoint(checkpoint) }
    }

    override suspend fun persistCheckpoint(checkpoint: ProcessingCheckpoint) {
        withContext(Dispatchers.IO) {
            runCatching {
                val payload = checkpoint.toJsonObject()
                val tempFile = File(checkpointFile.parentFile, "${checkpointFile.name}.tmp")
                tempFile.writeText(Json.encodeToString(JsonObject.serializer(), payload))
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

    override fun loadCheckpoint(): ProcessingCheckpoint? {
        val loadedCheckpoint = loadCheckpointFromDisk()
        _checkpoint.value = loadedCheckpoint
        return loadedCheckpoint
    }

    override fun clearCheckpoint() {
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

    private fun loadCheckpointFromDisk(): ProcessingCheckpoint? {
        if (!checkpointFile.exists()) {
            return null
        }
        return runCatching {
            val payload = checkpointFile.readText()
            val json = Json.parseToJsonElement(payload).jsonObject
            json.toCheckpoint()
        }.onFailure { throwable ->
            logRepository.error(
                "ProcessingCheckpointRepository",
                "Failed to load checkpoint: ${throwable.message}"
            )
        }.getOrNull()
    }

    private fun ProcessingCheckpoint.toJsonObject(): JsonObject {
        return buildJsonObject {
            put("taskId", taskId)
            put("source", source)
            put("modelName", modelName)
            put("distributedMode", distributedMode)
            put("totalItems", totalItems)
            put("completedItems", completedItems)
            put("outputDirectory", outputDirectory)
            outputDirectoryUri?.let { put("outputDirectoryUri", it) }
            put("overwriteOriginal", overwriteOriginal)
            put("backupOriginal", backupOriginal)
            put("processNestedFolders", processNestedFolders)
            put("createdAtEpochMs", createdAtEpochMs)
            put("updatedAtEpochMs", updatedAtEpochMs)
            lastErrorMessage?.let { put("lastErrorMessage", it) }
            put(
                "pendingItems",
                buildJsonArray {
                    pendingItems.forEach { item ->
                        add(
                            buildJsonObject {
                                put("uri", item.uri)
                                put("displayName", item.displayName)
                                put("attempts", item.attempts)
                            }
                        )
                    }
                }
            )
            put(
                "failureDetails",
                buildJsonArray {
                    failureDetails.forEach { detail ->
                        add(
                            buildJsonObject {
                                put("uri", detail.uri)
                                put("displayName", detail.displayName)
                                put("errorMessage", detail.errorMessage)
                                put("attempts", detail.attempts)
                            }
                        )
                    }
                }
            )
        }
    }

    private fun JsonObject.toCheckpoint(): ProcessingCheckpoint {
        val pendingItems = this["pendingItems"]?.jsonArrayOrNull()
            ?.mapNotNull { itemElement ->
                val item = itemElement.jsonObject
                val uri = item["uri"]?.jsonPrimitive?.contentOrNull.orEmpty()
                if (uri.isBlank()) {
                    null
                } else {
                    ProcessingPendingItem(
                        uri = uri,
                        displayName = item["displayName"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                        attempts = item["attempts"]?.jsonPrimitive?.intOrNull ?: 0
                    )
                }
            } ?: emptyList()

        val failureDetails = this["failureDetails"]?.jsonArrayOrNull()
            ?.mapNotNull { detailElement ->
                val detail = detailElement.jsonObject
                val uri = detail["uri"]?.jsonPrimitive?.contentOrNull.orEmpty()
                if (uri.isBlank()) {
                    null
                } else {
                    ProcessingFailureDetail(
                        uri = uri,
                        displayName = detail["displayName"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                        errorMessage = detail["errorMessage"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                        attempts = detail["attempts"]?.jsonPrimitive?.intOrNull ?: 1
                    )
                }
            } ?: emptyList()

        return ProcessingCheckpoint(
            taskId = this["taskId"]?.jsonPrimitive?.content ?: "",
            source = this["source"]?.jsonPrimitive?.content ?: "",
            modelName = this["modelName"]?.jsonPrimitive?.content ?: "",
            distributedMode = this["distributedMode"]?.jsonPrimitive?.booleanOrNull ?: false,
            totalItems = this["totalItems"]?.jsonPrimitive?.intOrNull ?: 0,
            completedItems = this["completedItems"]?.jsonPrimitive?.intOrNull ?: 0,
            outputDirectory = this["outputDirectory"]?.jsonPrimitive?.content ?: "",
            outputDirectoryUri = this["outputDirectoryUri"]?.jsonPrimitive?.contentOrNull,
            overwriteOriginal = this["overwriteOriginal"]?.jsonPrimitive?.booleanOrNull ?: false,
            backupOriginal = this["backupOriginal"]?.jsonPrimitive?.booleanOrNull ?: true,
            processNestedFolders = this["processNestedFolders"]?.jsonPrimitive?.booleanOrNull ?: true,
            createdAtEpochMs = this["createdAtEpochMs"]?.jsonPrimitive?.longOrNull ?: 0L,
            updatedAtEpochMs = this["updatedAtEpochMs"]?.jsonPrimitive?.longOrNull ?: 0L,
            lastErrorMessage = this["lastErrorMessage"]?.jsonPrimitive?.contentOrNull,
            pendingItems = pendingItems,
            failureDetails = failureDetails
        )
    }

    private fun kotlinx.serialization.json.JsonElement.jsonArrayOrNull(): kotlinx.serialization.json.JsonArray? {
        return runCatching { jsonArray }.getOrNull()
    }

    companion object {
        private const val CHECKPOINT_FILE_NAME = "processing_checkpoint.json"
    }
}
