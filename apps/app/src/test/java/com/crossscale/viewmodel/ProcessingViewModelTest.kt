package com.crossscale.viewmodel

import android.net.Uri
import com.crossscale.data.model.ModelImportResult
import com.crossscale.data.model.ModelInfo
import com.crossscale.data.model.ProcessingStatus
import com.crossscale.data.repository.BatchOutputDirectory
import com.crossscale.data.repository.BatchOutputStore
import com.crossscale.data.repository.ImageProcessingSession
import com.crossscale.data.repository.ImageProcessingStore
import com.crossscale.data.repository.InputSourceResolver
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelProvider
import com.crossscale.data.repository.NativeProcessResult
import com.crossscale.data.repository.PersistedQueueItemStatus
import com.crossscale.data.repository.ProcessingCheckpoint
import com.crossscale.data.repository.ProcessingCheckpointStore
import com.crossscale.data.repository.ProcessingFailureDetail
import com.crossscale.data.repository.ProcessingPendingItem
import com.crossscale.data.repository.ProcessingQueueRepository
import com.crossscale.data.repository.ProcessingQueueSnapshot
import com.crossscale.data.repository.ProcessingQueueStore
import com.crossscale.data.repository.ResolvedImageInput
import com.crossscale.util.StringProvider
import java.io.File
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class ProcessingViewModelTest {
    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun retryFailedOnlyKeepsDeferredPendingAndTransitionsToIdle() = runTest(mainDispatcherRule.testDispatcher) {
        val failedUri = "content://images/failed.jpg"
        val deferredUri = "content://images/deferred.jpg"
        val initialCheckpoint = ProcessingCheckpoint(
            taskId = "task-1",
            source = "content://source/tree",
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            totalItems = 3,
            completedItems = 1,
            outputDirectory = "/tmp/out",
            outputDirectoryUri = null,
            createdAtEpochMs = 1L,
            updatedAtEpochMs = 1L,
            pendingItems = listOf(
                ProcessingPendingItem(uri = failedUri, displayName = "failed.jpg", attempts = 1),
                ProcessingPendingItem(uri = deferredUri, displayName = "deferred.jpg", attempts = 0)
            ),
            failureDetails = listOf(
                ProcessingFailureDetail(
                    uri = failedUri,
                    displayName = "failed.jpg",
                    errorMessage = "previous failure",
                    attempts = 1
                )
            )
        )
        val checkpointStore = FakeCheckpointStore(initialCheckpoint)
        val inputSourceResolver = FakeInputSourceResolver()
        val imageProcessingStore = FakeImageProcessingStore()

        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver,
            checkpointRepository = checkpointStore,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()

        viewModel.retryFailedItems()
        advanceUntilIdle()

        val state = viewModel.uiState.value
        val task = state.task
        assertNotNull(task)
        assertEquals(ProcessingStatus.IDLE, task?.status)
        assertEquals(2, task?.completedItems)
        assertEquals(1, task?.pendingItems)
        assertEquals(0, task?.failedItems)
        assertFalse(state.isProcessing)
        assertFalse(state.canRetryFailedOnly)
        assertEquals(1, state.checkpointSummary?.pendingItems)
        assertEquals(0, state.checkpointSummary?.failedItems)
        assertEquals(1, state.checkpointSummary?.deferredItems)

        assertEquals(listOf(failedUri), imageProcessingStore.processedUris)
        assertEquals(0, inputSourceResolver.resolveCallCount)

        val updatedCheckpoint = checkpointStore.loadCheckpoint()
        assertNotNull(updatedCheckpoint)
        assertEquals(
            listOf(deferredUri),
            updatedCheckpoint?.pendingItems?.map { item -> item.uri }
        )
        assertTrue(updatedCheckpoint?.failureDetails?.isEmpty() == true)
    }

    @Test
    fun discardCheckpointClearsResumeCapability() = runTest(mainDispatcherRule.testDispatcher) {
        val checkpointStore = FakeCheckpointStore(
            ProcessingCheckpoint(
                taskId = "task-2",
                source = "content://source/tree",
                modelName = "Real-ESRGANv3-anime-x2",
                distributedMode = false,
                totalItems = 2,
                completedItems = 1,
                outputDirectory = "/tmp/out",
                outputDirectoryUri = null,
                createdAtEpochMs = 1L,
                updatedAtEpochMs = 1L,
                pendingItems = listOf(
                    ProcessingPendingItem(
                        uri = "content://images/pending.jpg",
                        displayName = "pending.jpg",
                        attempts = 0
                    )
                )
            )
        )

        val viewModel = makeViewModel(
            checkpointRepository = checkpointStore
        )

        advanceUntilIdle()
        assertNotNull(viewModel.uiState.value.checkpointSummary)

        viewModel.discardCheckpoint()
        advanceUntilIdle()

        val state = viewModel.uiState.value
        assertFalse(state.canRetryFailedOnly)
        assertNull(state.checkpointSummary)
        assertEquals(1, checkpointStore.clearCallCount)
        assertNull(checkpointStore.loadCheckpoint())
    }

    @Test
    fun resumePendingItemOnlyProcessesSelectedPendingUri() = runTest(mainDispatcherRule.testDispatcher) {
        val firstUri = "content://images/first.jpg"
        val secondUri = "content://images/second.jpg"
        val checkpointStore = FakeCheckpointStore(
            ProcessingCheckpoint(
                taskId = "task-3",
                source = "content://source/tree",
                modelName = "Real-ESRGANv3-anime-x2",
                distributedMode = false,
                totalItems = 2,
                completedItems = 0,
                outputDirectory = "/tmp/out",
                outputDirectoryUri = null,
                createdAtEpochMs = 1L,
                updatedAtEpochMs = 1L,
                pendingItems = listOf(
                    ProcessingPendingItem(uri = firstUri, displayName = "first.jpg", attempts = 0),
                    ProcessingPendingItem(uri = secondUri, displayName = "second.jpg", attempts = 0)
                )
            )
        )
        val imageProcessingStore = FakeImageProcessingStore()
        val viewModel = makeViewModel(
            checkpointRepository = checkpointStore,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()
        viewModel.resumePendingItem(firstUri)
        advanceUntilIdle()

        val state = viewModel.uiState.value
        assertEquals(ProcessingStatus.IDLE, state.task?.status)
        assertEquals(1, state.task?.completedItems)
        assertEquals(1, state.task?.pendingItems)
        assertEquals(0, state.task?.failedItems)
        assertEquals(listOf(firstUri), imageProcessingStore.processedUris)
        val updatedCheckpoint = checkpointStore.loadCheckpoint()
        assertNotNull(updatedCheckpoint)
        assertEquals(listOf(secondUri), updatedCheckpoint?.pendingItems?.map { item -> item.uri })
        assertTrue(updatedCheckpoint?.failureDetails?.isEmpty() == true)
    }

    @Test
    fun retryFailedItemOnlyRetriesSelectedFailedUri() = runTest(mainDispatcherRule.testDispatcher) {
        val firstFailedUri = "content://images/failed-1.jpg"
        val secondFailedUri = "content://images/failed-2.jpg"
        val deferredUri = "content://images/deferred.jpg"
        val checkpointStore = FakeCheckpointStore(
            ProcessingCheckpoint(
                taskId = "task-4",
                source = "content://source/tree",
                modelName = "Real-ESRGANv3-anime-x2",
                distributedMode = false,
                totalItems = 4,
                completedItems = 1,
                outputDirectory = "/tmp/out",
                outputDirectoryUri = null,
                createdAtEpochMs = 1L,
                updatedAtEpochMs = 1L,
                pendingItems = listOf(
                    ProcessingPendingItem(uri = firstFailedUri, displayName = "failed-1.jpg", attempts = 1),
                    ProcessingPendingItem(uri = secondFailedUri, displayName = "failed-2.jpg", attempts = 2),
                    ProcessingPendingItem(uri = deferredUri, displayName = "deferred.jpg", attempts = 0)
                ),
                failureDetails = listOf(
                    ProcessingFailureDetail(
                        uri = firstFailedUri,
                        displayName = "failed-1.jpg",
                        errorMessage = "error-1",
                        attempts = 1
                    ),
                    ProcessingFailureDetail(
                        uri = secondFailedUri,
                        displayName = "failed-2.jpg",
                        errorMessage = "error-2",
                        attempts = 2
                    )
                )
            )
        )
        val imageProcessingStore = FakeImageProcessingStore()
        val viewModel = makeViewModel(
            checkpointRepository = checkpointStore,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()
        viewModel.retryFailedItem(firstFailedUri)
        advanceUntilIdle()

        val state = viewModel.uiState.value
        assertEquals(ProcessingStatus.FAILED, state.task?.status)
        assertEquals(2, state.task?.completedItems)
        assertEquals(2, state.task?.pendingItems)
        assertEquals(1, state.task?.failedItems)
        assertEquals(listOf(firstFailedUri), imageProcessingStore.processedUris)
        val updatedCheckpoint = checkpointStore.loadCheckpoint()
        assertNotNull(updatedCheckpoint)
        assertEquals(
            listOf(secondFailedUri, deferredUri),
            updatedCheckpoint?.pendingItems?.map { item -> item.uri }
        )
        assertEquals(
            listOf(secondFailedUri),
            updatedCheckpoint?.failureDetails?.map { detail -> detail.uri }
        )
    }

    @Test
    fun enqueueBatchDeduplicatesPendingBatchWithSameConfiguration() = runTest(mainDispatcherRule.testDispatcher) {
        val source = "content://source/dedup"
        val inputUri = "content://images/dedup.jpg"
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(source, listOf(inputUri))
        }

        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver
        )

        advanceUntilIdle()
        val firstBatchId = viewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        val secondBatchId = viewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()

        assertNotNull(firstBatchId)
        assertEquals(firstBatchId, secondBatchId)

        val queueItems = viewModel.uiState.value.queueItems
        assertEquals(1, queueItems.size)
        assertEquals(firstBatchId, queueItems.first().id)
        assertEquals(PersistedQueueItemStatus.PENDING, queueItems.first().status)
        assertEquals(1, queueItems.first().expectedItems)
    }

    @Test
    fun batchQueueProcessesPendingBatchesSequentially() = runTest(mainDispatcherRule.testDispatcher) {
        val sourceA = "content://source/a"
        val sourceB = "content://source/b"
        val uriA = "content://images/a.jpg"
        val uriB = "content://images/b.jpg"
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(sourceA, listOf(uriA))
            register(sourceB, listOf(uriB))
        }
        val imageProcessingStore = FakeImageProcessingStore()
        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()
        viewModel.enqueueBatch(
            imagePath = sourceA,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        viewModel.enqueueBatch(
            imagePath = sourceB,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()

        viewModel.startBatchQueue()
        advanceUntilIdle()

        assertEquals(listOf(uriA, uriB), imageProcessingStore.processedUris)
        val finalState = viewModel.uiState.value
        assertEquals(2, finalState.queueItems.size)
        assertEquals(
            listOf(PersistedQueueItemStatus.COMPLETED, PersistedQueueItemStatus.COMPLETED),
            finalState.queueItems.map { item -> item.status }
        )
        assertFalse(finalState.isQueueRunning)
        assertEquals(ProcessingStatus.COMPLETED, finalState.task?.status)
    }

    @Test
    fun batchQueueStopsOnFailureAndAllowsRetry() = runTest(mainDispatcherRule.testDispatcher) {
        val sourceA = "content://source/fail-first"
        val sourceB = "content://source/second"
        val failedUri = "content://images/fail.jpg"
        val secondUri = "content://images/ok.jpg"
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(sourceA, listOf(failedUri))
            register(sourceB, listOf(secondUri))
        }
        val imageProcessingStore = FakeImageProcessingStore().apply {
            failingUris += failedUri
        }
        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()
        viewModel.enqueueBatch(
            imagePath = sourceA,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        viewModel.enqueueBatch(
            imagePath = sourceB,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()

        viewModel.startBatchQueue()
        advanceUntilIdle()

        val stateAfterFailure = viewModel.uiState.value
        assertFalse(stateAfterFailure.isQueueRunning)
        // 失败的批次不再被移除：保留为 FAILED 行，供 QueueSheet 重试。
        assertEquals(2, stateAfterFailure.queueItems.size)
        val failedItem = stateAfterFailure.queueItems.first { item -> item.source == sourceA }
        assertEquals(PersistedQueueItemStatus.FAILED, failedItem.status)
        assertNotNull(failedItem.lastErrorMessage)
        val pendingItem = stateAfterFailure.queueItems.first { item -> item.source == sourceB }
        assertEquals(PersistedQueueItemStatus.PENDING, pendingItem.status)
        assertEquals(ProcessingStatus.FAILED, stateAfterFailure.task?.status)

        imageProcessingStore.failingUris.clear()
        viewModel.startBatchQueue()
        advanceUntilIdle()

        // 重启队列只跑 PENDING 批次（B）；FAILED 的 A 需要显式 retry。
        assertEquals(
            listOf(failedUri, secondUri),
            imageProcessingStore.processedUris
        )
        val stateAfterRetry = viewModel.uiState.value
        assertEquals(2, stateAfterRetry.queueItems.size)
        assertEquals(
            PersistedQueueItemStatus.FAILED,
            stateAfterRetry.queueItems.first { item -> item.source == sourceA }.status
        )
        assertEquals(
            PersistedQueueItemStatus.COMPLETED,
            stateAfterRetry.queueItems.first { item -> item.source == sourceB }.status
        )
        assertFalse(stateAfterRetry.isQueueRunning)
        assertEquals(ProcessingStatus.COMPLETED, stateAfterRetry.task?.status)
    }

    @Test
    fun cancelQueuedRunKeepsRowAsFailedWithMessage() = runTest(mainDispatcherRule.testDispatcher) {
        val source = "content://source/cancel"
        val uri = "content://images/cancel.jpg"
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(source, listOf(uri))
        }
        val processLatch = java.util.concurrent.CountDownLatch(1)
        val imageProcessingStore = FakeImageProcessingStore().apply {
            processBlocker = { processLatch.await() }
        }
        // 处理放到真实 Default 线程并阻塞在闩上，制造可取消的运行中状态。
        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver,
            nativeImageProcessingRepository = imageProcessingStore,
            defaultDispatcher = Dispatchers.Default
        )

        advanceUntilIdle()
        viewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()
        viewModel.startBatchQueue()
        advanceUntilIdle()
        assertEquals(PersistedQueueItemStatus.RUNNING, viewModel.uiState.value.queueItems.first().status)
        assertTrue(viewModel.uiState.value.isProcessing)

        // 等处理真正开始（Default 线程阻塞在闩上），再取消并放行。
        waitForTrue { imageProcessingStore.processingEntered }
        viewModel.cancelProcessing()
        processLatch.countDown()
        // 收尾链路在真实 Default 线程与测试调度器之间往返，advanceUntilIdle 单发
        // 会与异步恢复竞争：轮询直到行离开 RUNNING 再断言。
        waitForTrue(pump = { advanceUntilIdle() }) {
            viewModel.uiState.value.queueItems.first().status !=
                PersistedQueueItemStatus.RUNNING && !viewModel.uiState.value.isProcessing
        }

        val state = viewModel.uiState.value
        // 取消的批次保留为 FAILED 行（不再被移除），队列停止。
        assertEquals(1, state.queueItems.size)
        assertEquals(PersistedQueueItemStatus.FAILED, state.queueItems.first().status)
        assertNotNull(state.queueItems.first().lastErrorMessage)
        assertFalse(state.isQueueRunning)
        assertEquals(ProcessingStatus.CANCELED, state.task?.status)
        assertFalse(state.isProcessing)
    }

    @Test
    fun failedOnlyQueueRetryUsesCheckpointFailedUrisOnly() = runTest(mainDispatcherRule.testDispatcher) {
        val source = "content://source/queue-failed-only"
        val failedUri = "content://images/failed.jpg"
        val deferredUri = "content://images/deferred.jpg"
        val checkpointStore = FakeCheckpointStore(
            ProcessingCheckpoint(
                taskId = "checkpoint-failed-only",
                source = source,
                modelName = "Real-ESRGANv3-anime-x2",
                distributedMode = false,
                totalItems = 3,
                completedItems = 1,
                outputDirectory = "/tmp/out",
                outputDirectoryUri = null,
                createdAtEpochMs = 1L,
                updatedAtEpochMs = 1L,
                pendingItems = listOf(
                    ProcessingPendingItem(uri = failedUri, displayName = "failed.jpg", attempts = 2),
                    ProcessingPendingItem(uri = deferredUri, displayName = "deferred.jpg", attempts = 0)
                ),
                failureDetails = listOf(
                    ProcessingFailureDetail(
                        uri = failedUri,
                        displayName = "failed.jpg",
                        errorMessage = "previous failure",
                        attempts = 2
                    )
                )
            )
        )
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(source, listOf("content://images/ignored.jpg"))
        }
        val imageProcessingStore = FakeImageProcessingStore()
        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver,
            checkpointRepository = checkpointStore,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()
        viewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()

        val queueItemId = viewModel.uiState.value.queueItems.first().id
        viewModel.retryQueuedBatchFailedOnly(queueItemId)
        advanceUntilIdle()

        val configuredItem = viewModel.uiState.value.queueItems.first()
        assertTrue(configuredItem.retryFailedOnly)
        assertEquals(PersistedQueueItemStatus.PENDING, configuredItem.status)
        inputSourceResolver.resetCallCount()

        viewModel.startBatchQueue()
        advanceUntilIdle()

        // 失败项重试成功后任务进入 IDLE：批次行回到 PENDING 并被队列链式推进，
        // 剩余的延迟项随后也跑完（批次最终 COMPLETED，不静默滞留工作）。
        assertEquals(listOf(failedUri, deferredUri), imageProcessingStore.processedUris)
        assertEquals(0, inputSourceResolver.resolveCallCount)

        val finalState = viewModel.uiState.value
        assertEquals(1, finalState.queueItems.size)
        assertEquals(PersistedQueueItemStatus.COMPLETED, finalState.queueItems.first().status)
        assertEquals(ProcessingStatus.COMPLETED, finalState.task?.status)
        assertFalse(finalState.isQueueRunning)

        val updatedCheckpoint = checkpointStore.loadCheckpoint()
        assertNull(updatedCheckpoint)
    }

    @Test
    fun queuedBatchUsesMatchingCheckpointForResume() = runTest(mainDispatcherRule.testDispatcher) {
        val source = "content://source/queue-resume"
        val pendingUriA = "content://images/pending-a.jpg"
        val pendingUriB = "content://images/pending-b.jpg"
        val checkpointStore = FakeCheckpointStore(
            ProcessingCheckpoint(
                taskId = "checkpoint-queue-resume",
                source = source,
                modelName = "Real-ESRGANv3-anime-x2",
                distributedMode = false,
                totalItems = 3,
                completedItems = 1,
                outputDirectory = "/tmp/out",
                outputDirectoryUri = null,
                createdAtEpochMs = 1L,
                updatedAtEpochMs = 1L,
                pendingItems = listOf(
                    ProcessingPendingItem(uri = pendingUriA, displayName = "pending-a.jpg", attempts = 0),
                    ProcessingPendingItem(uri = pendingUriB, displayName = "pending-b.jpg", attempts = 0)
                )
            )
        )
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(source, listOf("content://images/ignored.jpg"))
        }
        val imageProcessingStore = FakeImageProcessingStore()

        val viewModel = makeViewModel(
            inputSourceRepository = inputSourceResolver,
            checkpointRepository = checkpointStore,
            nativeImageProcessingRepository = imageProcessingStore
        )

        advanceUntilIdle()
        viewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()
        inputSourceResolver.resetCallCount()

        viewModel.startBatchQueue()
        advanceUntilIdle()

        assertEquals(listOf(pendingUriA, pendingUriB), imageProcessingStore.processedUris)
        assertEquals(0, inputSourceResolver.resolveCallCount)
        val finalState = viewModel.uiState.value
        assertEquals(1, finalState.queueItems.size)
        assertEquals(PersistedQueueItemStatus.COMPLETED, finalState.queueItems.first().status)
        assertEquals(ProcessingStatus.COMPLETED, finalState.task?.status)
    }

    @Test
    fun queueStatePersistsAcrossViewModelRecreation() = runTest(mainDispatcherRule.testDispatcher) {
        val source = "content://source/persist"
        val uri = "content://images/persist.jpg"
        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(source, listOf(uri))
        }
        val initialQueueStore = FakeQueueStore()
        val modelProvider = FakeModelProvider(testModel())

        val firstViewModel = makeViewModel(
            modelRepository = modelProvider,
            inputSourceRepository = inputSourceResolver,
            queueRepository = initialQueueStore
        )

        advanceUntilIdle()
        firstViewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()

        val persistedSnapshot = initialQueueStore.loadQueue()
        assertEquals(1, persistedSnapshot.items.size)
        assertFalse(persistedSnapshot.isQueueRunning)
        assertEquals(PersistedQueueItemStatus.PENDING, persistedSnapshot.items.first().status)

        val restoredQueueStore = FakeQueueStore(initialSnapshot = persistedSnapshot)
        val restoredViewModel = makeViewModel(
            modelRepository = modelProvider,
            inputSourceRepository = inputSourceResolver,
            queueRepository = restoredQueueStore
        )

        advanceUntilIdle()
        val restoredState = restoredViewModel.uiState.value
        assertEquals(1, restoredState.queueItems.size)
        assertEquals(source, restoredState.queueItems.first().source)
        assertEquals(1, restoredState.queueItems.first().expectedItems)
        assertFalse(restoredState.isQueueRunning)
    }

    @Test
    fun queueStatePersistsAcrossViewModelRecreationWithDiskRepository() = runTest(mainDispatcherRule.testDispatcher) {
        val source = "content://source/disk"
        val uri = "content://images/disk.jpg"
        val context = RuntimeEnvironment.getApplication()
        val queueFile = File(context.filesDir, "processing_queue.json")
        queueFile.delete()

        val inputSourceResolver = FakeInputSourceResolver().apply {
            register(source, listOf(uri))
        }
        val modelProvider = FakeModelProvider(testModel())
        val queueStore = ProcessingQueueRepository(context, LogRepository())
        val firstViewModel = makeViewModel(
            modelRepository = modelProvider,
            inputSourceRepository = inputSourceResolver,
            queueRepository = queueStore
        )

        advanceUntilIdle()
        firstViewModel.enqueueBatch(
            imagePath = source,
            modelName = "Real-ESRGANv3-anime-x2",
            distributedMode = false,
            outputDirectoryUri = null
        )
        advanceUntilIdle()
        // persistQueue writes on Dispatchers.IO, which advanceUntilIdle does not
        // cover — poll the real filesystem until the estimate lands. The first
        // write persists expectedItems=null; a second write follows once
        // estimateQueuedBatchExpectedItems resolves the source.
        waitForTrue(pump = { advanceUntilIdle() }) {
            queueFile.exists() &&
                queueStore.loadQueue().items.firstOrNull()?.expectedItems == 1
        }

        val restoredQueueStore = ProcessingQueueRepository(context, LogRepository())
        val restoredViewModel = makeViewModel(
            modelRepository = modelProvider,
            inputSourceRepository = inputSourceResolver,
            queueRepository = restoredQueueStore
        )

        advanceUntilIdle()
        val restoredState = restoredViewModel.uiState.value
        assertEquals(1, restoredState.queueItems.size)
        assertEquals(source, restoredState.queueItems.first().source)
        assertEquals(1, restoredState.queueItems.first().expectedItems)
        assertFalse(restoredState.isQueueRunning)
        assertEquals(PersistedQueueItemStatus.PENDING, restoredQueueStore.loadQueue().items.first().status)

        queueFile.delete()
    }

    // Blocks the test thread until condition holds; repository writes run on
    // Dispatchers.IO (a real thread pool under Robolectric), invisible to the
    // TestDispatcher that advanceUntilIdle drains. The chain estimate → state
    // → persist also hops back onto the test scheduler, so each poll must pump
    // it while yielding to the real IO threads.
    private fun waitForTrue(
        timeoutMs: Long = 5_000,
        pump: (() -> Unit)? = null,
        condition: () -> Boolean
    ) {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (!condition()) {
            check(System.currentTimeMillis() < deadline) {
                "condition not met within ${timeoutMs}ms; queue file: ${dumpQueueFile()}"
            }
            pump?.invoke()
            Thread.sleep(10)
        }
    }

    private fun dumpQueueFile(): String {
        val context = RuntimeEnvironment.getApplication()
        val queueFile = File(context.filesDir, "processing_queue.json")
        return if (queueFile.exists()) queueFile.readText() else "<missing>"
    }

    private fun testModel() = ModelInfo(
        name = "Real-ESRGANv3-anime-x2",
        scaleFactor = 2,
        totalSize = 1024L,
        paramPath = "/models/Real-ESRGANv3-anime-x2.param",
        binPath = "/models/Real-ESRGANv3-anime-x2.bin"
    )

    private fun makeViewModel(
        modelRepository: ModelProvider = FakeModelProvider(testModel()),
        inputSourceRepository: InputSourceResolver = FakeInputSourceResolver(),
        checkpointRepository: ProcessingCheckpointStore = FakeCheckpointStore(null),
        queueRepository: ProcessingQueueStore = FakeQueueStore(),
        nativeImageProcessingRepository: ImageProcessingStore = FakeImageProcessingStore(),
        defaultDispatcher: CoroutineDispatcher = mainDispatcherRule.testDispatcher
    ): ProcessingViewModel = ProcessingViewModel(
        logRepository = LogRepository(),
        modelRepository = modelRepository,
        inputSourceRepository = inputSourceRepository,
        batchOutputRepository = FakeBatchOutputStore(),
        checkpointRepository = checkpointRepository,
        queueRepository = queueRepository,
        nativeImageProcessingRepository = nativeImageProcessingRepository,
        ioDispatcher = mainDispatcherRule.testDispatcher,
        defaultDispatcher = defaultDispatcher,
        stringProvider = StringProvider { _, _ -> "" }
    )

    private class FakeModelProvider(
        private val modelInfo: ModelInfo
    ) : ModelProvider {
        private val _models = MutableStateFlow(listOf(modelInfo))
        override val models: StateFlow<List<ModelInfo>> = _models.asStateFlow()

        override fun getModelByName(name: String): ModelInfo? {
            return if (modelInfo.name == name) {
                modelInfo
            } else {
                null
            }
        }

        override fun modelDirectoryPath(): String = "/models"

        override suspend fun refreshModels() {
            _models.value = listOf(modelInfo)
        }

        override suspend fun importModels(inputUris: List<Uri>): ModelImportResult =
            ModelImportResult()

        override suspend fun deleteModel(modelInfo: ModelInfo) {
            _models.value = emptyList()
        }
    }

    private class FakeInputSourceResolver : InputSourceResolver {
        private val sourceMap = mutableMapOf<String, List<ResolvedImageInput>>()
        var resolveCallCount: Int = 0
            private set

        fun register(source: String, uris: List<String>) {
            sourceMap[source] = uris.map { uri ->
                ResolvedImageInput(
                    uri = android.net.Uri.parse(uri),
                    displayName = uri.substringAfterLast('/', uri)
                )
            }
        }

        override fun resolveImageInputs(
            source: String,
            nested: Boolean
        ): List<ResolvedImageInput> {
            resolveCallCount += 1
            return sourceMap[source].orEmpty()
        }

        fun resetCallCount() {
            resolveCallCount = 0
        }
    }

    private class FakeBatchOutputStore : BatchOutputStore {
        private val outputRoot = File("build/test-outputs/viewmodel").also { root ->
            if (!root.exists()) {
                root.mkdirs()
            }
        }

        override fun prepareOutputDirectory(
            source: String,
            modelName: String,
            preferredTreeUri: String?,
            overwriteOriginal: Boolean,
            backupOriginal: Boolean,
            processNestedFolders: Boolean,
            resumeOutputDirectory: String?,
            resumeOutputDirectoryUri: String?,
            resumeOverwriteOriginal: Boolean?,
            resumeBackupOriginal: Boolean?
        ): BatchOutputDirectory {
            val outputDir = File(outputRoot, "batch").also { dir ->
                if (!dir.exists()) {
                    dir.mkdirs()
                }
            }
            return BatchOutputDirectory(
                batchId = "batch",
                displayPath = outputDir.absolutePath,
                outputDirectoryUri = null,
                internalDirectory = outputDir
            )
        }

        override fun outputFileFor(
            outputDirectory: BatchOutputDirectory,
            input: ResolvedImageInput
        ): File {
            val safeName = input.displayName.ifBlank { "image" }.replace(Regex("[^A-Za-z0-9._-]"), "_")
            val outputDir = requireNotNull(outputDirectory.internalDirectory)
            return File(outputDir, "$safeName.png")
        }

        override fun isOutputReady(
            outputDirectory: BatchOutputDirectory,
            input: ResolvedImageInput
        ): Boolean {
            return false
        }

        override fun firstReadyOutputUri(
            outputDirectory: BatchOutputDirectory,
            inputs: List<ResolvedImageInput>
        ): String? {
            return null
        }

        override fun hasBackup(input: ResolvedImageInput): Boolean = false

        override fun restoreBackup(input: ResolvedImageInput): Result<Unit> = Result.success(Unit)

        override fun commitOutput(
            outputDirectory: BatchOutputDirectory,
            input: ResolvedImageInput,
            outputFile: File
        ): Result<String> {
            if (!outputFile.exists()) {
                outputFile.parentFile?.mkdirs()
                outputFile.writeBytes(byteArrayOf(0x01))
            }
            return Result.success(outputFile.absolutePath)
        }

        override fun discardOutputFile(outputFile: File) {
            if (outputFile.exists()) {
                outputFile.delete()
            }
        }
    }

    private class FakeCheckpointStore(
        checkpoint: ProcessingCheckpoint?
    ) : ProcessingCheckpointStore {
        private val state = MutableStateFlow(checkpoint)
        var clearCallCount: Int = 0
            private set

        override val checkpoint: StateFlow<ProcessingCheckpoint?>
            get() = state.asStateFlow()

        override suspend fun persistCheckpoint(checkpoint: ProcessingCheckpoint) {
            state.value = checkpoint
        }

        override suspend fun loadCheckpoint(): ProcessingCheckpoint? {
            return state.value
        }

        override suspend fun clearCheckpoint() {
            clearCallCount += 1
            state.value = null
        }
    }

    private class FakeQueueStore(
        initialSnapshot: ProcessingQueueSnapshot = ProcessingQueueSnapshot()
    ) : ProcessingQueueStore {
        private val state = MutableStateFlow(initialSnapshot)

        override val queue: StateFlow<ProcessingQueueSnapshot>
            get() = state.asStateFlow()

        override suspend fun persistQueue(snapshot: ProcessingQueueSnapshot) {
            state.value = snapshot
        }

        override fun loadQueue(): ProcessingQueueSnapshot {
            return state.value
        }
    }

    private class FakeImageProcessingStore : ImageProcessingStore {
        val processedUris = mutableListOf<String>()
        val failingUris = mutableSetOf<String>()

        // 取消测试用：处理阻塞在闩上制造可取消的“运行中”窗口。
        var processBlocker: (() -> Unit)? = null

        @Volatile
        var processingEntered = false
            private set

        override fun openSession(
            modelName: String,
            modelBasePath: String,
            preferGpu: Boolean
        ): ImageProcessingSession {
            return object : ImageProcessingSession {
                override fun process(
                    input: ResolvedImageInput,
                    outputFile: File
                ): Result<NativeProcessResult> {
                    val uri = input.uri.toString()
                    processingEntered = true
                    processBlocker?.invoke()
                    processedUris += uri
                    if (uri in failingUris) {
                        return Result.failure(IllegalStateException("mock failure for $uri"))
                    }
                    outputFile.parentFile?.mkdirs()
                    outputFile.writeBytes(byteArrayOf(0x02))
                    return Result.success(
                        NativeProcessResult(
                            outputFile = outputFile,
                            inputWidth = 10,
                            inputHeight = 10,
                            outputWidth = 20,
                            outputHeight = 20
                        )
                    )
                }

                override fun close() {
                }
            }
        }
    }

}
