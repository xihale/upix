package io.upix.ui.input

import org.junit.Assert.assertEquals
import org.junit.Test

/** 模型就绪补入队的候选源筛选（见 InputSelectionController.onSelectedModelNameChanged）。 */
class PendingEnqueueSourcesTest {

    @Test
    fun keepsUncorruptedUnqueuedSourcesInOrder() {
        val sources = listOf("content://a/1.jpg", "content://tree/2", "content://b/3.jpg")
        assertEquals(
            sources,
            pendingEnqueueSources(
                selectedSources = sources,
                corruptedSources = emptySet(),
                queuedSources = emptySet()
            )
        )
    }

    @Test
    fun dropsCorruptedSources() {
        assertEquals(
            listOf("content://a/1.jpg"),
            pendingEnqueueSources(
                selectedSources = listOf("content://a/1.jpg", "content://bad/corrupt.jpg"),
                corruptedSources = setOf("content://bad/corrupt.jpg"),
                queuedSources = emptySet()
            )
        )
    }

    @Test
    fun dropsAlreadyQueuedSources() {
        assertEquals(
            emptyList<String>(),
            pendingEnqueueSources(
                selectedSources = listOf("content://tree/folder"),
                corruptedSources = emptySet(),
                queuedSources = setOf("content://tree/folder")
            )
        )
    }

    @Test
    fun emptySelectionYieldsEmpty() {
        assertEquals(
            emptyList<String>(),
            pendingEnqueueSources(
                selectedSources = emptyList(),
                corruptedSources = setOf("x"),
                queuedSources = setOf("y")
            )
        )
    }
}
