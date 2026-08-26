package io.upix.data.repository

import android.content.Context
import java.io.File
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import kotlinx.coroutines.runBlocking
import org.robolectric.RuntimeEnvironment

@RunWith(RobolectricTestRunner::class)
class ProcessingQueueRepositoryTest {
    private lateinit var context: Context
    private lateinit var logRepository: LogRepository
    private lateinit var repository: ProcessingQueueRepository
    private lateinit var queueFile: File
    private lateinit var tempQueueFile: File

    @Before
    fun setUp() {
        context = RuntimeEnvironment.getApplication()
        logRepository = LogRepository()
        queueFile = File(context.filesDir, QUEUE_FILE_NAME)
        tempQueueFile = File(context.filesDir, "$QUEUE_FILE_NAME.tmp")
        queueFile.delete()
        tempQueueFile.delete()
        repository = ProcessingQueueRepository(
            context = context,
            logRepository = logRepository
        )
    }

    @After
    fun tearDown() {
        queueFile.delete()
        tempQueueFile.delete()
    }

    @Test
    fun saveQueueNormalizesItemsAndKeepsRunnableFlagWhenPendingExists() {
        runBlocking {
            repository.persistQueue(
                ProcessingQueueSnapshot(
                    items = listOf(
                        PersistedQueueItem(
                            id = "  queue-item-1  ",
                            source = "  content://images/first.jpg  ",
                            modelName = "  Real-ESRGANv3-anime-x2  ",
                            distributedMode = true,
                            retryFailedOnly = true,
                            outputDirectoryUri = "   ",
                            createdAtEpochMs = 100L,
                            expectedItems = -5,
                            status = PersistedQueueItemStatus.PENDING,
                            attempts = -2,
                            lastErrorMessage = "  "
                        ),
                        PersistedQueueItem(
                            id = "queue-item-2",
                            source = "content://images/second.jpg",
                            modelName = "   ",
                            distributedMode = false,
                            createdAtEpochMs = 200L,
                            status = PersistedQueueItemStatus.FAILED
                        )
                    ),
                    isQueueRunning = true
                )
            )
        }

        assertTrue(queueFile.exists())
        val inMemoryAfterSave = repository.queue.value
        assertEquals(1, inMemoryAfterSave.items.size)
        assertTrue(inMemoryAfterSave.isQueueRunning)

        val restored = inMemoryAfterSave.items.first()
        assertEquals("queue-item-1", restored.id)
        assertEquals("content://images/first.jpg", restored.source)
        assertEquals("Real-ESRGANv3-anime-x2", restored.modelName)
        assertEquals(PersistedQueueItemStatus.PENDING, restored.status)
        assertTrue(restored.retryFailedOnly)
        assertEquals(0, restored.expectedItems)
        assertEquals(0, restored.attempts)
        assertNull(restored.outputDirectoryUri)
        assertNull(restored.lastErrorMessage)

        val loadedAfterReload = repository.loadQueue()
        assertFalse(loadedAfterReload.isQueueRunning)
    }

    @Test
    fun loadQueueConvertsRunningItemsToFailedAfterAppRestart() {
        queueFile.writeText(
            """
            {
              "isQueueRunning": true,
              "items": [
                {
                  "id": "running-item",
                  "source": "content://images/running.jpg",
                  "modelName": "Real-ESRGANv3-anime-x2",
                  "distributedMode": false,
                  "createdAtEpochMs": 1,
                  "status": "RUNNING",
                  "attempts": 1
                }
              ]
            }
            """.trimIndent()
        )

        val restoredRepository = ProcessingQueueRepository(
            context = context,
            logRepository = logRepository
        )

        val loaded = restoredRepository.loadQueue()
        assertFalse(loaded.isQueueRunning)
        assertEquals(1, loaded.items.size)
        assertEquals(PersistedQueueItemStatus.FAILED, loaded.items.first().status)
        assertFalse(loaded.items.first().retryFailedOnly)
        assertEquals(
            "Application restarted before batch completed",
            loaded.items.first().lastErrorMessage
        )
    }

    @Test
    fun saveQueueDeletesFileWhenQueueBecomesEmpty() {
        runBlocking {
            repository.persistQueue(
                ProcessingQueueSnapshot(
                    items = listOf(
                        PersistedQueueItem(
                            id = "queue-item",
                            source = "content://images/image.jpg",
                            modelName = "Real-ESRGANv3-anime-x2",
                            distributedMode = false,
                            createdAtEpochMs = 1L
                        )
                    ),
                    isQueueRunning = false
                )
            )
        }
        assertTrue(queueFile.exists())

        runBlocking { repository.persistQueue(ProcessingQueueSnapshot()) }

        assertFalse(queueFile.exists())
        assertTrue(repository.queue.value.items.isEmpty())
        assertFalse(repository.queue.value.isQueueRunning)
    }

    @Test
    fun loadQueueReturnsEmptySnapshotOnCorruptedPayload() {
        queueFile.writeText("{invalid-json")

        val restoredRepository = ProcessingQueueRepository(
            context = context,
            logRepository = logRepository
        )
        val loaded = restoredRepository.loadQueue()

        assertTrue(loaded.items.isEmpty())
        assertFalse(loaded.isQueueRunning)
    }

    @Test
    fun multiSourceBatchRoundTripsVerbatimThroughPersistence() {
        // 相册多选合并批次：source 是 InputSourceSpec.encode 的 upix://multi:
        // 编码串（一个批次=N 张图）。持久化必须原样保留（trim 不破坏 JSON
        // payload），重载后旧/新两种格式共存。
        val multiSource = requireNotNull(
            InputSourceSpec.encode(
                listOf(
                    "content://media/external/images/media/31",
                    "content://media/external/images/media/42"
                )
            )
        )
        assertTrue(multiSource.startsWith("upix://multi:"))

        runBlocking {
            repository.persistQueue(
                ProcessingQueueSnapshot(
                    items = listOf(
                        PersistedQueueItem(
                            id = "multi-batch",
                            source = multiSource,
                            modelName = "Real-ESRGANv3-anime-x2",
                            distributedMode = false,
                            createdAtEpochMs = 1L,
                            expectedItems = 2,
                            status = PersistedQueueItemStatus.PENDING
                        )
                    ),
                    isQueueRunning = false
                )
            )
        }

        val restoredRepository = ProcessingQueueRepository(
            context = context,
            logRepository = logRepository
        )
        val loaded = restoredRepository.loadQueue()
        assertEquals(1, loaded.items.size)
        assertEquals(multiSource, loaded.items.first().source)
        assertEquals(2, loaded.items.first().expectedItems)
        assertEquals(
            listOf(
                "content://media/external/images/media/31",
                "content://media/external/images/media/42"
            ),
            InputSourceSpec.decode(loaded.items.first().source)
        )
    }

    companion object {
        private const val QUEUE_FILE_NAME = "processing_queue.json"
    }
}
