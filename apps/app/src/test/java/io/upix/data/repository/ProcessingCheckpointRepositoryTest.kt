package io.upix.data.repository

import android.content.Context
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment

@RunWith(RobolectricTestRunner::class)
class ProcessingCheckpointRepositoryTest {
    private lateinit var context: Context
    private lateinit var checkpointFile: File
    private lateinit var stagingFile: File
    private lateinit var backupFile: File
    private lateinit var repository: ProcessingCheckpointRepository

    @Before
    fun setUp() {
        context = RuntimeEnvironment.getApplication()
        checkpointFile = File(context.filesDir, CHECKPOINT_FILE_NAME)
        stagingFile = File(context.filesDir, "$CHECKPOINT_FILE_NAME.new")
        backupFile = File(context.filesDir, "$CHECKPOINT_FILE_NAME.bak")
        checkpointFile.delete()
        stagingFile.deleteRecursively()
        backupFile.delete()
        repository = ProcessingCheckpointRepository(context, LogRepository(), Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        checkpointFile.delete()
        stagingFile.deleteRecursively()
        backupFile.delete()
    }

    @Test
    fun queueIdentityRoundTripsAndLegacyCheckpointDefaultsToNull() = runBlocking {
        val queuedCheckpoint = checkpoint(queueItemId = "queue-row-7")
        repository.persistCheckpoint(queuedCheckpoint)
        assertEquals("queue-row-7", repository.loadCheckpoint()?.queueItemId)

        checkpointFile.writeText(
            """{"taskId":"legacy-task","source":"content://source/a","modelName":"model","distributedMode":false,"totalItems":1,"completedItems":0,"outputDirectory":"/out","createdAtEpochMs":1,"updatedAtEpochMs":2}"""
        )
        assertNull(repository.loadCheckpoint()?.queueItemId)
    }

    @Test
    fun failedCheckpointWriteKeepsDiskAndObservableCheckpointUnchanged() = runBlocking {
        val original = checkpoint(queueItemId = null)
        repository.persistCheckpoint(original)
        val committedPayload = checkpointFile.readText()
        assertTrue(stagingFile.mkdir())

        val failure = runCatching {
            repository.persistCheckpoint(original.copy(taskId = "replacement-task"))
        }.exceptionOrNull()

        assertTrue(failure != null)
        assertEquals(committedPayload, checkpointFile.readText())
        assertEquals(original, repository.checkpoint.value)
    }

    @Test
    fun clearingCheckpointRemovesPrimaryAndRecoveryFiles() = runBlocking {
        repository.persistCheckpoint(checkpoint(queueItemId = "queue-row"))
        backupFile.writeText("stale backup")
        stagingFile.writeText("stale staging")

        repository.clearCheckpoint()

        assertNull(repository.checkpoint.value)
        assertFalse(checkpointFile.exists())
        assertFalse(backupFile.exists())
        assertFalse(stagingFile.exists())
    }

    private fun checkpoint(queueItemId: String?) = ProcessingCheckpoint(
        taskId = "task-1",
        source = "content://source/a",
        modelName = "model",
        distributedMode = false,
        totalItems = 1,
        completedItems = 0,
        outputDirectory = "/out",
        createdAtEpochMs = 1L,
        updatedAtEpochMs = 2L,
        queueItemId = queueItemId
    )

    companion object {
        private const val CHECKPOINT_FILE_NAME = "processing_checkpoint.json"
    }
}
